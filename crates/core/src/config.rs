//! Firmius configuration: the umbrella `~/.firmius/config.json` file and the
//! typed settings it holds. This is the first general configuration mechanism
//! in the codebase (distinct from `UserSettings`, which stores per-persona
//! model preferences).
//!
//! Design goals:
//! - Forward compatible: every field has a `#[serde(default)]`, so an old file
//!   loads under a newer binary and a newer file degrades gracefully under an
//!   older one. Unknown keys are ignored.
//! - Layered retry policy: a `default` [`RetryConfig`] plus per-provider
//!   overrides that *merge* onto the default (see [`RetrySettings::resolve`]).
//!   Overrides are keyed by provider id or account-kind name, so a user can
//!   tune, say, `anthropic` without touching `openai`.
//! - Durations are stored as explicit millisecond integers (`*_ms`) so the
//!   JSON is unambiguous and editable by hand.
//! - Atomic, private saves, matching the account/session persistence style.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::PathBuf;

pub const CONFIG_VERSION: u32 = 1;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

#[derive(Debug, thiserror::Error)]
pub enum ConfigError {
    #[error("unable to resolve home directory for ~/.firmius/config.json")]
    HomeDirUnavailable,
    #[error("config I/O error at {path}: {source}")]
    Io { path: PathBuf, source: io::Error },
    #[error("config JSON error at {path}: {source}")]
    Json {
        path: PathBuf,
        source: serde_json::Error,
    },
}

// ---------------------------------------------------------------------------
// Backoff
// ---------------------------------------------------------------------------

/// How successive retry delays grow.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BackoffStrategy {
    /// Same delay every attempt (`initial_ms`).
    Fixed,
    /// `initial_ms * multiplier^(attempt-1)`, capped at `max_delay_ms`.
    Exponential,
    /// `initial_ms * attempt`, capped at `max_delay_ms`.
    Linear,
}

impl Default for BackoffStrategy {
    fn default() -> Self {
        Self::Exponential
    }
}

/// Backoff timing for one retry loop. Delays are computed per attempt and then
/// perturbed by up to `jitter` (a 0.0..=1.0 fraction) to avoid thundering-herd
/// retries when several agents fail at once.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BackoffConfig {
    #[serde(default)]
    pub strategy: BackoffStrategy,
    /// Delay before the first retry (and the fixed delay for `Fixed`).
    #[serde(default = "default_initial_ms")]
    pub initial_ms: u64,
    /// Growth factor for `Exponential`. Ignored by other strategies.
    #[serde(default = "default_multiplier")]
    pub multiplier: f64,
    /// Upper bound on any single delay after growth.
    #[serde(default = "default_max_delay_ms")]
    pub max_delay_ms: u64,
    /// Random spread as a fraction of the computed delay, `0.0..=1.0`.
    /// `0.2` means "up to 20% shorter or longer".
    #[serde(default = "default_jitter")]
    pub jitter: f64,
}

fn default_initial_ms() -> u64 {
    500
}
fn default_multiplier() -> f64 {
    2.0
}
fn default_max_delay_ms() -> u64 {
    30_000
}
fn default_jitter() -> f64 {
    0.2
}

impl Default for BackoffConfig {
    fn default() -> Self {
        Self {
            strategy: BackoffStrategy::default(),
            initial_ms: default_initial_ms(),
            multiplier: default_multiplier(),
            max_delay_ms: default_max_delay_ms(),
            jitter: default_jitter(),
        }
    }
}

impl BackoffConfig {
    /// The un-jittered base delay for a 1-based `attempt` number. The retry
    /// engine applies jitter on top of this; keeping the base pure makes the
    /// growth curve trivially testable.
    pub fn base_delay_ms(&self, attempt: u32) -> u64 {
        let attempt = attempt.max(1);
        let raw = match self.strategy {
            BackoffStrategy::Fixed => self.initial_ms as f64,
            BackoffStrategy::Linear => self.initial_ms as f64 * attempt as f64,
            BackoffStrategy::Exponential => {
                self.initial_ms as f64 * self.multiplier.powi((attempt - 1) as i32)
            }
        };
        raw.min(self.max_delay_ms as f64).max(0.0) as u64
    }
}

