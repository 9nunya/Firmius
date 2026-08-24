//! Session artifacts: a session-scoped virtual filesystem.
//!
//! Artifacts live in session memory (not the project filesystem) and are
//! addressed as `artifact://<path>`. Every agent in a session shares the same
//! store, so a subagent can leave a note on the board that a parent or sibling
//! can read back through the normal `read`/`list`/`grep`/`glob`/`edit` tools.
//!
//! The store is persisted with the session record so artifacts survive a
//! process restart, exactly like agent histories do.

use std::collections::BTreeMap;
use std::sync::RwLock;
use std::sync::atomic::{AtomicU64, Ordering};

use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};

/// Scheme prefix used to address artifacts, e.g. `artifact://review-notes.md`.
pub const ARTIFACT_SCHEME: &str = "artifact://";

/// Whether `path` addresses the artifact namespace rather than the filesystem.
pub fn is_artifact_path(path: &str) -> bool {
    path.trim().starts_with(ARTIFACT_SCHEME)
}

#[derive(Debug, thiserror::Error)]
pub enum ArtifactError {
    #[error("invalid artifact path: '{0}'")]
    InvalidPath(String),
    #[error("artifact not found: {0}")]
    NotFound(String),
}

/// Where an artifact came from. Useful provenance when agents inspect the
/// board: a hand-written note is distinguishable from an automatic result.
#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum ArtifactSource {
    #[default]
    Manual,
    DelegateResult {
        agent_id: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        delegate_id: Option<String>,
    },
}

/// One artifact file: a path under `artifact://` plus its UTF-8 content.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Artifact {
    pub path: String,
    pub content: String,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub created_by_agent_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub updated_by_agent_id: Option<String>,
    #[serde(default)]
    pub source: ArtifactSource,
    #[serde(default)]
    pub revision: u64,
}

/// Split an `artifact://` reference into normalized path components.
///
/// Accepts both `artifact://a/b` and the bare `a/b` form (for internal
/// callers), strips redundant separators, and rejects `..` traversal.
fn split_artifact_path(path: &str) -> Result<Vec<String>, ArtifactError> {
    let mut path = path.trim().to_string();
    if let Some(rest) = path.strip_prefix(ARTIFACT_SCHEME) {
        path = rest.to_string();
    }
    while let Some(rest) = path.strip_prefix("./") {
        path = rest.to_string();
    }
    if path.starts_with('/') {
        return Err(ArtifactError::InvalidPath(path));
    }

    let mut components = Vec::new();
    for component in path.split('/') {
        match component {
            "" | "." => {}
            ".." => return Err(ArtifactError::InvalidPath(path)),
            other => components.push(other.to_string()),
        }
    }
    Ok(components)
}

/// Normalize a file reference. The result is non-empty and never carries the
/// `artifact://` scheme (the store keys are scheme-free paths).
pub fn normalize_artifact_path(path: &str) -> Result<String, ArtifactError> {
    let components = split_artifact_path(path)?;
    if components.is_empty() {
        return Err(ArtifactError::InvalidPath(path.to_string()));
    }
    Ok(components.join("/"))
}

/// Normalize a directory reference. May be empty, which denotes the namespace
/// root (`artifact://`).
pub fn normalize_artifact_dir(path: &str) -> Result<String, ArtifactError> {
    let components = split_artifact_path(path)?;
    Ok(components.join("/"))
}

fn slugify(input: &str) -> String {
    let mut out = String::new();
    let mut last_was_dash = false;
    for c in input.chars() {
        if c.is_ascii_alphanumeric() {
            out.push(c.to_ascii_lowercase());
            last_was_dash = false;
        } else if (c == '-' || c == '_' || c == '.' || c.is_whitespace()) && !last_was_dash {
            out.push('-');
            last_was_dash = true;
        }
    }
    let slug = out.trim_matches('-').to_string();
    if slug.is_empty() {
        "agent".to_string()
    } else {
        slug
    }
}

/// Shared, session-wide artifact store. Cheap to clone (it is intended to be
/// held behind an `Arc`), and all access is interior-mutable so it can be
/// handed out to tool handlers and background delegate tasks alike.
#[derive(Debug)]
pub struct SessionArtifacts {
    artifacts: RwLock<BTreeMap<String, Artifact>>,
    /// Monotonic source of suffixes for automatic delegate result artifacts.
    next_result_index: AtomicU64,
}

impl Default for SessionArtifacts {
    fn default() -> Self {
        Self {
            artifacts: RwLock::new(BTreeMap::new()),
            next_result_index: AtomicU64::new(1),
        }
    }
}

impl SessionArtifacts {
    pub fn new() -> Self {
        Self::default()
    }

