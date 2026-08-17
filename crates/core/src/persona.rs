use indexmap::IndexMap;
use serde::Deserialize;
use std::path::{Path, PathBuf};

use crate::persistence;

pub const FS_READ_SCOPE: &str = "fs_read";
pub const FS_WRITE_SCOPE: &str = "fs_write";
pub const PROCESSES_SCOPE: &str = "processes";
pub const DELEGATION_SCOPE: &str = "delegation";

const STOCK_PERSONAS: &[(&str, &str)] = &[
    ("lead.md", include_str!("personas/lead.md")),
    ("general.md", include_str!("personas/general.md")),
    ("coder.md", include_str!("personas/coder.md")),
    ("reviewer.md", include_str!("personas/reviewer.md")),
];

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Persona {
    pub id: String,
    pub name: String,
    pub tool_scopes: Vec<String>,
    /// Delegate-only eligibility. This is unrelated to delegate run/spawn mode.
    pub background: bool,
    pub system_prompt: String,
    pub path: PathBuf,
}

impl Persona {
    pub fn allows_scope(&self, scope: &str) -> bool {
        self.tool_scopes.iter().any(|candidate| candidate == scope)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PersonaDiagnostic {
    pub path: PathBuf,
    pub message: String,
}

#[derive(Debug, thiserror::Error)]
pub enum PersonaError {
    #[error("persona I/O error at {path}: {message}")]
    Io { path: PathBuf, message: String },
    #[error("persona not found: {0}")]
    NotFound(String),
    #[error("persona '{0}' is delegate-only")]
    BackgroundOnly(String),
}

#[derive(Debug, Clone, Default)]
pub struct PersonaManager {
    directory: PathBuf,
    personas: IndexMap<String, Persona>,
    diagnostics: Vec<PersonaDiagnostic>,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PersonaFrontmatter {
    pub name: String,
    #[serde(default, alias = "scopes")]
    pub tool_scopes: Vec<String>,
    #[serde(default)]
    pub background: bool,
    #[serde(default)]
    pub description: Option<String>,
}

pub fn default_personas_dir() -> PathBuf {
    persistence::data_dir().join("personas")
}

impl PersonaManager {
    pub fn load_default() -> Result<Self, PersonaError> {
        Self::load_from(default_personas_dir())
    }

    pub fn load() -> Result<Self, PersonaError> {
        Self::load_default()
    }

    pub fn load_from_dir(directory: impl Into<PathBuf>) -> Result<Self, PersonaError> {
        Self::load_from(directory.into())
    }

    /// Load personas from `directory`, applying the same missing/empty bootstrap
    /// rule as the real user directory. This keeps tests fully hermetic.
    pub fn load_from(directory: PathBuf) -> Result<Self, PersonaError> {
        bootstrap_stock_personas(&directory)?;
        let mut manager = Self {
            directory,
            personas: IndexMap::new(),
            diagnostics: Vec::new(),
        };
        manager.reload()?;
        Ok(manager)
    }

    /// An empty manager for legacy/test agents that do not use personas.
    pub fn empty() -> Self {
        Self::default()
    }

    pub fn directory(&self) -> &Path {
        &self.directory
    }

    pub fn dir(&self) -> &Path {
        self.directory()
    }

    pub fn reload(&mut self) -> Result<(), PersonaError> {
        self.personas.clear();
        self.diagnostics.clear();
        if self.directory.as_os_str().is_empty() {
            return Ok(());
        }
        let entries = std::fs::read_dir(&self.directory).map_err(|error| PersonaError::Io {
            path: self.directory.clone(),
            message: error.to_string(),
        })?;
        let mut paths = entries
            .filter_map(Result::ok)
            .map(|entry| entry.path())
            .filter(|path| {
                path.is_file()
                    && path
                        .extension()
                        .and_then(|extension| extension.to_str())
                        .is_some_and(|extension| extension.eq_ignore_ascii_case("md"))
            })
            .collect::<Vec<_>>();
        paths.sort();

        for path in paths {
            match parse_persona_file(&path) {
                Ok(persona) => {
                    if self.personas.contains_key(&persona.id) {
                        self.diagnostics.push(PersonaDiagnostic {
                            path,
                            message: format!(
                                "duplicate persona id '{}' derived from filename",
                                persona.id
                            ),
                        });
                    } else {
                        self.personas.insert(persona.id.clone(), persona);
                    }
                }
                Err(message) => self.diagnostics.push(PersonaDiagnostic { path, message }),
            }
        }
        Ok(())
    }

    pub fn get(&self, id: &str) -> Option<&Persona> {
        self.personas.get(id)
    }

    pub fn contains(&self, id: &str) -> bool {
        self.personas.contains_key(id)
    }

    pub fn is_empty(&self) -> bool {
        self.personas.is_empty()
    }

    pub fn require(&self, id: &str) -> Result<&Persona, PersonaError> {
        self.get(id)
            .ok_or_else(|| PersonaError::NotFound(id.to_string()))
    }

    pub fn list(&self) -> Vec<Persona> {
        let mut personas = self.personas.values().cloned().collect::<Vec<_>>();
        personas.sort_by(|left, right| {
            left.name
                .to_lowercase()
                .cmp(&right.name.to_lowercase())
                .then_with(|| left.id.cmp(&right.id))
        });
        personas
    }

    pub fn main_personas(&self) -> Vec<Persona> {
        self.list()
            .into_iter()
            .filter(|persona| !persona.background)
            .collect()
    }

    pub fn diagnostics(&self) -> &[PersonaDiagnostic] {
        &self.diagnostics
    }
}

fn bootstrap_stock_personas(directory: &Path) -> Result<(), PersonaError> {
    if !directory.exists() {
        std::fs::create_dir_all(directory).map_err(|error| PersonaError::Io {
            path: directory.to_path_buf(),
            message: error.to_string(),
        })?;
    }
    let mut entries = std::fs::read_dir(directory).map_err(|error| PersonaError::Io {
        path: directory.to_path_buf(),
        message: error.to_string(),
    })?;
    if entries.next().is_some() {
        return Ok(());
    }
    for (name, contents) in STOCK_PERSONAS {
        let path = directory.join(name);
        std::fs::write(&path, contents).map_err(|error| PersonaError::Io {
            path,
            message: error.to_string(),
        })?;
    }
    Ok(())
}

fn parse_persona_file(path: &Path) -> Result<Persona, String> {
    let id = path
        .file_stem()
        .and_then(|stem| stem.to_str())
        .map(kebab_case)
        .ok_or_else(|| "filename is not valid UTF-8".to_string())?;
    if id.is_empty() {
        return Err("filename does not produce a persona id".to_string());
    }
    let source = std::fs::read_to_string(path).map_err(|error| error.to_string())?;
    let normalized = source.replace("\r\n", "\n");
    // The first bundled General persona was mistakenly marked main-capable.
    // Recognize only that exact, unmodified stock file so existing installs get
    // the corrected runtime policy without rewriting any user-owned persona.
    let legacy_stock_general = id == "general"
        && normalized.trim()
            == include_str!("personas/general.md")
                .replace("background: true", "background: false")
                .trim();
    let Some(rest) = normalized.strip_prefix("---\n") else {
        return Err("missing opening YAML frontmatter delimiter".to_string());
    };
    let Some((yaml, body)) = rest.split_once("\n---\n") else {
        return Err("missing closing YAML frontmatter delimiter".to_string());
    };
    let frontmatter: PersonaFrontmatter =
        serde_yaml::from_str(yaml).map_err(|error| format!("invalid YAML frontmatter: {error}"))?;
    let name = frontmatter.name.trim().to_string();
    if name.is_empty() {
        return Err("persona name cannot be empty".to_string());
    }
    let system_prompt = body.trim().to_string();
    if system_prompt.is_empty() {
        return Err("persona system prompt cannot be empty".to_string());
    }
    let mut tool_scopes = frontmatter
        .tool_scopes
        .into_iter()
        .map(|scope| scope.trim().to_string())
        .filter(|scope| !scope.is_empty())
        .collect::<Vec<_>>();
    tool_scopes.sort();
    tool_scopes.dedup();
    Ok(Persona {
        id,
        name,
        tool_scopes,
        background: frontmatter.background || legacy_stock_general,
        system_prompt,
        path: path.to_path_buf(),
    })
}

fn kebab_case(input: &str) -> String {
    let mut out = String::new();
    let mut previous_dash = false;
    for ch in input.chars() {
        if ch.is_ascii_alphanumeric() {
            if ch.is_ascii_uppercase() && !out.is_empty() && !previous_dash {
                out.push('-');
            }
            out.push(ch.to_ascii_lowercase());
            previous_dash = false;
        } else if !out.is_empty() && !previous_dash {
            out.push('-');
            previous_dash = true;
        }
    }
    while out.ends_with('-') {
        out.pop();
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let directory =
            std::env::temp_dir().join(format!("firmius-persona-{name}-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&directory).unwrap();
        directory
    }

    #[test]
    fn empty_directory_bootstraps_stock_personas() {
        let directory = temp_dir("bootstrap");
        let manager = PersonaManager::load_from(directory.clone()).unwrap();
        assert_eq!(manager.list().len(), 4);
        assert!(manager.get("lead").is_some_and(|persona| {
            !persona.background && persona.allows_scope(DELEGATION_SCOPE)
        }));
        assert!(manager.get("coder").is_some_and(|persona| {
            persona.background && !persona.allows_scope(DELEGATION_SCOPE)
        }));
        assert!(
            manager
                .get("general")
                .is_some_and(|persona| persona.background)
        );
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn nonempty_directory_is_never_bootstrapped() {
        let directory = temp_dir("nonempty");
        std::fs::write(directory.join("keep.txt"), "mine").unwrap();
        let manager = PersonaManager::load_from(directory.clone()).unwrap();
        assert!(manager.list().is_empty());
        assert!(!directory.join("lead.md").exists());
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn malformed_files_become_diagnostics_without_hiding_valid_files() {
        let directory = temp_dir("diagnostics");
        std::fs::write(
            directory.join("My Custom_Persona.md"),
            "---\nname: Valid\nscopes: [custom]\nbackground: false\n---\nDo useful work.",
        )
        .unwrap();
        std::fs::write(directory.join("Bad_Name.md"), "not frontmatter").unwrap();
        let manager = PersonaManager::load_from(directory.clone()).unwrap();
        assert_eq!(manager.list().len(), 1);
        assert_eq!(manager.diagnostics().len(), 1);
        assert!(
            manager
                .get("my-custom-persona")
                .unwrap()
                .allows_scope("custom")
        );
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn preferred_models_are_rejected_in_frontmatter_and_duplicate_ids_are_diagnosed() {
        let directory = temp_dir("schema");
        std::fs::write(
            directory.join("worker.md"),
            "---\nname: Worker\nscopes: [fs_read]\npreferred_model: provider/model\n---\nWork.",
        )
        .unwrap();
        std::fs::write(
            directory.join("My Persona.md"),
            "---\nname: First\nscopes: []\n---\nFirst prompt.",
        )
        .unwrap();
        std::fs::write(
            directory.join("my_persona.md"),
            "---\nname: Second\nscopes: []\n---\nSecond prompt.",
        )
        .unwrap();

        let manager = PersonaManager::load_from(directory.clone()).unwrap();
        assert_eq!(manager.list().len(), 1);
        assert_eq!(manager.get("my-persona").unwrap().name, "First");
        assert_eq!(manager.diagnostics().len(), 2);
        assert!(manager.diagnostics().iter().any(|diagnostic| {
            diagnostic
                .message
                .contains("unknown field `preferred_model`")
        }));
        assert!(manager.diagnostics().iter().any(|diagnostic| {
            diagnostic
                .message
                .contains("duplicate persona id 'my-persona'")
        }));
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn main_personas_exclude_delegate_only_personas() {
        let directory = temp_dir("main-filter");
        let manager = PersonaManager::load_from(directory.clone()).unwrap();
        let ids = manager
            .main_personas()
            .into_iter()
            .map(|persona| persona.id)
            .collect::<Vec<_>>();
        assert_eq!(ids, vec!["lead"]);
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn legacy_stock_general_is_delegate_only_without_rewriting_its_file() {
        let directory = temp_dir("legacy-general");
        let legacy =
            include_str!("personas/general.md").replace("background: true", "background: false");
        let path = directory.join("general.md");
        std::fs::write(&path, &legacy).unwrap();

        let manager = PersonaManager::load_from(directory.clone()).unwrap();
        assert!(manager.get("general").unwrap().background);
        assert_eq!(std::fs::read_to_string(path).unwrap(), legacy);
        std::fs::remove_dir_all(directory).ok();
    }
}
