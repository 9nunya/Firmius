use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::PathBuf;

pub const USER_SETTINGS_VERSION: u32 = 1;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PreferredModel {
    pub provider_id: String,
    pub model: String,
    #[serde(default)]
    pub effort: Option<String>,
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
    #[serde(skip)]
    storage_path: Option<PathBuf>,
}

impl Default for UserSettings {
    fn default() -> Self {
        Self {
            version: USER_SETTINGS_VERSION,
            default_model: None,
            persona_models: BTreeMap::new(),
            theme: None,
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
        let tmp = path.with_extension(format!("json.tmp.{}", std::process::id()));
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
}

pub fn default_user_settings_path() -> Result<PathBuf, UserSettingsError> {
    dirs::home_dir()
        .map(|home| home.join(".firmius").join("settings.json"))
        .ok_or(UserSettingsError::HomeDirUnavailable)
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
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }
}