// ---------------------------------------------------------------------------
// Failure-class toggles
// ---------------------------------------------------------------------------

/// Which failure classes participate in a given action (retrying, or switching
/// accounts). Each flag defaults to a sensible value so an omitted block still
/// behaves well.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct FailureClasses {
    #[serde(default = "default_true")]
    pub rate_limited: bool,
    #[serde(default = "default_true")]
    pub server_error: bool,
    #[serde(default = "default_true")]
    pub transport: bool,
    #[serde(default = "default_true")]
    pub decode: bool,
    /// Auth failures: usually worth an account switch (a bad/expired key),
    /// rarely worth retrying the same account.
    #[serde(default)]
    pub auth: bool,
}

fn default_true() -> bool {
    true
}

impl FailureClasses {
    /// Defaults tuned for *retrying the same account*: transient classes yes,
    /// auth no (a bad key will not fix itself).
    pub fn retry_defaults() -> Self {
        Self {
            rate_limited: true,
            server_error: true,
            transport: true,
            decode: true,
            auth: false,
        }
    }

    /// Defaults tuned for *switching accounts*: a different account can dodge
    /// a per-account rate limit or a bad key, so auth is enabled here.
    pub fn switch_defaults() -> Self {
        Self {
            rate_limited: true,
            server_error: true,
            transport: true,
            decode: false,
            auth: true,
        }
    }
}

// ---------------------------------------------------------------------------
// Retry config (one resolved policy)
// ---------------------------------------------------------------------------

/// A complete, resolved retry policy for one provider. Fields are `Option` in
/// the *override* representation (`RetryOverride`) but concrete here: this is
/// what the engine consumes after merging.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RetryConfig {
    /// Master switch. When false, the engine returns the first error untouched.
    #[serde(default = "default_true")]
    pub enabled: bool,
    /// Attempts *per account* before giving up on it. `1` means the initial
    /// try only, no same-account retries. Total tries on one account is this
    /// value (initial attempt counts).
    #[serde(default = "default_max_attempts")]
    pub max_attempts_per_account: u32,
    /// Whether to move on to sibling accounts once an account is exhausted.
    #[serde(default = "default_true")]
    pub account_switching: bool,
    /// Cap on how many distinct accounts to try in one turn (including the
    /// starting one). `0` means "no cap, use every available account".
    #[serde(default)]
    pub max_accounts: u32,
    /// Overall wall-clock budget for the whole retry loop, in milliseconds.
    /// `0` disables the budget. Prevents an unbounded backoff from stalling a
    /// turn indefinitely.
    #[serde(default)]
    pub max_elapsed_ms: u64,
    /// Honor a server-provided `Retry-After` / rate-limit reset hint instead of
    /// the computed backoff when the server's hint is longer.
    #[serde(default = "default_true")]
    pub respect_retry_after: bool,
    #[serde(default)]
    pub backoff: BackoffConfig,
    /// Failure classes that trigger a same-account retry.
    #[serde(default = "FailureClasses::retry_defaults")]
    pub retry_on: FailureClasses,
    /// Failure classes that trigger an account switch.
    #[serde(default = "FailureClasses::switch_defaults")]
    pub switch_on: FailureClasses,
}

fn default_max_attempts() -> u32 {
    3
}

impl Default for RetryConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            max_attempts_per_account: default_max_attempts(),
            account_switching: true,
            max_accounts: 0,
            max_elapsed_ms: 0,
            respect_retry_after: true,
            backoff: BackoffConfig::default(),
            retry_on: FailureClasses::retry_defaults(),
            switch_on: FailureClasses::switch_defaults(),
        }
    }
}

// ---------------------------------------------------------------------------
// Retry overrides (sparse, merge onto default)
// ---------------------------------------------------------------------------

/// A sparse per-provider override. Every field is optional; `None` means
/// "inherit the default". Applied with [`RetryConfig::with_override`].
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct RetryOverride {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_attempts_per_account: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub account_switching: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_accounts: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_elapsed_ms: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub respect_retry_after: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub backoff: Option<BackoffConfig>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub retry_on: Option<FailureClasses>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub switch_on: Option<FailureClasses>,
}