    /// Rebuild the store from persisted records (session resume).
    pub fn from_records(records: Vec<Artifact>) -> Self {
        let store = Self::new();
        {
            let mut artifacts = store.artifacts.write().unwrap();
            for artifact in records {
                let index = artifact
                    .path
                    .rsplit_once('-')
                    .and_then(|(_, suffix)| suffix.strip_suffix(".md"))
                    .and_then(|suffix| suffix.parse::<u64>().ok())
                    .unwrap_or(0);
                let current = store
                    .next_result_index
                    .load(Ordering::Relaxed)
                    .max(index.saturating_add(1));
                store.next_result_index.store(current, Ordering::Relaxed);
                artifacts.insert(artifact.path.clone(), artifact);
            }
        }
        store
    }

    /// Write or overwrite `path`, returning the stored artifact. `path` may
    /// include the `artifact://` scheme or be a bare store path.
    pub fn write(
        &self,
        path: &str,
        content: impl Into<String>,
        agent_id: Option<&str>,
        source: ArtifactSource,
    ) -> Result<Artifact, ArtifactError> {
        let path = normalize_artifact_path(path)?;
        let now = Utc::now();
        let mut artifacts = self.artifacts.write().unwrap();
        let revision = artifacts
            .get(&path)
            .map(|existing| existing.revision.saturating_add(1))
            .unwrap_or(1);
        let artifact = Artifact {
            created_at: artifacts.get(&path).map(|a| a.created_at).unwrap_or(now),
            updated_at: now,
            created_by_agent_id: artifacts
                .get(&path)
                .and_then(|a| a.created_by_agent_id.clone())
                .or(agent_id.map(str::to_string)),
            updated_by_agent_id: agent_id.map(str::to_string),
            path: path.clone(),
            content: content.into(),
            source,
            revision,
        };
        artifacts.insert(path.clone(), artifact.clone());
        Ok(artifact)
    }

    /// Append to one artifact while holding the store write lock for the
    /// complete read-modify-write operation. Concurrent message senders must
    /// not be able to overwrite one another's durable mailbox log entries.
    pub fn append(
        &self,
        path: &str,
        content: &str,
        agent_id: Option<&str>,
        source: ArtifactSource,
    ) -> Result<Artifact, ArtifactError> {
        let path = normalize_artifact_path(path)?;
        let now = Utc::now();
        let mut artifacts = self.artifacts.write().unwrap();
        let previous = artifacts.get(&path);
        let mut updated = previous.map(|a| a.content.clone()).unwrap_or_default();
        updated.push_str(content);
        let artifact = Artifact {
            created_at: previous.map(|a| a.created_at).unwrap_or(now),
            updated_at: now,
            created_by_agent_id: previous
                .and_then(|a| a.created_by_agent_id.clone())
                .or(agent_id.map(str::to_string)),
            updated_by_agent_id: agent_id.map(str::to_string),
            path: path.clone(),
            content: updated,
            source,
            revision: previous.map(|a| a.revision.saturating_add(1)).unwrap_or(1),
        };
        artifacts.insert(path, artifact.clone());
        Ok(artifact)
    }

    pub fn read(&self, path: &str) -> Result<String, ArtifactError> {
        let path = normalize_artifact_path(path)?;
        let artifacts = self.artifacts.read().unwrap();
        artifacts
            .get(&path)
            .map(|artifact| artifact.content.clone())
            .ok_or(ArtifactError::NotFound(path))
    }

    pub fn get(&self, path: &str) -> Option<Artifact> {
        let path = normalize_artifact_path(path).ok()?;
        self.artifacts.read().unwrap().get(&path).cloned()
    }

    pub fn remove(&self, path: &str) -> Result<(), ArtifactError> {
        let path = normalize_artifact_path(path)?;
        let mut artifacts = self.artifacts.write().unwrap();
        if artifacts.remove(&path).is_some() {
            Ok(())
        } else {
            Err(ArtifactError::NotFound(path))
        }
    }

    /// Move one artifact to another path (both within the artifact namespace).
    pub fn move_path(&self, from: &str, to: &str) -> Result<Artifact, ArtifactError> {
        let from = normalize_artifact_path(from)?;
        let to = normalize_artifact_path(to)?;
        let mut artifacts = self.artifacts.write().unwrap();
        let Some(mut artifact) = artifacts.remove(&from) else {
            return Err(ArtifactError::NotFound(from));
        };
        artifact.path = to.clone();
        artifact.updated_at = Utc::now();
        artifact.revision = artifact.revision.saturating_add(1);
        artifacts.insert(to.clone(), artifact.clone());
        Ok(artifact)
    }

    /// All artifact paths, scheme-free and sorted.
    pub fn paths(&self) -> Vec<String> {
        let artifacts = self.artifacts.read().unwrap();
        let mut paths: Vec<String> = artifacts.keys().cloned().collect();
        paths.sort();
        paths
    }

