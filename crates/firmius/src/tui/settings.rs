//! A modular, schema-driven settings framework for the TUI.
//!
//! The design goal: **tabs are data, not bespoke UI.** A settings tab does not
//! draw anything itself. Instead it *describes* its controls as a list of
//! [`Field`]s (each a typed [`FieldValue`] with a label and help text), and a
//! single generic renderer + editor in [`super::modal`] knows how to draw and
//! mutate any field of any kind. Adding a new tab is implementing
//! [`SettingsSection`]; adding a new control type is one new [`FieldValue`]
//! variant handled in the two generic match arms — never a new per-tab widget.
//!
//! The single source of truth is a working [`FirmiusConfig`]. Fields are a
//! *projection* of that config: `fields()` reads the config into controls, and
//! `apply()` writes one edited control back into the config. The modal owns the
//! working copy and persists it on save.

use firmius_core::{BackoffStrategy, FirmiusConfig, RetryConfig, RetryOverride};

// ---------------------------------------------------------------------------
// Field model
// ---------------------------------------------------------------------------

/// One editable value, tagged with everything the generic editor needs to
/// mutate it safely (bounds, step, options). No tab ever hand-rolls these
/// behaviors.
#[derive(Debug, Clone, PartialEq)]
pub enum FieldValue {
    /// An on/off toggle.
    Bool(bool),
    /// A bounded integer adjusted by `step`.
    Int {
        value: i64,
        min: i64,
        max: i64,
        step: i64,
    },
    /// A bounded float adjusted by `step`.
    Float {
        value: f64,
        min: f64,
        max: f64,
        step: f64,
    },
    /// A one-of-N selection. `options` is `(value, label)`; `selected` indexes
    /// into it.
    Choice {
        selected: usize,
        options: Vec<(String, String)>,
    },
    /// Free text. Part of the field vocabulary and fully supported by the
    /// generic editor; no shipping section declares one yet, so it is reserved
    /// for future string settings (e.g. an API base URL override).
    #[allow(dead_code)]
    Text(String),
}

impl FieldValue {
    /// A compact display of the current value for the field row.
    pub fn display(&self) -> String {
        match self {
            FieldValue::Bool(b) => (if *b { "on" } else { "off" }).to_string(),
            FieldValue::Int { value, .. } => value.to_string(),
            FieldValue::Float { value, .. } => format_float(*value),
            FieldValue::Choice { selected, options } => options
                .get(*selected)
                .map(|(_, label)| label.clone())
                .unwrap_or_default(),
            FieldValue::Text(s) => {
                if s.is_empty() {
                    "(empty)".to_string()
                } else {
                    s.clone()
                }
            }
        }
    }

    /// Whether this field is edited by cycling left/right (vs. text entry).
    pub fn is_cyclable(&self) -> bool {
        !matches!(self, FieldValue::Text(_))
    }

    /// Adjust the value by one step in `dir` (`-1` or `+1`). For non-numeric
    /// kinds this toggles/cycles. No-op for [`FieldValue::Text`].
    pub fn nudge(&mut self, dir: i32) {
        match self {
            FieldValue::Bool(b) => *b = !*b,
            FieldValue::Int {
                value,
                min,
                max,
                step,
            } => {
                let next = *value + (*step) * dir as i64;
                *value = next.clamp(*min, *max);
            }
            FieldValue::Float {
                value,
                min,
                max,
                step,
            } => {
                let next = *value + *step * dir as f64;
                // Round to a sane number of decimals to avoid float drift.
                *value = round2(next.clamp(*min, *max));
            }
            FieldValue::Choice { selected, options } => {
                if options.is_empty() {
                    return;
                }
                let n = options.len() as i32;
                *selected = (*selected as i32 + dir).rem_euclid(n) as usize;
            }
            FieldValue::Text(_) => {}
        }
    }