impl RetryConfig {
    /// Merge a sparse override onto this config, returning the resolved policy.
    pub fn with_override(&self, over: &RetryOverride) -> RetryConfig {
        RetryConfig {
            enabled: over.enabled.unwrap_or(self.enabled),
            max_attempts_per_account: over
                .max_attempts_per_account
                .unwrap_or(self.max_attempts_per_account),
            account_switching: over.account_switching.unwrap_or(self.account_switching),
            max_accounts: over.max_accounts.unwrap_or(self.max_accounts),
            max_elapsed_ms: over.max_elapsed_ms.unwrap_or(self.max_elapsed_ms),
            respect_retry_after: over.respect_retry_after.unwrap_or(self.respect_retry_after),
            backoff: over.backoff.clone().unwrap_or_else(|| self.backoff.clone()),
            retry_on: over.retry_on.unwrap_or(self.retry_on),
            switch_on: over.switch_on.unwrap_or(self.switch_on),
        }
    }
}

/// The retry section of the config: one default policy plus keyed overrides.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct RetrySettings {
    #[serde(default)]
    pub default: RetryConfig,
    /// Keyed by provider id (e.g. `"anthropic-user"`) or account-kind name
    /// (e.g. `"anthropic"`). Provider-id keys win over kind keys.
    #[serde(default)]
    pub providers: BTreeMap<String, RetryOverride>,
}

impl RetrySettings {
    /// Resolve the effective policy for a provider. `provider_id` is the
    /// account id; `kind` is its credential family. Precedence, highest first:
    /// exact provider-id override, then kind override, then the bare default.
    pub fn resolve(&self, provider_id: &str, kind: Option<&str>) -> RetryConfig {
        let mut config = self.default.clone();
        if let Some(kind) = kind
            && let Some(over) = self.providers.get(kind)
        {
            config = config.with_override(over);
        }
        if let Some(over) = self.providers.get(provider_id) {
            config = config.with_override(over);
        }
        config
    }
}

// ---------------------------------------------------------------------------
// General settings
// ---------------------------------------------------------------------------

/// Miscellaneous cross-cutting toggles. Kept intentionally small; this is the
/// "General" tab's home and the place new global flags land.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct GeneralSettings {
    /// Persist the session automatically after each completed turn.
    #[serde(default = "default_true")]
    pub autosave_sessions: bool,
    /// Stream the model's thinking/reasoning into the transcript.
    #[serde(default = "default_true")]
    pub show_thinking: bool,
    /// Default `max_tokens` for a new agent when nothing else specifies it.
    #[serde(default = "default_max_output_tokens")]
    pub default_max_output_tokens: u32,
    /// Provider used for context compaction. `None` leaves selection to the
    /// compaction caller (and keeps older configurations unchanged).
    #[serde(default)]
    pub compaction_provider: Option<String>,
    /// Model used for context compaction. `None` leaves selection to the
    /// compaction caller (and keeps older configurations unchanged).
    #[serde(default)]
    pub compaction_model: Option<String>,
    /// Hosted web-search policy. `None` (the default) is off even when the
    /// focused provider advertises the capability. Values: `"cached"`,
    /// `"indexed"`, `"live"`. Stored next to `show_thinking` because this is a
    /// machine-wide user policy, not a per-persona model preference.
    #[serde(default)]
    pub web_search: Option<String>,
}

fn default_max_output_tokens() -> u32 {
    32_000
}

impl Default for GeneralSettings {
    fn default() -> Self {
        Self {
            autosave_sessions: true,
            show_thinking: true,
            default_max_output_tokens: default_max_output_tokens(),
            compaction_provider: None,
            compaction_model: None,
            web_search: None,
        }
    }
}

// ---------------------------------------------------------------------------
// Umbrella config
// ---------------------------------------------------------------------------

/// The whole `~/.firmius/config.json`. New settings groups are new fields here,
/// each with a `#[serde(default)]` so older files keep loading.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FirmiusConfig {
    #[serde(default = "default_version")]
    pub version: u32,
    #[serde(default)]
    pub retry: RetrySettings,
    #[serde(default)]
    pub general: GeneralSettings,
    #[serde(skip)]
    storage_path: Option<PathBuf>,
}

fn default_version() -> u32 {
    CONFIG_VERSION
}

