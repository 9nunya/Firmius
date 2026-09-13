//! Discovery of user-configured SSH targets.
//!
//! `~/.ssh/config` is the alias inventory; `known_hosts` is a trust database
//! and may contain hashed names, so it is reported as verification metadata
//! rather than parsed as the host list.

use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SshHostEntry {
    pub alias: String,
    pub hostname: Option<String>,
    pub user: Option<String>,
    pub port: Option<u16>,
    pub configured: bool,
    pub known_hosts_present: bool,
}

pub fn discover() -> Result<Vec<SshHostEntry>, String> {
    let home = dirs::home_dir().ok_or_else(|| "home directory unavailable".to_string())?;
    discover_from(&home.join(".ssh/config"), &home.join(".ssh/known_hosts"))
}

pub fn discover_from(config: &Path, known_hosts: &Path) -> Result<Vec<SshHostEntry>, String> {
    let lines = load_config_lines(config)?;
    let mut out = Vec::new();
    let mut current: Vec<SshHostEntry> = Vec::new();
    for raw in lines {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut parts = line.split_whitespace();
        let Some(key) = parts.next() else { continue };
        let value = parts.collect::<Vec<_>>().join(" ");
        if key.eq_ignore_ascii_case("host") {
            out.append(&mut current);
            current = value
                .split_whitespace()
                .filter(|alias| {
                    !alias.is_empty()
                        && !alias.contains('*')
                        && !alias.contains('?')
                        && !alias.contains('[')
                        && !alias.starts_with('!')
                })
                .map(|alias| SshHostEntry {
                    alias: alias.into(),
                    hostname: None,
                    user: None,
                    port: None,
                    configured: true,
                    known_hosts_present: known_hosts.exists(),
                })
                .collect();
        } else if !current.is_empty() {
            for entry in &mut current {
                match key.to_ascii_lowercase().as_str() {
                    "hostname" => entry.hostname = Some(value.clone()),
                    "user" => entry.user = Some(value.clone()),
                    "port" => entry.port = value.parse().ok(),
                    _ => {}
                }
            }
        }
    }
    out.append(&mut current);

    // Config aliases are the preferred names, but known_hosts is still a
    // useful inventory when a host was added by another tool or the user
    // connects directly by hostname. Hashed and pattern entries cannot be
    // presented as actionable SSH targets, so skip them deliberately.
    if let Ok(known_text) = std::fs::read_to_string(known_hosts) {
        for raw in known_text.lines() {
            let line = raw.trim();
            if line.is_empty() || line.starts_with('#') || line.starts_with('@') {
                continue;
            }
            let mut fields = line.split_whitespace();
            let Some(hosts) = fields.next() else {
                continue;
            };
            // A valid known_hosts record has host patterns, a key type, and
            // usually a key. Requiring the key-type field avoids treating
            // arbitrary one-word lines as hostnames.
            if fields.next().is_none() {
                continue;
            }
            for raw_host in hosts.split(',') {
                let (host, port) = parse_known_host(raw_host);
                if host.is_empty()
                    || host.starts_with('|')
                    || host.contains('*')
                    || host.contains('?')
                    || out.iter().any(|entry| {
                        entry.alias == host || entry.hostname.as_deref() == Some(host.as_str())
                    })
                {
                    continue;
                }
                out.push(SshHostEntry {
                    alias: host.clone(),
                    hostname: Some(host),
                    user: None,
                    port,
                    configured: false,
                    known_hosts_present: true,
                });
            }
        }
    }
    Ok(out)
}

fn load_config_lines(path: &Path) -> Result<Vec<String>, String> {
    let mut visiting = HashSet::new();
    load_config_lines_inner(path, &mut visiting)
}

fn load_config_lines_inner(
    path: &Path,
    visiting: &mut HashSet<PathBuf>,
) -> Result<Vec<String>, String> {
    let canonical = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    if !visiting.insert(canonical.clone()) {
        return Ok(Vec::new());
    }
    let text = match std::fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            visiting.remove(&canonical);
            return Ok(Vec::new());
        }
        Err(error) => {
            visiting.remove(&canonical);
            return Err(format!("read {}: {error}", path.display()));
        }
    };
    let base = path.parent().unwrap_or_else(|| Path::new("."));
    let mut lines = Vec::new();
    for raw in text.lines() {
        let mut fields = raw.split_whitespace();
        let Some(key) = fields.next() else {
            lines.push(raw.to_string());
            continue;
        };
        if !key.eq_ignore_ascii_case("include") {
            lines.push(raw.to_string());
            continue;
        }
        for pattern in fields {
            for included in expand_include(base, pattern) {
                lines.extend(load_config_lines_inner(&included, visiting)?);
            }
        }
    }
    visiting.remove(&canonical);
    Ok(lines)
}

fn expand_include(base: &Path, pattern: &str) -> Vec<PathBuf> {
    let pattern = pattern.trim_matches('"').trim_matches('\'');
    let path = if let Some(rest) = pattern.strip_prefix("~/") {
        dirs::home_dir()
            .map(|home| home.join(rest))
            .unwrap_or_else(|| PathBuf::from(pattern))
    } else {
        let path = PathBuf::from(pattern);
        if path.is_absolute() {
            path
        } else {
            base.join(path)
        }
    };
    if !has_glob(&path) {
        return path.exists().then_some(path).into_iter().collect();
    }

    let directory = path.parent().unwrap_or_else(|| Path::new("."));
    let pattern_name = path
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or("");
    let Ok(entries) = std::fs::read_dir(directory) else {
        return Vec::new();
    };
    let mut matches = entries
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|candidate| candidate.is_file())
        .filter(|candidate| {
            candidate
                .file_name()
                .and_then(|name| name.to_str())
                .is_some_and(|name| simple_glob_match(pattern_name, name))
        })
        .collect::<Vec<_>>();
    matches.sort();
    matches
}