    /// Commit a typed string into the value, best-effort. Returns `Err` with a
    /// message when the text does not parse for a numeric field.
    pub fn commit_text(&mut self, text: &str) -> Result<(), String> {
        match self {
            FieldValue::Text(s) => {
                *s = text.to_string();
                Ok(())
            }
            FieldValue::Int {
                value, min, max, ..
            } => {
                let parsed: i64 = text
                    .trim()
                    .parse()
                    .map_err(|_| format!("'{text}' is not a whole number"))?;
                *value = parsed.clamp(*min, *max);
                Ok(())
            }
            FieldValue::Float {
                value, min, max, ..
            } => {
                let parsed: f64 = text
                    .trim()
                    .parse()
                    .map_err(|_| format!("'{text}' is not a number"))?;
                *value = round2(parsed.clamp(*min, *max));
                Ok(())
            }
            // Bool/Choice are not text-editable; ignore.
            FieldValue::Bool(_) | FieldValue::Choice { .. } => Ok(()),
        }
    }

    /// The current value as a string, for seeding the inline text editor.
    pub fn as_edit_seed(&self) -> String {
        match self {
            FieldValue::Text(s) => s.clone(),
            FieldValue::Int { value, .. } => value.to_string(),
            FieldValue::Float { value, .. } => format_float(*value),
            _ => String::new(),
        }
    }

    fn choice_value(&self) -> Option<&str> {
        match self {
            FieldValue::Choice { selected, options } => {
                options.get(*selected).map(|(v, _)| v.as_str())
            }
            _ => None,
        }
    }

    fn bool(&self) -> Option<bool> {
        match self {
            FieldValue::Bool(b) => Some(*b),
            _ => None,
        }
    }

    fn int(&self) -> Option<i64> {
        match self {
            FieldValue::Int { value, .. } => Some(*value),
            _ => None,
        }
    }

    fn float(&self) -> Option<f64> {
        match self {
            FieldValue::Float { value, .. } => Some(*value),
            _ => None,
        }
    }
}

/// A single labelled control on a settings tab.
#[derive(Debug, Clone, PartialEq)]
pub struct Field {
    /// Stable identifier, unique within its section, e.g.
    /// `"max_attempts_per_account"`. `apply()` dispatches on this.
    pub id: String,
    pub label: String,
    pub help: String,
    pub value: FieldValue,
}

impl Field {
    pub fn toggle(id: &str, label: &str, help: &str, on: bool) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
            help: help.into(),
            value: FieldValue::Bool(on),
        }
    }

    pub fn int(
        id: &str,
        label: &str,
        help: &str,
        value: i64,
        min: i64,
        max: i64,
        step: i64,
    ) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
            help: help.into(),
            value: FieldValue::Int {
                value,
                min,
                max,
                step,
            },
        }
    }

    pub fn float(
        id: &str,
        label: &str,
        help: &str,
        value: f64,
        min: f64,
        max: f64,
        step: f64,
    ) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
            help: help.into(),
            value: FieldValue::Float {
                value,
                min,
                max,
                step,
            },
        }
    }

    pub fn choice(
        id: &str,
        label: &str,
        help: &str,
        selected: usize,
        options: Vec<(String, String)>,
    ) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
            help: help.into(),
            value: FieldValue::Choice { selected, options },
        }
    }
}

// ---------------------------------------------------------------------------
// Section trait — one per tab
// ---------------------------------------------------------------------------

/// A settings tab. It projects a working [`FirmiusConfig`] into a list of
/// [`Field`]s and writes edited fields back. Sections may be stateful (e.g. the
/// retry tab remembers which provider scope is being edited), so the methods
/// take `&mut self`.
pub trait SettingsSection: Send {
    /// Tab label shown in the tab bar.
    fn title(&self) -> &str;

    /// Build the control list from the current config.
    fn fields(&mut self, config: &FirmiusConfig) -> Vec<Field>;

    /// Persist one edited field back into the config.
    fn apply(&mut self, config: &mut FirmiusConfig, field: &Field);

    /// Whether changing `field_id` should trigger a full field-list rebuild
    /// (e.g. a scope selector that changes which fields are shown). Defaults to
    /// no rebuild, so ordinary edits are cheap and preserve cursor position.
    fn rebuild_on_change(&self, _field_id: &str) -> bool {
        false
    }
}

// ---------------------------------------------------------------------------
// Retry section
// ---------------------------------------------------------------------------

/// Which retry policy the Retry tab is editing: the global default, or a
/// per-provider/kind override that merges onto it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RetryScope {
    Default,
    /// Provider id or account-kind name (a key in `retry.providers`).
    Provider(String),
}

