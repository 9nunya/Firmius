//! Semantic tool content, parsed from the tools' actual documented formats.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PatchLine {
    pub text: String,
    pub kind: String,
    pub old: String,
    pub new: String,
}

pub(crate) fn omitted_notice(output: &str) -> Option<&str> {
    output.lines().find(|line| {
        let lower = line.to_ascii_lowercase();
        lower.contains("truncated")
            || lower.contains("matches omitted")
            || lower.contains("bytes omitted")
    })
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct FileChange {
    pub path: String,
    pub operation: String,
    pub added: usize,
    pub removed: usize,
    pub lines: Vec<PatchLine>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct SearchMatch {
    pub path: String,
    pub line: usize,
    pub text: String,
}

pub(crate) fn patch_files(patch: &str) -> Vec<FileChange> {
    let mut files: Vec<FileChange> = Vec::new();
    let (mut old, mut new) = (None::<usize>, None::<usize>);
    for line in patch.lines() {
        let header = [
            ("*** Add File: ", "added"),
            ("*** Update File: ", "modified"),
            ("*** Delete File: ", "deleted"),
        ]
        .iter()
        .find_map(|(prefix, operation)| line.strip_prefix(prefix).map(|path| (path, *operation)));
        if let Some((path, operation)) = header {
            files.push(FileChange {
                path: path.into(),
                operation: operation.into(),
                added: 0,
                removed: 0,
                lines: vec![],
            });
            old = None;
            new = if operation == "added" { Some(1) } else { None };
            continue;
        }
        if let Some(path) = line
            .strip_prefix("diff --git a/")
            .and_then(|rest| rest.split_once(" b/").map(|(_, path)| path))
        {
            files.push(FileChange {
                path: path.into(),
                operation: "modified".into(),
                added: 0,
                removed: 0,
                lines: vec![],
            });
            old = None;
            new = None;
            continue;
        }
        let Some(file) = files.last_mut() else {
            continue;
        };
        if let Some(destination) = line.strip_prefix("*** Move to: ") {
            file.operation = format!("moved to {destination}");
            continue;
        }
        if line.starts_with("*** ") {
            continue;
        }
        if line.starts_with("new file mode ") {
            file.operation = "added".into();
            continue;
        }
        if line.starts_with("deleted file mode ") {
            file.operation = "deleted".into();
            continue;
        }
        if let Some(destination) = line.strip_prefix("rename to ") {
            file.operation = format!("moved to {destination}");
            continue;
        }
        if line.starts_with("--- ") || line.starts_with("+++ ") || line.starts_with("index ") {
            continue;
        }
        if line.starts_with("@@") {
            let mut tokens = line.split_whitespace();
            tokens.next();
            old = tokens
                .next()
                .and_then(|s| s.strip_prefix('-'))
                .and_then(|s| s.split(',').next())
                .and_then(|s| s.parse().ok());
            new = tokens
                .next()
                .and_then(|s| s.strip_prefix('+'))
                .and_then(|s| s.split(',').next())
                .and_then(|s| s.parse().ok());
            file.lines.push(PatchLine {
                text: line.into(),
                kind: "hunk".into(),
                old: String::new(),
                new: String::new(),
            });
            continue;
        }
        let (kind, content) = match line.as_bytes().first() {
            Some(b'+') => {
                file.added += 1;
                ("addition", &line[1..])
            }
            Some(b'-') => {
                file.removed += 1;
                ("deletion", &line[1..])
            }
            Some(b' ') => ("context", &line[1..]),
            _ => continue,
        };
        file.lines.push(PatchLine {
            text: content.into(),
            kind: kind.into(),
            old: if kind != "addition" {
                old.map(|n| n.to_string()).unwrap_or_default()
            } else {
                String::new()
            },
            new: if kind != "deletion" {
                new.map(|n| n.to_string()).unwrap_or_default()
            } else {
                String::new()
            },
        });
        if kind != "addition" {
            old = old.map(|n| n + 1);
        }
        if kind != "deletion" {
            new = new.map(|n| n + 1);
        }
    }
    files
}
pub(crate) fn listed_paths(output: &str) -> Vec<SearchMatch> {
    output
        .lines()
        .map(str::trim)
        .filter(|line| {
            !line.is_empty()
                && omitted_notice(line).is_none()
                && !line.eq_ignore_ascii_case("empty")
                && !line.eq_ignore_ascii_case("no matches")
        })
        .map(|path| SearchMatch {
            path: path.to_owned(),
            line: 0,
            text: String::new(),
        })
        .collect()
}

pub(crate) fn search_matches(output: &str) -> Vec<SearchMatch> {
    output
        .lines()
        .filter_map(|text| {
            // A path may itself contain colons (Windows drives and artifact URIs).
            for (index, _) in text.match_indices(':') {
                let rest = &text[index + 1..];
                let Some((number, content)) = rest.split_once(':') else {
                    continue;
                };
                if let Ok(line) = number.parse::<usize>() {
                    if line > 0 && index > 0 {
                        return Some(SearchMatch {
                            path: text[..index].into(),
                            line,
                            text: content.trim_start().into(),
                        });
                    }
                }
            }
            None
        })
        .collect()
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn actual_apply_patch_format_preserves_files_hunks_and_unknown_line_numbers() {
        let files = patch_files(
            "*** Begin Patch\n*** Update File: a.rs\n@@ fn main\n-old\n+new\n*** Add File: b.rs\n+hello\n*** Delete File: c.rs\n*** End Patch",
        );
        assert_eq!(files.len(), 3);
        assert_eq!(files[0].added, 1);
        assert_eq!(files[0].removed, 1);
        assert_eq!(files[0].lines[1].old, "");
        assert_eq!(files[1].lines[0].new, "1");
        assert_eq!(files[2].operation, "deleted");
    }
    #[test]
    fn matches_preserve_colons_and_skip_non_results() {
        let matches = search_matches(
            "C:\\src\\file.rs:12: let x = 1;\nartifact://report:8: found\nNo matches\n",
        );
        assert_eq!(matches.len(), 2);
        assert_eq!(matches[0].path, "C:\\src\\file.rs");
        assert_eq!(matches[1].path, "artifact://report");
        assert_eq!(matches[1].line, 8);
    }

    #[test]
    fn unified_diff_tracks_files_counts_and_renames() {
        let files = patch_files(
            "diff --git a/old.rs b/new.rs\nsimilarity index 90%\nrename from old.rs\nrename to new.rs\n--- a/old.rs\n+++ b/new.rs\n@@ -2,2 +2,2 @@\n-old\n+new\n same",
        );
        assert_eq!(files.len(), 1);
        assert_eq!(files[0].path, "new.rs");
        assert_eq!(files[0].operation, "moved to new.rs");
        assert_eq!((files[0].added, files[0].removed), (1, 1));
        assert_eq!(files[0].lines[1].old, "2");
        assert_eq!(files[0].lines[2].new, "2");
    }

    #[test]
    fn truncation_disclosure_is_detected() {
        assert_eq!(
            omitted_notice("one\n[...truncated at 200 matches...]"),
            Some("[...truncated at 200 matches...]")
        );
    }

    #[test]
    fn listed_paths_skip_empty_and_truncation_rows() {
        let listed =
            listed_paths("src/a.rs\nempty\n[...truncated at 500 matches...]\nartifact://report");
        assert_eq!(listed.len(), 2);
        assert_eq!(listed[0].path, "src/a.rs");
        assert_eq!(listed[0].line, 0);
        assert_eq!(listed[1].path, "artifact://report");
    }
}
