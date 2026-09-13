use super::{PERMISSION_POLICY_VERSION, PermissionPolicy};
use fs2::FileExt;
use std::fs::{self, OpenOptions};
use std::io;
use std::path::{Path, PathBuf};

#[derive(Debug, thiserror::Error)]
pub enum PermissionStoreError {
    #[error("unable to resolve home directory for ~/.firmius/permissions.json")]
    HomeDirUnavailable,
    #[error("permission store I/O error at {path}: {source}")]
    Io { path: PathBuf, source: io::Error },
    #[error("permission store JSON error at {path}: {source}")]
    Json {
        path: PathBuf,
        source: serde_json::Error,
    },
    #[error("stale permission policy revision: expected {expected}, found {actual}")]
    StaleRevision { expected: u64, actual: u64 },
    #[error("unsupported permission policy version {found}; maximum supported is {supported}")]
    UnsupportedVersion { found: u32, supported: u32 },
}

pub fn default_permission_store_path() -> Result<PathBuf, PermissionStoreError> {
    PermissionStore::default_path()
}

#[derive(Debug, Clone)]
pub struct PermissionStore {
    path: PathBuf,
}

impl PermissionStore {
    pub fn new(path: impl Into<PathBuf>) -> Self {
        Self { path: path.into() }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
    pub fn default_path() -> Result<PathBuf, PermissionStoreError> {
        Ok(crate::persistence::data_dir().join("permissions.json"))
    }
    pub fn current() -> Result<Self, PermissionStoreError> {
        Ok(Self::new(Self::default_path()?))
    }
    pub fn load_from_path(
        path: impl Into<PathBuf>,
    ) -> Result<PermissionPolicy, PermissionStoreError> {
        Self::new(path).load()
    }
    pub fn load(&self) -> Result<PermissionPolicy, PermissionStoreError> {
        if !self.path.exists() {
            return Ok(PermissionPolicy::default());
        }
        let text = fs::read_to_string(&self.path).map_err(|source| PermissionStoreError::Io {
            path: self.path.clone(),
            source,
        })?;
        let mut policy: PermissionPolicy =
            serde_json::from_str(&text).map_err(|source| PermissionStoreError::Json {
                path: self.path.clone(),
                source,
            })?;
        if policy.version == 0 {
            policy.version = PERMISSION_POLICY_VERSION;
        }
        if policy.version > PERMISSION_POLICY_VERSION {
            return Err(PermissionStoreError::UnsupportedVersion {
                found: policy.version,
                supported: PERMISSION_POLICY_VERSION,
            });
        }
        Ok(policy)
    }
    pub fn save(&self, policy: &PermissionPolicy) -> Result<(), PermissionStoreError> {
        let lock = self.lock_file()?;
        let result = self.write_atomic(policy);
        let _ = lock.unlock();
        result
    }
    pub fn compare_and_swap(
        &self,
        expected_revision: u64,
        mut next: PermissionPolicy,
    ) -> Result<PermissionPolicy, PermissionStoreError> {
        let lock = self.lock_file()?;
        let current = self.load()?;
        if current.revision != expected_revision {
            let _ = lock.unlock();
            return Err(PermissionStoreError::StaleRevision {
                expected: expected_revision,
                actual: current.revision,
            });
        }
        next.revision = expected_revision.saturating_add(1);
        next.version = PERMISSION_POLICY_VERSION;
        let result = self.write_atomic(&next);
        let _ = lock.unlock();
        result.map(|()| next)
    }
    pub fn update<F>(
        &self,
        expected_revision: u64,
        f: F,
    ) -> Result<PermissionPolicy, PermissionStoreError>
    where
        F: FnOnce(&mut PermissionPolicy),
    {
        let mut next = self.load()?;
        if next.revision != expected_revision {
            return Err(PermissionStoreError::StaleRevision {
                expected: expected_revision,
                actual: next.revision,
            });
        }
        f(&mut next);
        self.compare_and_swap(expected_revision, next)
    }
    fn write_atomic(&self, policy: &PermissionPolicy) -> Result<(), PermissionStoreError> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent).map_err(|source| PermissionStoreError::Io {
                path: parent.to_path_buf(),
                source,
            })?;
        }
        let tmp = self
            .path
            .with_extension(format!("json.tmp-{}", std::process::id()));
        let bytes =
            serde_json::to_vec_pretty(policy).map_err(|source| PermissionStoreError::Json {
                path: self.path.clone(),
                source,
            })?;
        let mut file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&tmp)
            .map_err(|source| PermissionStoreError::Io {
                path: tmp.clone(),
                source,
            })?;
        use std::io::Write;
        file.write_all(&bytes)
            .and_then(|_| file.sync_all())
            .map_err(|source| PermissionStoreError::Io {
                path: tmp.clone(),
                source,
            })?;
        fs::rename(&tmp, &self.path).map_err(|source| PermissionStoreError::Io {
            path: self.path.clone(),
            source,
        })
    }

    fn lock_file(&self) -> Result<std::fs::File, PermissionStoreError> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent).map_err(|source| PermissionStoreError::Io {
                path: parent.to_path_buf(),
                source,
            })?;
        }
        let path = self.path.with_extension("json.lock");
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .open(&path)
            .map_err(|source| PermissionStoreError::Io {
                path: path.clone(),
                source,
            })?;
        file.lock_exclusive()
            .map_err(|source| PermissionStoreError::Io { path, source })?;
        Ok(file)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::permissions::{PermissionAction, PermissionDecision, PermissionMode};
    use std::time::{SystemTime, UNIX_EPOCH};

    fn path() -> PathBuf {
        std::env::temp_dir().join(format!(
            "firmius-permissions-{}-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos(),
            uuid::Uuid::new_v4().simple()
        ))
    }

    #[test]
    fn defaults_ask_edits_and_hosted_search() {
        let p = PermissionPolicy::default();
        assert_eq!(
            p.evaluate(&PermissionAction::new("edit_file", Some("a"))),
            PermissionDecision::Ask
        );
        assert_eq!(
            p.evaluate(&PermissionAction::new("network", Some("a"))),
            PermissionDecision::Ask
        );
        assert_eq!(
            p.evaluate(&PermissionAction::new("unrecognized", Some("a"))),
            PermissionDecision::Deny
        );
    }
    #[test]
    fn cas_and_persistence() {
        let store = PermissionStore::new(path());
        let first = store.load().unwrap();
        let next = store.compare_and_swap(0, first.clone()).unwrap();
        assert_eq!(next.revision, 1);
        assert!(matches!(
            store.compare_and_swap(0, first),
            Err(PermissionStoreError::StaleRevision { .. })
        ));
        assert_eq!(store.load().unwrap().revision, 1);
        let _ = fs::remove_file(store.path());
    }

    #[test]
    fn future_policy_version_is_rejected() {
        let store = PermissionStore::new(path());
        fs::write(
            store.path(),
            format!(
                r#"{{"version":{},"mode":"default","profiles":{{}}}}"#,
                PERMISSION_POLICY_VERSION + 1
            ),
        )
        .unwrap();
        assert!(matches!(
            store.load(),
            Err(PermissionStoreError::UnsupportedVersion { .. })
        ));
        let _ = fs::remove_file(store.path());
    }
    #[test]
    fn deny_wins_over_higher_priority_allow() {
        let mut p = PermissionPolicy::default();
        p.mode = PermissionMode::Custom("x".into());
        p.profiles.insert(
            "x".into(),
            super::super::PermissionProfile {
                rules: vec![
                    super::super::PermissionRule {
                        id: "allow".into(),
                        action_kind: Some("edit*".into()),
                        target: None,
                        decision: PermissionDecision::Allow,
                        priority: 100,
                        enabled: true,
                    },
                    super::super::PermissionRule {
                        id: "deny".into(),
                        action_kind: Some("edit_file".into()),
                        target: None,
                        decision: PermissionDecision::Deny,
                        priority: 1,
                        enabled: true,
                    },
                ],
            },
        );
        assert_eq!(
            p.evaluate(&PermissionAction::new("edit_file", None::<String>)),
            PermissionDecision::Deny
        );
    }

    #[test]
    fn old_policy_json_migrates_missing_fields() {
        let store = PermissionStore::new(path());
        fs::write(store.path(), r#"{"mode":"default","profiles":{}}"#).unwrap();
        let policy = store.load().unwrap();
        assert_eq!(policy.version, PERMISSION_POLICY_VERSION);
        assert!(!policy.yolo_confirmed);
        let _ = fs::remove_file(store.path());
    }

    #[test]
    fn action_digest_is_stable_for_parameter_insertion_order() {
        let a = PermissionAction::new("edit_file", Some("x"))
            .with_parameter("b", serde_json::json!(2))
            .with_parameter("a", serde_json::json!(1));
        let b = PermissionAction::new("edit_file", Some("x"))
            .with_parameter("a", serde_json::json!(1))
            .with_parameter("b", serde_json::json!(2));
        assert_eq!(a.digest(), b.digest());
    }
}