/// The Retry tab. Holds a scope selector so the *same* generic field machinery
/// edits either the default policy or any per-provider override.
pub struct RetrySection {
    scope: RetryScope,
    /// Candidate provider scopes offered by the scope selector (provider ids
    /// and kind names discovered from the manager), sorted and de-duplicated.
    scopes: Vec<String>,
}

impl RetrySection {
    /// `scopes` are additional provider/kind keys the user may target, e.g.
    /// `["anthropic", "anthropic-user", "openai"]`.
    pub fn new(mut scopes: Vec<String>) -> Self {
        scopes.sort();
        scopes.dedup();
        Self {
            scope: RetryScope::Default,
            scopes,
        }
    }

    fn scope_options(&self) -> Vec<(String, String)> {
        let mut options = vec![(
            "__default__".to_string(),
            "Default (all providers)".to_string(),
        )];
        for key in &self.scopes {
            options.push((key.clone(), format!("Override: {key}")));
        }
        options
    }

    fn scope_index(&self) -> usize {
        match &self.scope {
            RetryScope::Default => 0,
            RetryScope::Provider(key) => self
                .scopes
                .iter()
                .position(|s| s == key)
                .map(|i| i + 1)
                .unwrap_or(0),
        }
    }

    /// The effective config being edited: the default, or the default merged
    /// with the selected override so inherited values show through.
    fn effective(&self, config: &FirmiusConfig) -> RetryConfig {
        match &self.scope {
            RetryScope::Default => config.retry.default.clone(),
            RetryScope::Provider(key) => match config.retry.providers.get(key) {
                Some(over) => config.retry.default.with_override(over),
                None => config.retry.default.clone(),
            },
        }
    }

    /// Write a full [`RetryConfig`] back into the working config for the current
    /// scope. For a provider scope we store a full override (every field set),
    /// which keeps the mental model simple: what you see on the tab is exactly
    /// what that provider will use.
    fn store(&self, config: &mut FirmiusConfig, retry: RetryConfig) {
        match &self.scope {
            RetryScope::Default => config.retry.default = retry,
            RetryScope::Provider(key) => {
                config
                    .retry
                    .providers
                    .insert(key.clone(), full_override(&retry));
            }
        }
    }
}

fn backoff_strategy_options() -> Vec<(String, String)> {
    vec![
        ("exponential".into(), "Exponential".into()),
        ("linear".into(), "Linear".into()),
        ("fixed".into(), "Fixed".into()),
    ]
}

fn strategy_to_value(strategy: BackoffStrategy) -> &'static str {
    match strategy {
        BackoffStrategy::Exponential => "exponential",
        BackoffStrategy::Linear => "linear",
        BackoffStrategy::Fixed => "fixed",
    }
}

fn value_to_strategy(value: &str) -> BackoffStrategy {
    match value {
        "linear" => BackoffStrategy::Linear,
        "fixed" => BackoffStrategy::Fixed,
        _ => BackoffStrategy::Exponential,
    }
}

impl SettingsSection for RetrySection {
    fn title(&self) -> &str {
        "Retry"
    }

    fn rebuild_on_change(&self, field_id: &str) -> bool {
        // Switching scope changes which override is edited; rebuild so the
        // fields reflect the newly selected policy.
        field_id == "scope"
    }

