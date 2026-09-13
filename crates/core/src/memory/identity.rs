use std::path::{Path, PathBuf};
use std::process::Command;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ProjectIdentitySource {
    GitRemote,
    CanonicalRoot,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProjectIdentity {
    pub project_id: String,
    pub root: PathBuf,
    pub source: ProjectIdentitySource,
    /// A credential-free canonical remote. Raw remote URLs are never retained.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub canonical_remote: Option<String>,
}

pub fn resolve_project_identity(start: &Path) -> ProjectIdentity {
    let start = if start.is_file() {
        start.parent().unwrap_or(start)
    } else {
        start
    };
    let canonical_start = start.canonicalize().unwrap_or_else(|_| absolute(start));
    let git_root = git_output(&canonical_start, &["rev-parse", "--show-toplevel"])
        .map(PathBuf::from)
        .and_then(|path| path.canonicalize().ok().or(Some(path)));
    let root = git_root.unwrap_or_else(|| canonical_start.clone());

    if let Some(remote) = git_output(&root, &["remote", "get-url", "origin"])
        .and_then(|remote| canonicalize_git_remote(&remote))
    {
        return ProjectIdentity {
            project_id: digest_identity("git-remote", &remote),
            root,
            source: ProjectIdentitySource::GitRemote,
            canonical_remote: Some(remote),
        };
    }

    let root_text = root.to_string_lossy().replace('\\', "/");
    ProjectIdentity {
        project_id: digest_identity("canonical-root", &root_text),
        root,
        source: ProjectIdentitySource::CanonicalRoot,
        canonical_remote: None,
    }
}

#[cfg(test)]
mod tests {
    use std::fs;

    use super::*;

    #[test]
    fn canonical_remote_strips_protocol_credentials_and_git_suffix() {
        assert_eq!(
            canonicalize_git_remote("https://token:secret@GitHub.COM/Owner/Repo.git?ignored=1"),
            Some("github.com/Owner/Repo".into())
        );
        assert_eq!(
            canonicalize_git_remote("git@github.com:Owner/Repo.git"),
            Some("github.com/Owner/Repo".into())
        );
        assert_eq!(
            canonicalize_git_remote("file:///definitely/not/a/relative/path"),
            Some("file:///definitely/not/a/relative/path".into())
        );
    }

    #[test]
    fn root_fallback_is_deterministic() {
        let path = std::env::temp_dir().join(format!("firmius-identity-{}", uuid::Uuid::new_v4()));
        fs::create_dir_all(&path).unwrap();
        let first = resolve_project_identity(&path);
        let second = resolve_project_identity(&path);
        assert_eq!(first.project_id, second.project_id);
        assert_eq!(first.source, ProjectIdentitySource::CanonicalRoot);
        fs::remove_dir_all(path).unwrap();
    }
}

fn git_output(root: &Path, args: &[&str]) -> Option<String> {
    let output = Command::new("git")
        .arg("-C")
        .arg(root)
        .args(args)
        .env("GIT_TERMINAL_PROMPT", "0")
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let value = String::from_utf8(output.stdout).ok()?;
    let value = value.trim();
    (!value.is_empty()).then(|| value.to_owned())
}

pub fn canonicalize_git_remote(remote: &str) -> Option<String> {
    let mut value = remote.trim().replace('\\', "/");
    if value.is_empty() {
        return None;
    }

    // SCP-style SSH syntax: git@host:owner/repository.git.
    if !value.contains("://") {
        if let Some((authority, path)) = value.split_once(':') {
            if authority.contains('@') && !path.starts_with('/') {
                let host = authority.rsplit('@').next()?.to_ascii_lowercase();
                return canonical_host_path(&host, path);
            }
        }
        // Local remotes are stable only relative to their canonical location.
        let path = PathBuf::from(&value);
        let canonical = path.canonicalize().unwrap_or(path);
        value = canonical.to_string_lossy().replace('\\', "/");
        return Some(format!("file://{}", value.trim_end_matches('/')));
    }

    let (_, remainder) = value.split_once("://")?;
    if value.starts_with("file://") {
        let path = PathBuf::from(if remainder.starts_with('/') {
            remainder.to_owned()
        } else {
            format!("/{remainder}")
        });
        let canonical = path.canonicalize().unwrap_or(path);
        return Some(format!(
            "file://{}",
            canonical.to_string_lossy().replace('\\', "/")
        ));
    }
    let without_suffix = remainder.split(['?', '#']).next().unwrap_or(remainder);
    let (authority, path) = without_suffix
        .split_once('/')
        .unwrap_or((without_suffix, ""));
    // Strip username/password/token material before the host.
    let host = authority.rsplit('@').next()?.to_ascii_lowercase();
    canonical_host_path(&host, path)
}

fn canonical_host_path(host: &str, path: &str) -> Option<String> {
    let host = host.trim().trim_end_matches('/');
    let path = path.trim().trim_matches('/').trim_end_matches(".git");
    if host.is_empty() || path.is_empty() {
        return None;
    }
    Some(format!("{host}/{path}"))
}

fn digest_identity(namespace: &str, value: &str) -> String {
    let digest = Sha256::digest(format!("firmius-project-v1\n{namespace}\n{value}").as_bytes());
    format!("project-{:x}", digest)
}

fn absolute(path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_owned()
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(path)
    }
}