fn has_glob(path: &Path) -> bool {
    path.to_string_lossy()
        .chars()
        .any(|character| matches!(character, '*' | '?' | '['))
}

fn simple_glob_match(pattern: &str, value: &str) -> bool {
    fn matches(pattern: &[u8], value: &[u8]) -> bool {
        match pattern.first() {
            None => value.is_empty(),
            Some(b'*') => {
                matches(&pattern[1..], value)
                    || (!value.is_empty() && matches(pattern, &value[1..]))
            }
            Some(b'?') => !value.is_empty() && matches(&pattern[1..], &value[1..]),
            Some(byte) => value.first() == Some(byte) && matches(&pattern[1..], &value[1..]),
        }
    }
    matches(pattern.as_bytes(), value.as_bytes())
}

fn parse_known_host(raw: &str) -> (String, Option<u16>) {
    let raw = raw.trim();
    if let Some(inner) = raw.strip_prefix('[') {
        if let Some(end) = inner.find(']') {
            let host = &inner[..end];
            let port = inner[end + 1..]
                .strip_prefix(':')
                .and_then(|p| p.parse().ok());
            return (host.to_string(), port);
        }
    }
    (raw.to_string(), None)
}

#[allow(dead_code)]
fn _path_for(home: &Path) -> PathBuf {
    home.join(".ssh")
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parses_aliases_and_ignores_patterns() {
        let dir = std::env::temp_dir().join(format!("firmius-ssh-test-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&dir).unwrap();
        let config = dir.join("config");
        let known = dir.join("known_hosts");
        std::fs::write(&config, "Host dev\n  HostName example.com\n  User me\n  Port 2222\nHost *.corp\n  User ignored\nHost prod\n  HostName prod.example\n").unwrap();
        std::fs::write(&known, "hashed\n").unwrap();
        let hosts = discover_from(&config, &known).unwrap();
        assert_eq!(hosts.len(), 2);
        assert_eq!(hosts[0].alias, "dev");
        assert_eq!(hosts[0].port, Some(2222));
        assert!(hosts[0].known_hosts_present);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn expands_multiple_concrete_aliases_from_one_host_block() {
        let dir =
            std::env::temp_dir().join(format!("firmius-ssh-aliases-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&dir).unwrap();
        let config = dir.join("config");
        let known = dir.join("known_hosts");
        std::fs::write(
            &config,
            "Host build ci !private\n  HostName builder.example\n  User runner\n",
        )
        .unwrap();
        std::fs::write(&known, "").unwrap();
        let hosts = discover_from(&config, &known).unwrap();
        assert_eq!(
            hosts
                .iter()
                .map(|host| host.alias.as_str())
                .collect::<Vec<_>>(),
            ["build", "ci"]
        );
        assert_eq!(hosts[1].hostname.as_deref(), Some("builder.example"));
        assert_eq!(hosts[1].user.as_deref(), Some("runner"));
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn merges_concrete_known_hosts_without_duplicates_or_hashed_entries() {
        let dir = std::env::temp_dir().join(format!("firmius-ssh-test-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&dir).unwrap();
        let config = dir.join("config");
        let known = dir.join("known_hosts");
        std::fs::write(&config, "Host dev\n  HostName example.com\n").unwrap();
        std::fs::write(
            &known,
            "example.com ssh-ed25519 AAAA\n[remote.example]:2200 ssh-ed25519 BBBB\n|1|hashed ssh-ed25519 CCCC\n*.wildcard ssh-ed25519 DDDD\n",
        )
        .unwrap();

        let hosts = discover_from(&config, &known).unwrap();
        assert_eq!(
            hosts
                .iter()
                .filter(|entry| entry.alias == "example.com")
                .count(),
            0
        );
        let remote = hosts
            .iter()
            .find(|entry| entry.alias == "remote.example")
            .unwrap();
        assert_eq!(remote.port, Some(2200));
        assert!(!remote.configured);
        assert!(!hosts.iter().any(|entry| entry.alias.starts_with('|')));
        assert!(!hosts.iter().any(|entry| entry.alias.contains("wildcard")));
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn expands_nested_include_files_and_ignores_include_cycles() {
        let dir = std::env::temp_dir().join(format!("firmius-ssh-test-{}", uuid::Uuid::new_v4()));
        let fragments = dir.join("conf.d");
        std::fs::create_dir_all(&fragments).unwrap();
        let config = dir.join("config");
        let first = fragments.join("01-first.conf");
        let second = fragments.join("02-second.conf");
        let known = dir.join("known_hosts");
        std::fs::write(&config, "Include conf.d/*.conf\nInclude config\n").unwrap();
        std::fs::write(&first, "Host included-one\n  HostName one.example\n").unwrap();
        std::fs::write(
            &second,
            "Host included-two\n  HostName two.example\n  Port 2201\n",
        )
        .unwrap();
        std::fs::write(&known, "").unwrap();

        let hosts = discover_from(&config, &known).unwrap();
        assert_eq!(hosts.len(), 2);
        assert_eq!(hosts[1].port, Some(2201));
        assert_eq!(hosts[0].hostname.as_deref(), Some("one.example"));
        let _ = std::fs::remove_dir_all(dir);
    }
}