    fn fields(&mut self, config: &FirmiusConfig) -> Vec<Field> {
        let retry = self.effective(config);
        let strat_options = backoff_strategy_options();
        let strat_selected = strat_options
            .iter()
            .position(|(v, _)| v == strategy_to_value(retry.backoff.strategy))
            .unwrap_or(0);

        vec![
            Field::choice(
                "scope",
                "Scope",
                "Which policy to edit: the shared default or a per-provider override.",
                self.scope_index(),
                self.scope_options(),
            ),
            Field::toggle(
                "enabled",
                "Enabled",
                "Master switch. When off, the first provider error is surfaced immediately.",
                retry.enabled,
            ),
            Field::int(
                "max_attempts_per_account",
                "Attempts per account",
                "Total tries on one account before giving up on it (1 = no retries).",
                retry.max_attempts_per_account as i64,
                1,
                20,
                1,
            ),
            Field::toggle(
                "account_switching",
                "Account switching",
                "After an account is exhausted, try other accounts for the same provider.",
                retry.account_switching,
            ),
            Field::int(
                "max_accounts",
                "Max accounts",
                "Cap on distinct accounts to try in one turn (0 = use all available).",
                retry.max_accounts as i64,
                0,
                20,
                1,
            ),
            Field::int(
                "max_elapsed_ms",
                "Time budget (ms)",
                "Wall-clock cap on the whole retry loop (0 = no limit).",
                retry.max_elapsed_ms as i64,
                0,
                600_000,
                1_000,
            ),
            Field::toggle(
                "respect_retry_after",
                "Respect Retry-After",
                "Honor a server rate-limit hint when it is longer than the backoff.",
                retry.respect_retry_after,
            ),
            Field::choice(
                "backoff.strategy",
                "Backoff strategy",
                "How the delay grows between successive retries.",
                strat_selected,
                strat_options,
            ),
            Field::int(
                "backoff.initial_ms",
                "Initial delay (ms)",
                "Delay before the first retry (and the fixed delay for Fixed).",
                retry.backoff.initial_ms as i64,
                0,
                60_000,
                100,
            ),
            Field::float(
                "backoff.multiplier",
                "Backoff multiplier",
                "Growth factor per attempt for the Exponential strategy.",
                retry.backoff.multiplier,
                1.0,
                10.0,
                0.5,
            ),
            Field::int(
                "backoff.max_delay_ms",
                "Max delay (ms)",
                "Upper bound on any single retry delay after growth.",
                retry.backoff.max_delay_ms as i64,
                0,
                600_000,
                1_000,
            ),
            Field::float(
                "backoff.jitter",
                "Jitter",
                "Random spread as a fraction of the delay (0 = none, 0.2 = +/-20%).",
                retry.backoff.jitter,
                0.0,
                1.0,
                0.05,
            ),
            Field::toggle(
                "retry_on.rate_limited",
                "Retry on rate limit",
                "Retry the same account on HTTP 429/529.",
                retry.retry_on.rate_limited,
            ),
            Field::toggle(
                "retry_on.server_error",
                "Retry on server error",
                "Retry the same account on HTTP 5xx.",
                retry.retry_on.server_error,
            ),
            Field::toggle(
                "retry_on.transport",
                "Retry on transport error",
                "Retry the same account on network/timeout failures.",
                retry.retry_on.transport,
            ),
            Field::toggle(
                "retry_on.decode",
                "Retry on decode error",
                "Retry the same account when a response fails to decode.",
                retry.retry_on.decode,
            ),
            Field::toggle(
                "retry_on.auth",
                "Retry on auth error",
                "Retry the same account on 401/403 (usually leave off).",
                retry.retry_on.auth,
            ),
            Field::toggle(
                "switch_on.rate_limited",
                "Switch on rate limit",
                "Move to another account on HTTP 429/529.",
                retry.switch_on.rate_limited,
            ),
            Field::toggle(
                "switch_on.server_error",
                "Switch on server error",
                "Move to another account on HTTP 5xx.",
                retry.switch_on.server_error,
            ),
            Field::toggle(
                "switch_on.transport",
                "Switch on transport error",
                "Move to another account on network/timeout failures.",
                retry.switch_on.transport,
            ),
            Field::toggle(
                "switch_on.decode",
                "Switch on decode error",
                "Move to another account when a response fails to decode.",
                retry.switch_on.decode,
            ),
            Field::toggle(
                "switch_on.auth",
                "Switch on auth error",
                "Move to another account on 401/403 (a fresh key may work).",
                retry.switch_on.auth,
            ),
        ]
    }

