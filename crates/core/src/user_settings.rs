use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};

fn persistence_lock() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}

pub const USER_SETTINGS_VERSION: u32 = 1;
/// Increment only when the first-run experience changes enough that existing
/// users should be offered it again. Dismissing the OOBE counts as seeing it;
/// users can always reopen it from the TUI.
pub const ONBOARDING_VERSION: u32 = 1;

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct OnboardingState {
    #[serde(default)]
    pub completed_version: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PreferredModel {
    pub provider_id: String,
    pub model: String,
    #[serde(default)]
    pub effort: Option<String>,
}

/// A user-approved SSH target remembered by Firmius. The SSH config remains
/// authoritative for authentication and transport options; this record only
/// remembers the convenient workspace default the user chose in Firmius.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SavedSshHost {
    pub alias: String,
    pub directory: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UserSettings {
    pub version: u32,
    #[serde(default)]
    pub default_model: Option<PreferredModel>,
    #[serde(default)]
    pub persona_models: BTreeMap<String, PreferredModel>,
    /// Theme name. `None` = default "firmius". Persisted like `default_model`.
    #[serde(default)]
    pub theme: Option<String>,
    /// Recent composer submissions, newest last. Capped; used by Up/Down
    /// prompt history in the TUI. Not a transcript — just the lines the
    /// human typed, so they survive process restarts.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub prompt_history: Vec<String>,
    /// Persisted separately from provider state: a user may intentionally use
    /// Firmius without configuring an account during the first-run flow.
    #[serde(default)]
    pub onboarding: OnboardingState,
    /// Remote targets explicitly saved from SSH discovery or the TUI.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub remote_hosts: Vec<SavedSshHost>,
    #[serde(skip)]
    storage_path: Option<PathBuf>,
}

/// How many composer submissions we keep across restarts.
pub const PROMPT_HISTORY_CAP: usize = 200;

impl Default for UserSettings {
    fn default() -> Self {
        Self {
            version: USER_SETTINGS_VERSION,
            default_model: None,
            persona_models: BTreeMap::new(),
            theme: None,
            prompt_history: Vec::new(),
            onboarding: OnboardingState::default(),
            remote_hosts: Vec::new(),
            storage_path: None,
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum UserSettingsError {
    #[error("unable to resolve home directory for ~/.firmius/settings.json")]
    HomeDirUnavailable,
    #[error("settings I/O error at {path}: {source}")]
    Io { path: PathBuf, source: io::Error },
    #[error("settings JSON error at {path}: {source}")]
    Json {
        path: PathBuf,
        source: serde_json::Error,
    },
}

impl UserSettings {
    pub fn load() -> Result<Self, UserSettingsError> {
        Self::load_from_path(default_user_settings_path()?)
    }

    pub fn load_from_path(path: impl Into<PathBuf>) -> Result<Self, UserSettingsError> {
        let path = path.into();
        if !path.exists() {
            return Ok(Self {
                storage_path: Some(path),
                ..Self::default()
            });
        }
        let content = fs::read_to_string(&path).map_err(|source| UserSettingsError::Io {
            path: path.clone(),
            source,
        })?;
        let mut settings: Self =
            serde_json::from_str(&content).map_err(|source| UserSettingsError::Json {
                path: path.clone(),
                source,
            })?;
        settings.storage_path = Some(path);
        Ok(settings)
    }

    pub fn save(&self) -> Result<(), UserSettingsError> {
        let path = match &self.storage_path {
            Some(path) => path.clone(),
            None => default_user_settings_path()?,
        };
        self.save_to_path(path)
    }

    pub fn save_to_path(&self, path: impl Into<PathBuf>) -> Result<(), UserSettingsError> {
        let path = path.into();
        // Multiple TUI/daemon surfaces can persist settings at once. Serialize
        // the snapshot-and-rename sequence and give each writer a unique temp
        // path so one writer can never rename over another writer's staging
        // file or leave a partially-written settings document behind.
        let _persist_guard = persistence_lock().lock().unwrap();
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|source| UserSettingsError::Io {
                path: parent.to_path_buf(),
                source,
            })?;
        }
        let bytes = serde_json::to_vec_pretty(self).map_err(|source| UserSettingsError::Json {
            path: path.clone(),
            source,
        })?;
        let tmp = path.with_extension(format!(
            "json.tmp.{}.{}",
            std::process::id(),
            uuid::Uuid::new_v4()
        ));
        fs::write(&tmp, bytes).map_err(|source| UserSettingsError::Io {
            path: tmp.clone(),
            source,
        })?;
        fs::rename(&tmp, &path).map_err(|source| UserSettingsError::Io {
            path: path.clone(),
            source,
        })?;
        Ok(())
    }

    pub fn preferred_model(&self, persona_id: &str) -> Option<&PreferredModel> {
        self.persona_models.get(persona_id)
    }

    pub fn preferred_default_model(&self) -> Option<&PreferredModel> {
        self.default_model.as_ref()
    }

    pub fn needs_onboarding(&self) -> bool {
        self.onboarding.completed_version < ONBOARDING_VERSION
    }

    /// Mark the current OOBE as seen. "Skip for now" deliberately calls this
    /// too, so Firmius never traps someone in a recurring setup wizard.
    pub fn complete_onboarding(&mut self) {
        self.onboarding.completed_version = ONBOARDING_VERSION;
    }

    pub fn reset_onboarding(&mut self) {
        self.onboarding.completed_version = 0;
    }

    pub fn save_remote_host(
        &mut self,
        alias: impl Into<String>,
        directory: impl Into<String>,
    ) -> Result<(), String> {
        let alias = alias.into().trim().to_string();
        let directory = directory.into().trim().to_string();
        if alias.is_empty() || alias.starts_with('-') || alias.chars().any(char::is_whitespace) {
            return Err("SSH alias must be a non-empty name without whitespace".into());
        }
        if !directory.starts_with('/') {
            return Err("SSH workspace directory must be an absolute path".into());
        }
        if let Some(existing) = self
            .remote_hosts
            .iter_mut()
            .find(|host| host.alias == alias)
        {
            existing.directory = directory;
        } else {
            self.remote_hosts.push(SavedSshHost { alias, directory });
            self.remote_hosts.sort_by(|a, b| a.alias.cmp(&b.alias));
        }
        Ok(())
    }

    pub fn remove_remote_host(&mut self, alias: &str) -> bool {
        let before = self.remote_hosts.len();
        self.remote_hosts.retain(|host| host.alias != alias);
        self.remote_hosts.len() != before
    }

    pub fn set_preferred_default_model(
        &mut self,
        provider_id: impl Into<String>,
        model: impl Into<String>,
    ) {
        self.set_preferred_default(provider_id, model, None);
    }

    pub fn set_preferred_default(
        &mut self,
        provider_id: impl Into<String>,
        model: impl Into<String>,
        effort: Option<String>,
    ) {
        self.default_model = Some(PreferredModel {
            provider_id: provider_id.into(),
            model: model.into(),
            effort,
        });
    }

    pub fn clear_preferred_default_model(&mut self) -> Option<PreferredModel> {
        self.default_model.take()
    }

    pub fn set_preferred_model(
        &mut self,
        persona_id: impl Into<String>,
        provider_id: impl Into<String>,
        model: impl Into<String>,
    ) {
        self.set_preferred_model_and_effort(persona_id, provider_id, model, None);
    }

    pub fn set_preferred_model_and_effort(
        &mut self,
        persona_id: impl Into<String>,
        provider_id: impl Into<String>,
        model: impl Into<String>,
        effort: Option<String>,
    ) {
        self.persona_models.insert(
            persona_id.into(),
            PreferredModel {
                provider_id: provider_id.into(),
                model: model.into(),
                effort,
            },
        );
    }

    pub fn clear_preferred_model(&mut self, persona_id: &str) -> Option<PreferredModel> {
        self.persona_models.remove(persona_id)
    }

    /// Record a composer submission. Empty / whitespace-only lines are
    /// ignored; consecutive duplicates are collapsed; the list is capped.
    pub fn push_prompt(&mut self, prompt: impl Into<String>) {
        let prompt = prompt.into();
        let prompt = prompt.trim();
        if prompt.is_empty() {
            return;
        }
        if self.prompt_history.last().map(String::as_str) == Some(prompt) {
            return;
        }
        self.prompt_history.push(prompt.to_string());
        if self.prompt_history.len() > PROMPT_HISTORY_CAP {
            let excess = self.prompt_history.len() - PROMPT_HISTORY_CAP;
            self.prompt_history.drain(..excess);
        }
    }
}

pub fn default_user_settings_path() -> Result<PathBuf, UserSettingsError> {
    Ok(crate::persistence::data_dir().join("settings.json"))
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
            .join(format!("firmius-settings-test-{name}-{nonce}"))
            .join("settings.json")
    }

    #[test]
    fn missing_settings_loads_default() {
        let path = temp_file("missing");
        let mut settings = UserSettings::load_from_path(&path).unwrap();
        assert_eq!(settings.version, USER_SETTINGS_VERSION);
        assert!(settings.default_model.is_none());
        assert!(settings.persona_models.is_empty());
        settings.set_preferred_default_model("openai", "gpt-default");
        settings.save().unwrap();
        assert_eq!(
            UserSettings::load_from_path(&path)
                .unwrap()
                .preferred_default_model()
                .unwrap()
                .model,
            "gpt-default"
        );
        fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn saved_ssh_hosts_validate_update_sort_and_remove() {
        let mut settings = UserSettings::default();
        settings.save_remote_host("prod", "/srv/prod").unwrap();
        settings.save_remote_host("build", "/srv/build").unwrap();
        settings.save_remote_host("prod", "/srv/new-prod").unwrap();
        assert_eq!(
            settings
                .remote_hosts
                .iter()
                .map(|host| host.alias.as_str())
                .collect::<Vec<_>>(),
            ["build", "prod"]
        );
        assert_eq!(settings.remote_hosts[1].directory, "/srv/new-prod");
        assert!(settings.save_remote_host("bad alias", "/srv/x").is_err());
        assert!(settings.save_remote_host("bad", "relative").is_err());
        assert!(settings.remove_remote_host("build"));
        assert!(!settings.remove_remote_host("missing"));
    }

    #[test]
    fn saves_atomically_and_round_trips_preferred_models() {
        let path = temp_file("roundtrip");
        let mut settings = UserSettings::default();
        settings.set_preferred_default("openai", "gpt-5.5", Some("high".to_string()));
        settings.set_preferred_model_and_effort(
            "coder",
            "anthropic",
            "claude-fable-5",
            Some("medium".to_string()),
        );
        settings.save_to_path(&path).unwrap();
        let loaded = UserSettings::load_from_path(&path).unwrap();
        assert_eq!(
            loaded.preferred_default_model().unwrap(),
            &PreferredModel {
                provider_id: "openai".into(),
                model: "gpt-5.5".into(),
                effort: Some("high".into()),
            }
        );
        assert_eq!(
            loaded.preferred_model("coder").unwrap().provider_id,
            "anthropic"
        );
        assert_eq!(
            loaded.preferred_model("coder").unwrap().model,
            "claude-fable-5"
        );
        assert_eq!(
            loaded.preferred_model("coder").unwrap().effort.as_deref(),
            Some("medium")
        );
        assert!(path.exists());
        assert_eq!(fs::read_dir(path.parent().unwrap()).unwrap().count(), 1);
        fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn concurrent_saves_leave_valid_json_and_no_staging_files() {
        let path = temp_file("concurrent");
        let mut settings = UserSettings::default();
        settings.set_preferred_default_model("openai", "baseline");
        let writers = (0..8)
            .map(|index| {
                let mut snapshot = settings.clone();
                snapshot.set_preferred_default_model("openai", format!("model-{index}"));
                let path = path.clone();
                std::thread::spawn(move || snapshot.save_to_path(path).unwrap())
            })
            .collect::<Vec<_>>();
        for writer in writers {
            writer.join().unwrap();
        }
        let loaded = UserSettings::load_from_path(&path).unwrap();
        assert!(
            loaded
                .preferred_default_model()
                .is_some_and(|model| model.model.starts_with("model-"))
        );
        assert_eq!(fs::read_dir(path.parent().unwrap()).unwrap().count(), 1);
        fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn clear_preferred_model_removes_mapping() {
        let mut settings = UserSettings::default();
        settings.set_preferred_model("reviewer", "openai", "gpt-5.5");
        assert!(settings.clear_preferred_model("reviewer").is_some());
        assert!(settings.preferred_model("reviewer").is_none());
    }

    #[test]
    fn settings_without_a_default_model_remain_compatible() {
        let path = temp_file("legacy");
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(&path, r#"{"version":1,"persona_models":{}}"#).unwrap();
        let settings = UserSettings::load_from_path(&path).unwrap();
        assert!(settings.preferred_default_model().is_none());
        assert!(settings.needs_onboarding());
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn onboarding_can_be_completed_and_reopened() {
        let path = temp_file("onboarding");
        let mut settings = UserSettings::default();
        assert!(settings.needs_onboarding());
        settings.complete_onboarding();
        assert!(!settings.needs_onboarding());
        settings.save_to_path(&path).unwrap();

        let mut loaded = UserSettings::load_from_path(&path).unwrap();
        assert!(!loaded.needs_onboarding());
        loaded.reset_onboarding();
        assert!(loaded.needs_onboarding());
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn prompt_history_caps_dedupes_and_round_trips() {
        let path = temp_file("prompts");
        let mut settings = UserSettings::default();
        settings.push_prompt("  ");
        settings.push_prompt("one");
        settings.push_prompt("one");
        settings.push_prompt("two");
        assert_eq!(settings.prompt_history, ["one", "two"]);
        for i in 0..PROMPT_HISTORY_CAP + 5 {
            settings.push_prompt(format!("n{i}"));
        }
        assert_eq!(settings.prompt_history.len(), PROMPT_HISTORY_CAP);
        assert_eq!(settings.prompt_history.first().unwrap(), "n5");
        settings.save_to_path(&path).unwrap();
        let loaded = UserSettings::load_from_path(&path).unwrap();
        assert_eq!(loaded.prompt_history.len(), PROMPT_HISTORY_CAP);
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }
}