impl Default for FirmiusConfig {
    fn default() -> Self {
        Self {
            version: CONFIG_VERSION,
            retry: RetrySettings::default(),
            general: GeneralSettings::default(),
            storage_path: None,
        }
    }
}

impl FirmiusConfig {
    pub fn load() -> Result<Self, ConfigError> {
        Self::load_from_path(default_config_path()?)
    }

    pub fn load_from_path(path: impl Into<PathBuf>) -> Result<Self, ConfigError> {
        let path = path.into();
        if !path.exists() {
            return Ok(Self {
                storage_path: Some(path),
                ..Self::default()
            });
        }
        let content = fs::read_to_string(&path).map_err(|source| ConfigError::Io {
            path: path.clone(),
            source,
        })?;
        let mut config: Self =
            serde_json::from_str(&content).map_err(|source| ConfigError::Json {
                path: path.clone(),
                source,
            })?;
        config.storage_path = Some(path);
        Ok(config)
    }

    pub fn save(&self) -> Result<(), ConfigError> {
        let path = match &self.storage_path {
            Some(path) => path.clone(),
            None => default_config_path()?,
        };
        self.save_to_path(path)
    }

    pub fn save_to_path(&self, path: impl Into<PathBuf>) -> Result<(), ConfigError> {
        let path = path.into();
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|source| ConfigError::Io {
                path: parent.to_path_buf(),
                source,
            })?;
        }
        let bytes = serde_json::to_vec_pretty(self).map_err(|source| ConfigError::Json {
            path: path.clone(),
            source,
        })?;
        let tmp = path.with_extension(format!("json.tmp.{}", std::process::id()));
        fs::write(&tmp, bytes).map_err(|source| ConfigError::Io {
            path: tmp.clone(),
            source,
        })?;
        fs::rename(&tmp, &path).map_err(|source| ConfigError::Io {
            path: path.clone(),
            source,
        })?;
        Ok(())
    }

    /// Effective retry policy for a provider account.
    pub fn retry_for(&self, provider_id: &str, kind: Option<&str>) -> RetryConfig {
        self.retry.resolve(provider_id, kind)
    }
}