    fn apply(&mut self, config: &mut FirmiusConfig, field: &Field) {
        // The scope selector is special: it changes section state, not config.
        if field.id == "scope" {
            if let Some(value) = field.value.choice_value() {
                self.scope = if value == "__default__" {
                    RetryScope::Default
                } else {
                    RetryScope::Provider(value.to_string())
                };
            }
            return;
        }

        let mut retry = self.effective(config);
        match field.id.as_str() {
            "enabled" => retry.enabled = field.value.bool().unwrap_or(retry.enabled),
            "max_attempts_per_account" => {
                if let Some(v) = field.value.int() {
                    retry.max_attempts_per_account = v.max(1) as u32;
                }
            }
            "account_switching" => {
                retry.account_switching = field.value.bool().unwrap_or(retry.account_switching)
            }
            "max_accounts" => {
                if let Some(v) = field.value.int() {
                    retry.max_accounts = v.max(0) as u32;
                }
            }
            "max_elapsed_ms" => {
                if let Some(v) = field.value.int() {
                    retry.max_elapsed_ms = v.max(0) as u64;
                }
            }
            "respect_retry_after" => {
                retry.respect_retry_after = field.value.bool().unwrap_or(retry.respect_retry_after)
            }
            "backoff.strategy" => {
                if let Some(v) = field.value.choice_value() {
                    retry.backoff.strategy = value_to_strategy(v);
                }
            }
            "backoff.initial_ms" => {
                if let Some(v) = field.value.int() {
                    retry.backoff.initial_ms = v.max(0) as u64;
                }
            }
            "backoff.multiplier" => {
                if let Some(v) = field.value.float() {
                    retry.backoff.multiplier = v;
                }
            }
            "backoff.max_delay_ms" => {
                if let Some(v) = field.value.int() {
                    retry.backoff.max_delay_ms = v.max(0) as u64;
                }
            }
            "backoff.jitter" => {
                if let Some(v) = field.value.float() {
                    retry.backoff.jitter = v.clamp(0.0, 1.0);
                }
            }
            "retry_on.rate_limited" => set_bool(&mut retry.retry_on.rate_limited, field),
            "retry_on.server_error" => set_bool(&mut retry.retry_on.server_error, field),
            "retry_on.transport" => set_bool(&mut retry.retry_on.transport, field),
            "retry_on.decode" => set_bool(&mut retry.retry_on.decode, field),
            "retry_on.auth" => set_bool(&mut retry.retry_on.auth, field),
            "switch_on.rate_limited" => set_bool(&mut retry.switch_on.rate_limited, field),
            "switch_on.server_error" => set_bool(&mut retry.switch_on.server_error, field),
            "switch_on.transport" => set_bool(&mut retry.switch_on.transport, field),
            "switch_on.decode" => set_bool(&mut retry.switch_on.decode, field),
            "switch_on.auth" => set_bool(&mut retry.switch_on.auth, field),
            _ => {}
        }
        self.store(config, retry);
    }
}

fn set_bool(target: &mut bool, field: &Field) {
    if let Some(b) = field.value.bool() {
        *target = b;
    }
}

/// Build a fully-populated override from a resolved config, so a provider scope
/// stores exactly what it will use (no surprising inheritance after the fact).
fn full_override(retry: &RetryConfig) -> RetryOverride {
    RetryOverride {
        enabled: Some(retry.enabled),
        max_attempts_per_account: Some(retry.max_attempts_per_account),
        account_switching: Some(retry.account_switching),
        max_accounts: Some(retry.max_accounts),
        max_elapsed_ms: Some(retry.max_elapsed_ms),
        respect_retry_after: Some(retry.respect_retry_after),
        backoff: Some(retry.backoff.clone()),
        retry_on: Some(retry.retry_on),
        switch_on: Some(retry.switch_on),
    }
}

// ---------------------------------------------------------------------------
// General section
// ---------------------------------------------------------------------------

/// A second tab, purely to prove the framework is modular: it declares three
/// unrelated fields and shares every bit of rendering/editing with Retry.
pub struct GeneralSection;

impl SettingsSection for GeneralSection {
    fn title(&self) -> &str {
        "General"
    }

    fn fields(&mut self, config: &FirmiusConfig) -> Vec<Field> {
        vec![
            Field::toggle(
                "autosave_sessions",
                "Autosave sessions",
                "Save the session automatically after each completed turn.",
                config.general.autosave_sessions,
            ),
            Field::toggle(
                "show_thinking",
                "Show thinking",
                "Stream the model's reasoning into the transcript.",
                config.general.show_thinking,
            ),
            Field::int(
                "default_max_output_tokens",
                "Default max output tokens",
                "Default response token budget for a new agent.",
                config.general.default_max_output_tokens as i64,
                256,
                200_000,
                1_000,
            ),
        ]
    }

    fn apply(&mut self, config: &mut FirmiusConfig, field: &Field) {
        match field.id.as_str() {
            "autosave_sessions" => {
                config.general.autosave_sessions = field
                    .value
                    .bool()
                    .unwrap_or(config.general.autosave_sessions)
            }
            "show_thinking" => {
                config.general.show_thinking =
                    field.value.bool().unwrap_or(config.general.show_thinking)
            }
            "default_max_output_tokens" => {
                if let Some(v) = field.value.int() {
                    config.general.default_max_output_tokens = v.max(256) as u32;
                }
            }
            _ => {}
        }
    }
}

