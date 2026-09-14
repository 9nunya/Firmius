//! Bounded terminal previews: SGR styling and in-line cursor updates.
use super::*;
use unicode_width::UnicodeWidthChar;

#[derive(Clone, Copy, PartialEq)]
struct Style {
    color: slint::Color,
    bold: bool,
}
impl Default for Style {
    fn default() -> Self {
        Self {
            color: slint::Color::from_rgb_u8(231, 238, 231),
            bold: false,
        }
    }
}
fn palette(n: u16) -> slint::Color {
    let colors = [
        0x202622, 0xe78383, 0x94dcb1, 0xe4ca86, 0x8eb5ed, 0xc5a0df, 0x8bd0d2, 0xd5ded6, 0x7f9183,
        0xffaaaa, 0xb4efc7, 0xf6e2a5, 0xb3ceff, 0xe2bcf5, 0xb0eeec, 0xffffff,
    ];
    let rgb = if n < 16 {
        colors[n as usize]
    } else if n < 232 {
        let n = n - 16;
        let channel = |v: u16| if v == 0 { 0 } else { 55 + v as u32 * 40 };
        channel(n / 36) << 16 | channel(n / 6 % 6) << 8 | channel(n % 6)
    } else {
        let v = 8 + (n.min(255) as u32 - 232) * 10;
        v << 16 | v << 8 | v
    };
    slint::Color::from_rgb_u8((rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8)
}

#[derive(Clone)]
struct Cell {
    text: String,
    style: Style,
    continuation: bool,
}

fn blank(style: Style) -> Cell {
    Cell {
        text: " ".into(),
        style,
        continuation: false,
    }
}

fn put(line: &mut Vec<Cell>, cursor: &mut usize, value: char, style: Style) {
    let width = UnicodeWidthChar::width(value).unwrap_or(0);
    if width == 0 {
        let end = (*cursor).min(line.len());
        if let Some(cell) = line[..end].iter_mut().rev().find(|cell| !cell.continuation) {
            cell.text.push(value);
        }
        return;
    }
    if *cursor > 8192 || width > 2 {
        return;
    }
    while line.len() < *cursor + width {
        line.push(blank(style));
    }
    // Writing onto either half of a wide glyph replaces the complete glyph.
    // Cursor motion can land on a continuation cell; snap back to the start
    // so a replacement does not leave a leading blank column.
    if line[*cursor].continuation && *cursor > 0 {
        *cursor -= 1;
    }
    if *cursor + 1 < line.len() && line[*cursor + 1].continuation {
        line[*cursor + 1] = blank(style);
    }
    line[*cursor] = Cell {
        text: value.to_string(),
        style,
        continuation: false,
    };
    if width == 2 {
        line[*cursor + 1] = Cell {
            text: String::new(),
            style,
            continuation: true,
        };
    }
    *cursor += width;
}

pub(super) fn render(text: &str) -> Vec<TerminalLine> {
    let mut lines: Vec<Vec<Cell>> = vec![vec![]];
    let mut row = 0usize;
    let mut cursor = 0usize;
    let mut style = Style::default();
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '\u{1b}' => match chars.next() {
                Some('[') => {
                    let mut params = String::new();
                    let mut final_char = None;
                    for c in chars.by_ref() {
                        if ('@'..='~').contains(&c) {
                            final_char = Some(c);
                            break;
                        }
                        params.push(c);
                    }
                    let codes: Vec<u16> =
                        params.split(';').map(|v| v.parse().unwrap_or(0)).collect();
                    match final_char {
                        Some('m') => {
                            let mut i = 0;
                            while i < codes.len() {
                                match codes[i] {
                                    0 => style = Style::default(),
                                    1 => style.bold = true,
                                    22 => style.bold = false,
                                    30..=37 => style.color = palette(codes[i] - 30),
                                    90..=97 => style.color = palette(codes[i] - 90 + 8),
                                    39 => style.color = Style::default().color,
                                    38 if codes.get(i + 1) == Some(&5) && codes.len() > i + 2 => {
                                        style.color = palette(codes[i + 2]);
                                        i += 2;
                                    }
                                    38 if codes.get(i + 1) == Some(&2) && codes.len() > i + 4 => {
                                        style.color = slint::Color::from_rgb_u8(
                                            codes[i + 2].min(255) as u8,
                                            codes[i + 3].min(255) as u8,
                                            codes[i + 4].min(255) as u8,
                                        );
                                        i += 4;
                                    }
                                    _ => {}
                                }
                                i += 1;
                            }
                        }
                        Some('K') => {
                            let line = &mut lines[row];
                            match codes.first().copied().unwrap_or(0) {
                                0 => line.truncate(cursor),
                                1 => {
                                    let end = cursor.min(line.len().saturating_sub(1));
                                    for cell in line.iter_mut().take(end + 1) {
                                        *cell = blank(style);
                                    }
                                }
                                2 => line.clear(),
                                _ => {}
                            }
                        }
                        Some('J') => match codes.first().copied().unwrap_or(0) {
                            0 => {
                                lines[row].truncate(cursor);
                                lines.truncate(row + 1);
                            }
                            1 => {
                                for line in lines.iter_mut().take(row) {
                                    line.clear();
                                }
                                let end = cursor.min(lines[row].len().saturating_sub(1));
                                for cell in lines[row].iter_mut().take(end + 1) {
                                    *cell = blank(style);
                                }
                            }
                            2 | 3 => {
                                lines.clear();
                                lines.push(vec![]);
                                row = 0;
                                cursor = 0;
                            }
                            _ => {}
                        },
                        Some('G') | Some('`') => {
                            cursor = codes.first().copied().unwrap_or(1).max(1) as usize - 1
                        }
                        Some('C') => {
                            cursor = cursor
                                .saturating_add(codes.first().copied().unwrap_or(1).max(1) as usize)
                                .min(8192)
                        }
                        Some('D') => {
                            cursor = cursor
                                .saturating_sub(codes.first().copied().unwrap_or(1).max(1) as usize)
                        }
                        Some('A') => {
                            row = row
                                .saturating_sub(codes.first().copied().unwrap_or(1).max(1) as usize)
                        }
                        Some('B') => {
                            row = row
                                .saturating_add(codes.first().copied().unwrap_or(1).max(1) as usize)
                                .min(8192);
                            while lines.len() <= row {
                                lines.push(vec![]);
                            }
                        }
                        Some('H') | Some('f') => {
                            row = codes.first().copied().unwrap_or(1).max(1) as usize - 1;
                            cursor = codes.get(1).copied().unwrap_or(1).max(1) as usize - 1;
                            row = row.min(8192);
                            cursor = cursor.min(8192);
                            while lines.len() <= row {
                                lines.push(vec![]);
                            }
                        }
                        Some('d') => {
                            row = codes.first().copied().unwrap_or(1).max(1) as usize - 1;
                            row = row.min(8192);
                            while lines.len() <= row {
                                lines.push(vec![]);
                            }
                        }
                        Some('X') => {
                            let count = codes.first().copied().unwrap_or(1).max(1) as usize;
                            let line = &mut lines[row];
                            while line.len() < cursor + count {
                                line.push(blank(style));
                            }
                            for cell in line.iter_mut().skip(cursor).take(count) {
                                *cell = blank(style);
                            }
                        }
                        Some('P') => {
                            let count = codes.first().copied().unwrap_or(1).max(1) as usize;
                            let line = &mut lines[row];
                            let start = cursor.min(line.len());
                            let end = (start + count).min(line.len());
                            line.drain(start..end);
                        }
                        Some('@') => {
                            let count = codes.first().copied().unwrap_or(1).max(1) as usize;
                            let line = &mut lines[row];
                            let start = cursor.min(line.len());
                            for _ in 0..count {
                                line.insert(start, blank(style));
                            }
                        }
                        _ => {}
                    }
                }
                Some(']') => {
                    while let Some(c) = chars.next() {
                        if c == '\u{7}' || (c == '\u{1b}' && chars.next() == Some('\\')) {
                            break;
                        }
                    }
                }
                _ => {}
            },
            '\r' => cursor = 0,
            '\n' => {
                row += 1;
                while lines.len() <= row {
                    lines.push(vec![]);
                }
                cursor = 0;
            }
            '\u{8}' => cursor = cursor.saturating_sub(1),
            c if c.is_control() && c != '\t' => {}
            c => {
                let count = if c == '\t' { 8 - cursor % 8 } else { 1 };
                let c = if c == '\t' { ' ' } else { c };
                let line = &mut lines[row];
                for _ in 0..count {
                    put(line, &mut cursor, c, style);
                }
            }
        }
    }
    let start = lines.len().saturating_sub(80);
    lines
        .into_iter()
        .skip(start)
        .map(|line| {
            let mut spans: Vec<TerminalSpan> = Vec::new();
            let mut text = String::new();
            let mut previous = Style::default();
            for cell in line {
                if cell.continuation {
                    continue;
                }
                if cell.style != previous && !text.is_empty() {
                    spans.push(TerminalSpan {
                        text: std::mem::take(&mut text).into(),
                        ink: previous.color,
                        strong: previous.bold,
                    });
                }
                previous = cell.style;
                text.push_str(&cell.text);
            }
            spans.push(TerminalSpan {
                text: text.into(),
                ink: previous.color,
                strong: previous.bold,
            });
            TerminalLine {
                spans: ModelRc::new(VecModel::from(spans)),
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn colors_reset_and_progress_overwrites_without_escape_leaks() {
        let rows = render("\x1b[31mfail\x1b[0m ok\nprogress 1\r\x1b[2Kdone");
        let first: Vec<_> = rows[0].spans.iter().collect();
        assert_eq!(first[0].text, "fail");
        assert_ne!(first[0].ink, first[1].ink);
        assert_eq!(rows[1].spans.row_data(0).unwrap().text, "done");
    }
    #[test]
    fn truecolor_and_incomplete_sequences() {
        let rows = render("\x1b[38;2;12;34;56mtext\x1b[3");
        let span = rows[0].spans.row_data(0).unwrap();
        assert_eq!(span.text, "text");
        assert_eq!(span.ink, slint::Color::from_rgb_u8(12, 34, 56));
    }

    #[test]
    fn unicode_width_combining_marks_and_cursor_motion_share_terminal_cells() {
        let rows = render("界x\x1b[2D好e\u{301}");
        let text = rows[0]
            .spans
            .iter()
            .map(|span| span.text.to_string())
            .collect::<String>();
        assert_eq!(text, "好e\u{301}");
    }

    #[test]
    fn cursor_addressing_and_all_erase_modes_are_applied() {
        let rows = render("first\nsecond\x1b[1A\x1b[2G\x1b[0KX");
        assert_eq!(rows[0].spans.row_data(0).unwrap().text, "fX");
        assert_eq!(rows[1].spans.row_data(0).unwrap().text, "second");

        let rows = render("old\ncontent\x1b[2Jnew");
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].spans.row_data(0).unwrap().text, "new");

        let rows = render("abc\x1b[2G\x1b[1KZ");
        assert_eq!(rows[0].spans.row_data(0).unwrap().text, " Zc");
    }

    #[test]
    fn insert_delete_and_erase_characters_preserve_unicode_cells() {
        let rows = render("ab界\x1b[1G\x1b[2P");
        let text = rows[0]
            .spans
            .iter()
            .map(|span| span.text.to_string())
            .collect::<String>();
        assert_eq!(text.trim_end(), "界");
        let rows = render("xy\x1b[1G\x1b[2@Z");
        let text = rows[0]
            .spans
            .iter()
            .map(|span| span.text.to_string())
            .collect::<String>();
        assert!(text.starts_with("Z"), "{text}");
        let rows = render("hello\x1b[2G\x1b[3X");
        assert_eq!(rows[0].spans.row_data(0).unwrap().text, "h   o");
    }

    #[test]
    fn vertical_position_and_wide_glyph_insert_delete_share_cells() {
        let rows = render("one\ntwo\x1b[1d\x1b[1GX");
        assert_eq!(rows[0].spans.row_data(0).unwrap().text, "Xne");
        assert_eq!(rows[1].spans.row_data(0).unwrap().text, "two");
        let rows = render("界界\x1b[1G\x1b[1P");
        let text = rows[0]
            .spans
            .iter()
            .map(|span| span.text.to_string())
            .collect::<String>();
        assert_eq!(text.trim_end(), "界");
    }
}
