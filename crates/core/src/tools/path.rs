//! Shared filesystem path containment checks for tools.
use std::path::{Path, PathBuf};

fn windows_absolute(path: &str) -> bool {
    let p = path.replace('\\', "/");
    path.starts_with("\\\\")
        || path.starts_with("\\\\.\\")
        || path.starts_with("\\\\?\\")
        || p.starts_with('/')
        || p.starts_with("//")
        || (p.as_bytes().get(1) == Some(&b':')
            && p.as_bytes()
                .first()
                .is_some_and(|b| b.is_ascii_alphabetic()))
}

/// Resolve an existing directory supplied as a tool cwd. Absolute paths are
/// accepted only when they remain inside the workspace; relative paths retain
/// the normal relative-only and symlink-safe behavior.
pub fn existing_directory(workdir: &Path, path: &str) -> Result<PathBuf, String> {
    let resolved = if Path::new(path).is_absolute() {
        existing_read(workdir, path)?
    } else {
        existing(workdir, path)?
    };
    if !resolved.is_dir() {
        return Err(format!("cwd is not a directory: '{path}'"));
    }
    Ok(resolved)
}

/// Resolve a directory supplied to a read-only walker.
pub fn directory_read(workdir: &Path, path: Option<&str>) -> Result<PathBuf, String> {
    match path {
        None | Some("") | Some(".") => std::fs::canonicalize(workdir).map_err(|e| e.to_string()),
        Some(p) => existing_read(workdir, p),
    }
}

/// Resolve an existing path for read-only tools. Unlike [`existing`], this
/// accepts an absolute path when it is contained by `workdir`; all symlink
/// components and lexical traversal remain prohibited.
pub fn existing_read(workdir: &Path, path: &str) -> Result<PathBuf, String> {
    let root = std::fs::canonicalize(workdir)
        .map_err(|e| format!("cannot resolve workdir {}: {e}", workdir.display()))?;
    let candidate = if std::path::Path::new(path).is_absolute() {
        let absolute = absolute_read_path(path)?;
        // Preserve the no-symlink policy for components supplied beneath the
        // declared workdir. (System aliases such as macOS /var are outside
        // this relative portion and are handled by canonicalization below.)
        if let Ok(relative) = absolute.strip_prefix(workdir) {
            let mut current = workdir.to_path_buf();
            for component in relative.components() {
                current.push(component.as_os_str());
                if std::fs::symlink_metadata(&current)
                    .map(|metadata| metadata.file_type().is_symlink())
                    .unwrap_or(false)
                {
                    return Err(format!(
                        "symlink path component is not allowed: {}",
                        current.display()
                    ));
                }
            }
        }
        // On platforms such as macOS, user-visible absolute paths can pass
        // through a system alias (/var -> /private/var). Canonicalize the
        // existing target before comparing it with the canonical workdir.
        if absolute.exists() {
            std::fs::canonicalize(&absolute)
                .map_err(|e| format!("cannot resolve path {}: {e}", absolute.display()))?
        } else {
            absolute
        }
    } else {
        let rel = lexical(path)?;
        root.join(rel)
    };
    let canonical = contained(&root, &candidate)?;
    if !candidate.exists() {
        return Err(format!("path does not exist: '{path}'"));
    }
    Ok(canonical)
}

/// Validate an absolute read path without making absolute paths generally
/// available to mutating tools.  Parent components are rejected rather than
/// normalized so callers cannot use lexical traversal to bypass the workdir
/// boundary.
fn absolute_read_path(path: &str) -> Result<PathBuf, String> {
    if path.trim().is_empty() || !std::path::Path::new(path).is_absolute() {
        return Err(format!("path must be absolute: '{path}'"));
    }
    for component in path.replace('\\', "/").split('/') {
        if component == ".." {
            return Err(format!("path traversal is not allowed: '{path}'"));
        }
    }
    Ok(PathBuf::from(path))
}

fn lexical(path: &str) -> Result<PathBuf, String> {
    if path.trim().is_empty() || windows_absolute(path) {
        return Err(format!("path must be relative to the workdir: '{path}'"));
    }
    let mut out = PathBuf::new();
    for component in path.replace('\\', "/").split('/') {
        match component {
            "" | "." => {}
            ".." => return Err(format!("path traversal is not allowed: '{path}'")),
            c => out.push(c),
        }
    }
    if out.as_os_str().is_empty() && path.trim() != "." {
        return Err(format!("empty path: '{path}'"));
    }
    Ok(out)
}

fn contained(root: &Path, candidate: &Path) -> Result<PathBuf, String> {
    let root = std::fs::canonicalize(root)
        .map_err(|e| format!("cannot resolve workdir {}: {e}", root.display()))?;
    let relative = candidate
        .strip_prefix(&root)
        .map_err(|_| format!("path escapes workdir: {}", candidate.display()))?;
    let mut result = root.clone();
    for component in relative.components() {
        let name = component.as_os_str();
        let next = result.join(name);
        match std::fs::symlink_metadata(&next) {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                // A destination symlink (including a dangling one) must never
                // be followed by a later write. Reject it rather than
                // canonicalizing only the nearest existing ancestor.
                return Err(format!(
                    "symlink path component is not allowed: {}",
                    next.display()
                ));
            }
            Ok(_) => {
                let canonical = std::fs::canonicalize(&next).map_err(|e| {
                    format!("cannot resolve path component {}: {e}", next.display())
                })?;
                if !canonical.starts_with(&root) {
                    return Err(format!("path escapes workdir: {}", candidate.display()));
                }
                result = canonical;
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                // The rest of the path is non-existent. Keep it lexical, but
                // continue checking no already-existing component was a
                // symlink (handled above).
                result.push(name);
            }
            Err(error) => {
                return Err(format!(
                    "cannot inspect path component {}: {error}",
                    next.display()
                ));
            }
        }
    }
    Ok(result)
}

/// Resolve an existing path and reject symlink/junction escapes.
pub fn existing(workdir: &Path, path: &str) -> Result<PathBuf, String> {
    let rel = lexical(path)?;
    let candidate = workdir.join(rel);
    let canonical = destination(workdir, path)?;
    if !candidate.exists() {
        return Err(format!("path does not exist: '{path}'"));
    }
    Ok(canonical)
}

/// Resolve a destination, validating its nearest existing ancestor.
pub fn destination(workdir: &Path, path: &str) -> Result<PathBuf, String> {
    let rel = lexical(path)?;
    let root = std::fs::canonicalize(workdir)
        .map_err(|e| format!("cannot resolve workdir {}: {e}", workdir.display()))?;
    contained(&root, &root.join(rel))
}

/// Resolve a user-supplied directory for read-only walkers.
pub fn directory(workdir: &Path, path: Option<&str>) -> Result<PathBuf, String> {
    match path {
        None | Some("") | Some(".") => std::fs::canonicalize(workdir).map_err(|e| e.to_string()),
        Some(p) => existing(workdir, p),
    }
}