// ---------------------------------------------------------------------------
// Small numeric helpers
// ---------------------------------------------------------------------------

fn round2(v: f64) -> f64 {
    (v * 100.0).round() / 100.0
}

fn format_float(v: f64) -> String {
    // Trim trailing zeros for a clean display: 2.00 -> "2", 0.20 -> "0.2".
    let s = format!("{v:.2}");
    let trimmed = s.trim_end_matches('0').trim_end_matches('.');
    if trimmed.is_empty() {
        "0".to_string()
    } else {
        trimmed.to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn int_field_nudges_within_bounds() {
        let mut v = FieldValue::Int {
            value: 3,
            min: 1,
            max: 5,
            step: 1,
        };
        v.nudge(1);
        assert_eq!(v.int(), Some(4));
        v.nudge(1);
        v.nudge(1); // would be 6, clamps to 5
        assert_eq!(v.int(), Some(5));
        v.nudge(-10);
        assert_eq!(v.int(), Some(1));
    }

    #[test]
    fn choice_cycles_and_reports_value() {
        let mut v = FieldValue::Choice {
            selected: 0,
            options: vec![("a".into(), "A".into()), ("b".into(), "B".into())],
        };
        v.nudge(1);
        assert_eq!(v.choice_value(), Some("b"));
        v.nudge(1); // wraps to a
        assert_eq!(v.choice_value(), Some("a"));
    }

    #[test]
    fn text_commit_and_numeric_parse() {
        let mut t = FieldValue::Text(String::new());
        t.commit_text("hello").unwrap();
        assert_eq!(t.display(), "hello");

        let mut n = FieldValue::Int {
            value: 0,
            min: 0,
            max: 100,
            step: 1,
        };
        assert!(n.commit_text("42").is_ok());
        assert_eq!(n.int(), Some(42));
        assert!(n.commit_text("nope").is_err());
        // Out-of-range clamps rather than erroring.
        n.commit_text("9999").unwrap();
        assert_eq!(n.int(), Some(100));
    }

    #[test]
    fn retry_section_round_trips_a_default_edit() {
        let mut config = FirmiusConfig::default();
        let mut section = RetrySection::new(vec!["anthropic".into()]);
        let fields = section.fields(&config);
        // Flip "enabled" off and apply it.
        let mut enabled = fields.iter().find(|f| f.id == "enabled").cloned().unwrap();
        enabled.value.nudge(1); // toggles bool
        section.apply(&mut config, &enabled);
        assert!(!config.retry.default.enabled);
    }

    #[test]
    fn retry_section_scope_switch_targets_provider_override() {
        let mut config = FirmiusConfig::default();
        let mut section = RetrySection::new(vec!["anthropic".into()]);

        // Switch scope to the anthropic override.
        let scope = Field::choice("scope", "Scope", "", 1, section.scope_options());
        assert!(section.rebuild_on_change(&scope.id));
        section.apply(&mut config, &scope);

        // Now edit attempts; it must land in the provider override, not default.
        let fields = section.fields(&config);
        let mut attempts = fields
            .iter()
            .find(|f| f.id == "max_attempts_per_account")
            .cloned()
            .unwrap();
        attempts.value.commit_text("7").unwrap();
        section.apply(&mut config, &attempts);

        assert_eq!(config.retry.default.max_attempts_per_account, 3); // untouched
        assert_eq!(
            config
                .retry
                .providers
                .get("anthropic")
                .unwrap()
                .max_attempts_per_account,
            Some(7)
        );
    }

    #[test]
    fn general_section_is_just_another_field_list() {
        let mut config = FirmiusConfig::default();
        let mut section = GeneralSection;
        assert_eq!(section.title(), "General");
        let fields = section.fields(&config);
        let mut toggle = fields
            .iter()
            .find(|f| f.id == "autosave_sessions")
            .cloned()
            .unwrap();
        toggle.value.nudge(1);
        section.apply(&mut config, &toggle);
        assert!(!config.general.autosave_sessions);
    }
}
