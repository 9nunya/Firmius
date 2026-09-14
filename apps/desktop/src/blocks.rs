//! Structured transcript blocks; raw Markdown stays in the durable message.
use pulldown_cmark::{CodeBlockKind, Event, HeadingLevel, Options, Parser, Tag, TagEnd};
#[derive(Debug, PartialEq, Eq)]
pub struct Block {
    pub text: String,
    pub kind: &'static str,
}
pub fn render(source: &str, tool: bool) -> Vec<Block> {
    if tool {
        return source
            .lines()
            .map(|line| Block {
                text: line.into(),
                kind: if line.starts_with('+') {
                    "addition"
                } else if line.starts_with('-') {
                    "deletion"
                } else {
                    "code"
                },
            })
            .collect();
    }
    let mut blocks = Vec::new();
    let mut text = String::new();
    let mut kind = "paragraph";
    let mut list_numbers: Vec<Option<u64>> = Vec::new();
    let mut link_destination: Option<String> = None;
    let mut table_alignments: Vec<String> = Vec::new();
    let mut table_row: Vec<String> = Vec::new();
    let mut table_cell = String::new();
    let flush = |text: &mut String, kind, blocks: &mut Vec<Block>| {
        if !text.trim().is_empty() {
            blocks.push(Block {
                text: std::mem::take(text),
                kind,
            });
        }
    };
    let finish_cell = |table_cell: &mut String, table_row: &mut Vec<String>| {
        table_row.push(std::mem::take(table_cell).trim().to_owned());
    };
    let finish_row =
        |table_row: &mut Vec<String>, table_alignments: &[String], blocks: &mut Vec<Block>| {
            if table_row.is_empty() {
                return;
            }
            let cells: Vec<String> = table_row
                .drain(..)
                .enumerate()
                .map(|(index, cell)| {
                    let align = table_alignments
                        .get(index)
                        .map(String::as_str)
                        .unwrap_or("");
                    match align {
                        "center" => format!(" {cell} "),
                        "right" => format!("{cell} "),
                        _ => cell,
                    }
                })
                .collect();
            blocks.push(Block {
                text: format!("│ {} │", cells.join(" │ ")),
                kind: "table",
            });
        };
    for event in Parser::new_ext(
        source,
        Options::ENABLE_TASKLISTS | Options::ENABLE_STRIKETHROUGH | Options::ENABLE_TABLES,
    ) {
        match event {
            Event::Start(Tag::Heading { level, .. }) => {
                flush(&mut text, kind, &mut blocks);
                kind = match level {
                    HeadingLevel::H1 => "heading-1",
                    HeadingLevel::H2 => "heading-2",
                    _ => "heading-3",
                };
            }
            Event::Start(Tag::CodeBlock(fence)) => {
                flush(&mut text, kind, &mut blocks);
                kind = "code";
                if let CodeBlockKind::Fenced(language) = fence {
                    let language = language.trim();
                    if !language.is_empty() {
                        text.push_str(language);
                        text.push('\n');
                    }
                }
            }
            Event::Start(Tag::BlockQuote(_)) => {
                flush(&mut text, kind, &mut blocks);
                kind = "quote";
            }
            Event::Start(Tag::Table(alignments)) => {
                flush(&mut text, kind, &mut blocks);
                table_alignments = alignments
                    .into_iter()
                    .map(|align| format!("{align:?}").to_ascii_lowercase())
                    .collect();
            }
            Event::Start(Tag::TableHead) => {
                table_row.clear();
            }
            Event::Start(Tag::TableRow) => {
                table_row.clear();
            }
            Event::Start(Tag::TableCell) => table_cell.clear(),
            Event::Start(Tag::List(start)) => list_numbers.push(start),
            Event::Start(Tag::Item) => {
                flush(&mut text, kind, &mut blocks);
                kind = "list-item";
                let indent = "  ".repeat(list_numbers.len().saturating_sub(1));
                text.push_str(&indent);
                if let Some(Some(number)) = list_numbers.last_mut() {
                    text.push_str(&format!("{number}. "));
                    *number += 1;
                } else {
                    text.push_str("• ");
                }
            }
            Event::Start(Tag::Link { dest_url, .. }) => {
                link_destination = Some(dest_url.into_string())
            }
            Event::Text(value) | Event::Code(value) => {
                if !table_alignments.is_empty() && kind != "code" {
                    table_cell.push_str(&value);
                } else {
                    text.push_str(&value);
                }
            }
            Event::TaskListMarker(checked) => {
                text.push_str(if checked { "☑ " } else { "☐ " });
            }
            Event::SoftBreak | Event::HardBreak => {
                if !table_cell.is_empty() {
                    table_cell.push(' ');
                } else {
                    text.push('\n');
                }
            }
            Event::End(TagEnd::TableCell) => finish_cell(&mut table_cell, &mut table_row),
            Event::End(TagEnd::TableHead) => {
                finish_row(&mut table_row, &table_alignments, &mut blocks);
                if !table_alignments.is_empty() {
                    let rule = table_alignments
                        .iter()
                        .map(|_| "---")
                        .collect::<Vec<_>>()
                        .join("─┼─");
                    blocks.push(Block {
                        text: format!("├─{rule}─┤"),
                        kind: "table",
                    });
                }
            }
            Event::End(TagEnd::TableRow) => {
                finish_row(&mut table_row, &table_alignments, &mut blocks)
            }
            Event::End(TagEnd::Table) => {
                table_alignments.clear();
                table_row.clear();
                table_cell.clear();
                kind = "paragraph";
            }
            Event::End(TagEnd::Link) => {
                if let Some(destination) = link_destination.take() {
                    if !destination.is_empty() {
                        text.push_str(&format!(" ↗ {destination}"));
                    }
                }
            }
            Event::End(TagEnd::List(_)) => {
                flush(&mut text, kind, &mut blocks);
                list_numbers.pop();
                kind = "paragraph";
            }
            Event::End(
                TagEnd::Paragraph
                | TagEnd::Heading(_)
                | TagEnd::CodeBlock
                | TagEnd::BlockQuote(_)
                | TagEnd::Item,
            ) => {
                flush(&mut text, kind, &mut blocks);
                kind = "paragraph";
            }
            Event::Rule => blocks.push(Block {
                text: "────────────────".into(),
                kind: "quote",
            }),
            _ => {}
        }
    }
    flush(&mut text, kind, &mut blocks);
    blocks
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn headings_code_and_diff_are_distinct_blocks() {
        let blocks = render("# Result\n\nText\n\n```rust\nlet ok = true;\n```", false);
        assert_eq!(blocks[0].kind, "heading-1");
        assert_eq!(blocks[2].kind, "code");
        assert!(blocks[2].text.starts_with("rust\n"));
        assert!(blocks[2].text.contains("let ok = true;"));
        let diff = render("@@ change\n-old\n+new", true);
        assert_eq!(diff[1].kind, "deletion");
        assert_eq!(diff[2].kind, "addition");
    }

    #[test]
    fn semantic_lists_tasks_and_links_survive_projection() {
        let blocks = render(
            "## Steps\n\n1. first\n2. second\n\n- [x] shipped\n\n[docs](https://example.test)",
            false,
        );
        assert_eq!(blocks[0].kind, "heading-2");
        assert!(blocks.iter().any(|block| block.text == "1. first"));
        assert!(blocks.iter().any(|block| block.text.contains("☑ shipped")));
        assert!(
            blocks
                .iter()
                .any(|block| block.text == "docs ↗ https://example.test")
        );
    }

    #[test]
    fn tables_become_native_record_rows() {
        let blocks = render(
            "| File | State |\n| --- | --- |\n| app.slint | ready |\n| presenters.rs | live |",
            false,
        );
        let tables: Vec<_> = blocks
            .iter()
            .filter(|block| block.kind == "table")
            .collect();
        assert!(tables.len() >= 2);
        assert!(tables[0].text.contains("File"));
        assert!(
            tables
                .iter()
                .any(|block| block.text.contains("presenters.rs"))
        );
        assert!(blocks.iter().all(|block| !block.text.contains("| File |")));
    }
}