pub fn default_config_path() -> Result<PathBuf, ConfigError> {
    dirs::home_dir()
        .map(|home| home.join(".firmius").join("config.json"))
        .ok_or(ConfigError::HomeDirUnavailable)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_file(name: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir()
            .join(format!("firmius-config-test-{name}-{nonce}"))
            .join("config.json")
    }

    #[test]
    fn missing_config_loads_defaults() {
        let path = temp_file("missing");
        let config = FirmiusConfig::load_from_path(&path).unwrap();
        assert_eq!(config.version, CONFIG_VERSION);
        assert!(config.retry.default.enabled);
        assert_eq!(config.retry.default.max_attempts_per_account, 3);
        assert!(config.general.autosave_sessions);
        assert_eq!(config.general.compaction_provider, None);
        assert_eq!(config.general.compaction_model, None);
        assert_eq!(config.general.web_search, None);
    }

    #[test]
    fn saves_atomically_and_round_trips() {
        let path = temp_file("roundtrip");
        let mut config = FirmiusConfig::default();
        config.retry.default.max_attempts_per_account = 5;
        config.retry.default.backoff.initial_ms = 250;
        config.general.compaction_provider = Some("anthropic".into());
        config.general.compaction_model = Some("claude-sonnet-4".into());
        config.retry.providers.insert(
            "anthropic".to_string(),
            RetryOverride {
                max_attempts_per_account: Some(2),
                account_switching: Some(false),
                ..Default::default()
            },
        );
        config.save_to_path(&path).unwrap();

        let loaded = FirmiusConfig::load_from_path(&path).unwrap();
        assert_eq!(loaded.retry.default.max_attempts_per_account, 5);
        assert_eq!(loaded.retry.default.backoff.initial_ms, 250);
        assert_eq!(
            loaded.general.compaction_provider.as_deref(),
            Some("anthropic")
        );
        assert_eq!(
            loaded.general.compaction_model.as_deref(),
            Some("claude-sonnet-4")
        );
        assert_eq!(
            loaded
                .retry
                .providers
                .get("anthropic")
                .unwrap()
                .max_attempts_per_account,
            Some(2)
        );
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn web_search_defaults_to_off_and_old_files_still_load() {
        let path = temp_file("web-search-default");
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(
            &path,
            r#"{"version":1,"general":{"autosave_sessions":true}}"#,
        )
        .unwrap();
        let config = FirmiusConfig::load_from_path(&path).unwrap();
        assert_eq!(config.general.web_search, None);
        assert_eq!(FirmiusConfig::default().general.web_search, None);
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn old_config_without_compaction_fields_loads_and_saves() {
        let path = temp_file("old-without-compaction");
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(
            &path,
            r#"{"version":1,"general":{"autosave_sessions":false}}"#,
        )
        .unwrap();

        let config = FirmiusConfig::load_from_path(&path).unwrap();
        assert!(!config.general.autosave_sessions);
        assert_eq!(config.general.compaction_provider, None);
        assert_eq!(config.general.compaction_model, None);
        config.save_to_path(&path).unwrap();
        let saved = std::fs::read_to_string(&path).unwrap();
        assert!(saved.contains("compaction_provider"));
        assert!(saved.contains("compaction_model"));
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn override_merges_onto_default() {
        let mut settings = RetrySettings::default();
        settings.default.max_attempts_per_account = 4;
        settings.default.account_switching = true;
        settings.providers.insert(
            "anthropic".to_string(),
            RetryOverride {
                account_switching: Some(false),
                ..Default::default()
            },
        );
        let resolved = settings.resolve("anthropic-user", Some("anthropic"));
        // Inherited from default:
        assert_eq!(resolved.max_attempts_per_account, 4);
        // Overridden by the kind key:
        assert!(!resolved.account_switching);
    }

    #[test]
    fn provider_id_override_beats_kind_override() {
        let mut settings = RetrySettings::default();
        settings.providers.insert(
            "anthropic".to_string(),
            RetryOverride {
                max_attempts_per_account: Some(2),
                ..Default::default()
            },
        );
        settings.providers.insert(
            "anthropic-user".to_string(),
            RetryOverride {
                max_attempts_per_account: Some(9),
                ..Default::default()
            },
        );
        let resolved = settings.resolve("anthropic-user", Some("anthropic"));
        assert_eq!(resolved.max_attempts_per_account, 9);
    }

    #[test]
    fn exponential_backoff_grows_and_caps() {
        let backoff = BackoffConfig {
            strategy: BackoffStrategy::Exponential,
            initial_ms: 100,
            multiplier: 2.0,
            max_delay_ms: 500,
            jitter: 0.0,
        };
        assert_eq!(backoff.base_delay_ms(1), 100);
        assert_eq!(backoff.base_delay_ms(2), 200);
        assert_eq!(backoff.base_delay_ms(3), 400);
        // 800 clamps to the 500 ceiling.
        assert_eq!(backoff.base_delay_ms(4), 500);
    }

    #[test]
    fn fixed_and_linear_backoff() {
        let fixed = BackoffConfig {
            strategy: BackoffStrategy::Fixed,
            initial_ms: 300,
            multiplier: 2.0,
            max_delay_ms: 10_000,
            jitter: 0.0,
        };
        assert_eq!(fixed.base_delay_ms(1), 300);
        assert_eq!(fixed.base_delay_ms(5), 300);

        let linear = BackoffConfig {
            strategy: BackoffStrategy::Linear,
            initial_ms: 200,
            multiplier: 2.0,
            max_delay_ms: 10_000,
            jitter: 0.0,
        };
        assert_eq!(linear.base_delay_ms(1), 200);
        assert_eq!(linear.base_delay_ms(3), 600);
    }

    #[test]
    fn partial_json_fills_defaults() {
        let path = temp_file("partial");
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        // Only sets one nested field; everything else must default.
        std::fs::write(
            &path,
            r#"{"version":1,"retry":{"default":{"max_attempts_per_account":7}}}"#,
        )
        .unwrap();
        let config = FirmiusConfig::load_from_path(&path).unwrap();
        assert_eq!(config.retry.default.max_attempts_per_account, 7);
        // Untouched nested defaults still present:
        assert!(config.retry.default.enabled);
        assert_eq!(config.retry.default.backoff.initial_ms, 500);
        assert!(config.general.show_thinking);
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }
}
