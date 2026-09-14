//! Prompt workflow file discovery shared by the desktop workflow browser.

use std::path::{Path, PathBuf};

use firmius_core::data_dir;

fn collect(root: &Path, files: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(root) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect(&path, files);
        } else if matches!(
            path.extension().and_then(|extension| extension.to_str()),
            Some("md" | "markdown" | "txt" | "workflow" | "yaml" | "yml" | "json" | "toml")
        ) {
            files.push(path);
        }
    }
}

pub(crate) fn discover() -> Vec<PathBuf> {
    let cwd = std::env::current_dir().unwrap_or_default();
    let roots = [
        cwd.join(".firmius").join("workflows"),
        cwd.join("workflows"),
        data_dir().join("workflows"),
    ];
    let mut files = Vec::new();
    for root in roots {
        collect(&root, &mut files);
    }
    files.sort();
    files.dedup();
    files
}
