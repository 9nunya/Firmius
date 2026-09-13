//! Discovery and loading of user-authored workflow prompt files.
//!
//! Workflows are intentionally just text files.  This keeps the feature useful
//! for prompt templates as well as exported plans, and means a workflow can be
//! reviewed and edited with the user's normal tools.  The TUI searches the
//! project-local directories first, followed by the global Firmius directory.

use std::collections::HashSet;
use std::path::{Path, PathBuf};

use firmius_core::data_dir;

/// A workflow file available to the TUI.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkflowFile {
    pub path: PathBuf,
    /// Stable, human-readable path shown by the picker.
    pub label: String,
}

fn is_workflow_file(path: &Path) -> bool {
    if !path.is_file() {
        return false;
    }
    // Markdown is the common format, while accepting the data formats makes
    // exported/hand-authored task graphs discoverable without pretending that
    // Firmius parses them in the TUI. Files without an extension are omitted
    // to avoid surfacing editor locks and unrelated project files.
    matches!(
        path.extension()
            .and_then(|ext| ext.to_str())
            .map(|ext| ext.to_ascii_lowercase())
            .as_deref(),
        Some("md" | "markdown" | "txt" | "workflow" | "yaml" | "yml" | "json" | "toml")
    )
}

fn visit(
    root: &Path,
    boundary: &Path,
    out: &mut Vec<PathBuf>,
    seen: &mut HashSet<PathBuf>,
    depth: usize,
) {
    if depth > 4 {
        return;
    }
    let Ok(canonical) = std::fs::canonicalize(root) else {
        return;
    };
    if !canonical.starts_with(boundary) || !seen.insert(canonical.clone()) {
        return;
    }
    let Ok(entries) = std::fs::read_dir(root) else {
        return;
    };
    let mut entries = entries.flatten().collect::<Vec<_>>();
    entries.sort_by_key(|entry| entry.file_name());
    for entry in entries {
        let path = entry.path();
        let Ok(metadata) = std::fs::symlink_metadata(&path) else {
            continue;
        };
        if metadata.file_type().is_symlink() {
            continue;
        }
        if metadata.is_dir() {
            visit(&path, boundary, out, seen, depth + 1);
        } else if is_workflow_file(&path) {
            if let Ok(file) = std::fs::canonicalize(&path)
                && file.starts_with(boundary)
            {
                out.push(file);
            }
        }
    }
}

/// Return the standard project and global workflow roots.
pub fn default_roots() -> Vec<PathBuf> {
    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    vec![
        cwd.join(".firmius").join("workflows"),
        cwd.join("workflows"),
        data_dir().join("workflows"),
    ]
}

/// Discover workflow files below `roots`, de-duplicating paths and sorting by
/// their displayed path. Missing or unreadable roots are simply absent; the
/// picker can therefore be used before any workflow directory is created.
pub fn discover_from(roots: &[PathBuf]) -> Vec<WorkflowFile> {
    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let mut paths = Vec::new();
    let mut seen = HashSet::new();
    for root in roots {
        let Ok(boundary) = std::fs::canonicalize(root) else {
            continue;
        };
        visit(&boundary, &boundary, &mut paths, &mut seen, 0);
    }
    paths.sort();
    paths.dedup();
    let mut files = paths
        .into_iter()
        .map(|path| {
            let label = path
                .strip_prefix(&cwd)
                .map(|relative| relative.display().to_string())
                .unwrap_or_else(|_| path.display().to_string());
            WorkflowFile { path, label }
        })
        .collect::<Vec<_>>();
    files.sort_by(|a, b| a.label.cmp(&b.label));
    files
}

/// Discover workflow files in the standard locations.
pub fn discover() -> Vec<WorkflowFile> {
    discover_from(&default_roots())
}

/// Resolve and read a workflow selected by the user. Exact paths are accepted
/// in addition to picker labels, which makes `/workflow insert path/to/file.md`
/// useful for scripts and keyboard users alike.
pub fn read(path: &str) -> Result<String, String> {
    let requested = PathBuf::from(path);
    let matches = discover()
        .into_iter()
        .filter(|file| {
            file.label == path
                || file.path.to_string_lossy() == path
                || (requested.is_file()
                    && std::fs::canonicalize(&requested).ok().as_ref() == Some(&file.path))
        })
        .collect::<Vec<_>>();
    let selected = match matches.as_slice() {
        [file] => file.path.clone(),
        [] => return Err(format!("workflow not found: {path}")),
        _ => return Err(format!("workflow path is ambiguous: {path}")),
    };
    let content = std::fs::read_to_string(&selected)
        .map_err(|error| format!("read workflow {}: {error}", selected.display()))?;
    if content.trim().is_empty() {
        return Err(format!("workflow is empty: {}", selected.display()));
    }
    Ok(content)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_root(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "firmius-workflows-{name}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ))
    }

    #[test]
    fn discovers_supported_files_recursively_and_ignores_other_files() {
        let root = temp_root("discover");
        std::fs::create_dir_all(root.join("nested")).unwrap();
        std::fs::write(root.join("a.md"), "A").unwrap();
        std::fs::write(root.join("nested/b.workflow"), "B").unwrap();
        std::fs::write(root.join("ignore.rs"), "no").unwrap();
        let files = discover_from(std::slice::from_ref(&root));
        assert_eq!(files.len(), 2);
        assert!(files.iter().any(|file| file.label.ends_with("a.md")));
        assert!(files.iter().any(|file| file.label.ends_with("b.workflow")));
        std::fs::remove_dir_all(root).ok();
    }

    #[test]
    fn read_rejects_empty_workflows() {
        let root = temp_root("empty");
        std::fs::create_dir_all(&root).unwrap();
        let path = root.join("empty.md");
        std::fs::write(&path, " \n").unwrap();
        let error = read(path.to_str().unwrap()).unwrap_err();
        assert!(error.contains("empty"));
        std::fs::remove_dir_all(root).ok();
    }

    #[test]
    fn discover_deduplicates_paths_and_sorts_labels() {
        let root = temp_root("dedupe");
        std::fs::create_dir_all(&root).unwrap();
        let path = root.join("same.md");
        std::fs::write(&path, "prompt").unwrap();
        let files = discover_from(&[root.clone(), root.clone()]);
        assert_eq!(files.len(), 1);
        assert!(files[0].label.ends_with("same.md"));
        std::fs::remove_dir_all(root).ok();
    }

    #[cfg(unix)]
    #[test]
    fn discovery_does_not_follow_symlinked_directories() {
        use std::os::unix::fs::symlink;
        let root = temp_root("symlink");
        let outside = temp_root("outside");
        std::fs::create_dir_all(&root).unwrap();
        std::fs::create_dir_all(&outside).unwrap();
        std::fs::write(outside.join("escape.md"), "outside").unwrap();
        symlink(&outside, root.join("linked")).unwrap();
        assert!(discover_from(std::slice::from_ref(&root)).is_empty());
        std::fs::remove_dir_all(root).ok();
        std::fs::remove_dir_all(outside).ok();
    }
}