    /// Full artifact paths under `dir` (empty `dir` lists everything).
    pub fn list(&self, dir: &str) -> Vec<String> {
        let dir = normalize_artifact_dir(dir).unwrap_or_default();
        let artifacts = self.artifacts.read().unwrap();
        let mut paths: Vec<String> = artifacts
            .keys()
            .filter(|path| {
                dir.is_empty()
                    || path.as_str() == dir
                    || path
                        .strip_prefix(&dir)
                        .is_some_and(|rest| rest.starts_with('/'))
            })
            .cloned()
            .collect();
        paths.sort();
        paths
    }

    pub fn len(&self) -> usize {
        self.artifacts.read().unwrap().len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// All artifacts, for session persistence.
    pub fn snapshot(&self) -> Vec<Artifact> {
        self.artifacts.read().unwrap().values().cloned().collect()
    }

    /// Store a finished subagent result under a collision-free
    /// `{slug}-agent-result-{n}.md` path and return the stored artifact.
    pub fn store_delegate_result(
        &self,
        slug: &str,
        content: impl Into<String>,
        agent_id: &str,
        delegate_id: Option<&str>,
    ) -> Artifact {
        let slug = slugify(slug);
        let prefix = format!("{slug}-agent-result-");
        let now = Utc::now();
        let mut artifacts = self.artifacts.write().unwrap();
        loop {
            let index = self.next_result_index.fetch_add(1, Ordering::Relaxed);
            let path = format!("{prefix}{index}.md");
            if artifacts.contains_key(&path) {
                continue;
            }
            let artifact = Artifact {
                path: path.clone(),
                content: content.into(),
                created_at: now,
                updated_at: now,
                created_by_agent_id: Some(agent_id.to_string()),
                updated_by_agent_id: Some(agent_id.to_string()),
                source: ArtifactSource::DelegateResult {
                    agent_id: agent_id.to_string(),
                    delegate_id: delegate_id.map(str::to_string),
                },
                revision: 1,
            };
            artifacts.insert(path.clone(), artifact.clone());
            return artifact;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_and_rejects_paths() {
        assert_eq!(
            normalize_artifact_path("artifact://a/b.md").unwrap(),
            "a/b.md"
        );
        assert_eq!(normalize_artifact_path("a//./b").unwrap(), "a/b");
        assert_eq!(normalize_artifact_dir("artifact://").unwrap(), "");
        assert!(normalize_artifact_path("artifact://").is_err());
        assert!(normalize_artifact_path("artifact://../etc").is_err());
        assert!(normalize_artifact_path("artifact:///abs").is_err());
    }

    #[test]
    fn write_read_update_remove_round_trip() {
        let store = SessionArtifacts::new();
        store
            .write(
                "artifact://note.md",
                "hello",
                Some("agent-1"),
                ArtifactSource::Manual,
            )
            .unwrap();
        assert_eq!(store.read("note.md").unwrap(), "hello");
        assert_eq!(store.len(), 1);

        let updated = store
            .write(
                "note.md",
                "hello again",
                Some("agent-2"),
                ArtifactSource::Manual,
            )
            .unwrap();
        assert_eq!(updated.revision, 2);
        assert_eq!(updated.created_by_agent_id.as_deref(), Some("agent-1"));
        assert_eq!(updated.updated_by_agent_id.as_deref(), Some("agent-2"));

        assert_eq!(store.remove("note.md").unwrap(), ());
        assert!(store.read("note.md").is_err());
    }

    #[test]
    fn list_is_directory_aware() {
        let store = SessionArtifacts::new();
        for path in ["a/one.md", "a/two.md", "b/three.md"] {
            store
                .write(path, "x", None, ArtifactSource::Manual)
                .unwrap();
        }
        assert_eq!(store.list(""), vec!["a/one.md", "a/two.md", "b/three.md"]);
        assert_eq!(store.list("a"), vec!["a/one.md", "a/two.md"]);
        assert_eq!(store.list("a/one.md"), vec!["a/one.md"]);
    }

    #[test]
    fn delegate_results_get_unique_names() {
        let store = SessionArtifacts::new();
        let first = store.store_delegate_result("reviewer", "one", "agent-1", Some("d1"));
        let second = store.store_delegate_result("reviewer", "two", "agent-2", None);
        assert_eq!(first.path, "reviewer-agent-result-1.md");
        assert_eq!(second.path, "reviewer-agent-result-2.md");
        assert!(matches!(
            first.source,
            ArtifactSource::DelegateResult {
                delegate_id: Some(ref id),
                ..
            } if id == "d1"
        ));
    }

    #[test]
    fn resume_seeds_result_counter_past_persisted_names() {
        let store = SessionArtifacts::from_records(vec![Artifact {
            path: "coder-agent-result-7.md".into(),
            content: "x".into(),
            created_at: Utc::now(),
            updated_at: Utc::now(),
            created_by_agent_id: None,
            updated_by_agent_id: None,
            source: ArtifactSource::Manual,
            revision: 1,
        }]);
        let next = store.store_delegate_result("coder", "y", "agent", None);
        assert_eq!(next.path, "coder-agent-result-8.md");
    }
}
