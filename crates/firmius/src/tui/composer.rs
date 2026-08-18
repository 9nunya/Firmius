//! Input composer: segment-based so pasted blocks stay atomic (Claude Code
//! style).
//!
//! ## Model
//! `segments` is the source of truth; the cursor is a logical position:
//! `(segment index, char offset)`. `segments.len()` means "at the end".
//! Right after a paste block at index i, the cursor is canonicalized to
//! `(i + 1, 0)`.
//!
//! ## Display
//! `lines()` renders each segment — `Text` verbatim, `Paste` as its
//! `[Pasted text #N +L lines]` placeholder — joined by newlines. While
//! rendering, the row layout is cached (`rows_cache`) so the paste-free
//! movement methods (`up`/`down`/`home`/`end`) can be line-aware without
//! access to the paste store. The cache is refreshed by every `lines()` /
//! `cursor_pos()` call, i.e. every frame, before the next key is handled.
//!
//! ## Invariants
//! - Cursor offsets always sit on char boundaries (multibyte-safe).
//! - Placeholders are atomic: movement hops over them, deletion removes
//!   them whole, the cursor never lands inside one.
//! - `cursor_pos()` agrees with `lines()` by construction — both derive
//!   from the same row layout.

use std::cell::RefCell;

use firmius_core::{ImagePart, Message, MessagePart, MessageRole};
use unicode_width::UnicodeWidthChar;

pub const PASTE_BLOCK_THRESHOLD: usize = 120;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PastedImage {
    pub media_type: String,
    pub data_base64: String,
    pub width: usize,
    pub height: usize,
    pub bytes: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum StoredPaste {
    Text(String),
    Image(PastedImage),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ComposerSubmission {
    Text(String),
    Message(Message),
}

#[derive(Debug, Clone)]
pub enum Segment {
    Text(String),
    /// Reference into the paste store (`Model.pastes`); label is the id.
    Paste(usize),
}

/// One display row: which segment it belongs to, the char offset within
/// that segment's text where the row starts, and the row's text.
#[derive(Debug, Clone)]
struct Row {
    seg: usize,
    off: usize,
    text: String,
    paste: bool,
}

#[derive(Debug, Default)]
pub struct Composer {
    segments: Vec<Segment>,
    /// (segment index, char offset within); `segments.len()` = at end.
    cursor: (usize, usize),
    /// Row layout from the last `lines()`/`cursor_pos()` render.
    rows_cache: RefCell<Vec<Row>>,
}

fn byte_at(s: &str, char_off: usize) -> usize {
    s.char_indices()
        .nth(char_off)
        .map(|(b, _)| b)
        .unwrap_or(s.len())
}

fn char_count(s: &str) -> usize {
    s.chars().count()
}

impl Composer {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn is_empty(&self) -> bool {
        self.segments
            .iter()
            .all(|s| matches!(s, Segment::Text(t) if t.trim().is_empty()))
    }

    // ------------------------------------------------------------------
    // Editing
    // ------------------------------------------------------------------

    pub fn insert_char(&mut self, c: char) {
        let (i, off) = self.cursor;
        match self.segments.get_mut(i) {
            Some(Segment::Text(t)) => {
                let at = byte_at(t, off);
                t.insert(at, c);
                self.cursor = (i, off + 1);
            }
            Some(Segment::Paste(_)) => {
                // Cursor sits before a paste block: start a text segment here.
                self.segments.insert(i, Segment::Text(c.to_string()));
                self.cursor = (i, 1);
            }
            None => {
                self.segments.push(Segment::Text(c.to_string()));
                self.cursor = (self.segments.len() - 1, 1);
            }
        }
    }

    pub fn insert_str(&mut self, s: &str) {
        for c in s.chars() {
            self.insert_char(c);
        }
    }

    pub fn newline(&mut self) {
        self.insert_char('\n');
    }

    /// Insert a paste-block placeholder at the cursor; the real content
    /// lives in the paste store under `id` (1-based). Splits a text
    /// segment if the cursor sits inside one.
    pub fn insert_paste_block(&mut self, id: usize) {
        let (i, off) = self.cursor;
        let splits = matches!(self.segments.get(i), Some(Segment::Text(t)) if off > 0 && off < char_count(t));
        if splits {
            if let Some(Segment::Text(t)) = self.segments.get_mut(i) {
                let tail = t.split_off(byte_at(t, off));
                self.segments.insert(i + 1, Segment::Paste(id));
                self.segments.insert(i + 2, Segment::Text(tail));
                self.cursor = (i + 2, 0);
            }
        } else {
            // A cursor at the END of a text segment inserts after it.
            let at_end_of_text =
                matches!(self.segments.get(i), Some(Segment::Text(t)) if off == char_count(t));
            let at = if at_end_of_text {
                i + 1
            } else {
                i.min(self.segments.len())
            };
            self.segments.insert(at, Segment::Paste(id));
            self.cursor = (at + 1, 0);
        }
    }

    pub fn backspace(&mut self) {
        let (i, off) = self.cursor;
        if let Some(Segment::Text(t)) = self.segments.get_mut(i)
            && off > 0
        {
            let start = byte_at(t, off - 1);
            let end = byte_at(t, off);
            t.replace_range(start..end, "");
            if t.is_empty() {
                self.segments.remove(i);
                let at = i.min(self.segments.len());
                self.cursor = (at, 0);
                self.merge_around(at);
            } else {
                self.cursor = (i, off - 1);
            }
            return;
        }
        if i > 0 {
            match &self.segments[i - 1] {
                Segment::Text(_) => {
                    if let Some(Segment::Text(t)) = self.segments.get_mut(i - 1) {
                        let n = char_count(t);
                        let start = byte_at(t, n - 1);
                        t.truncate(start);
                        if t.is_empty() {
                            self.segments.remove(i - 1);
                            self.cursor = (i - 1, 0);
                            self.merge_around(i - 1);
                        } else {
                            self.cursor = (i - 1, n - 1);
                        }
                    }
                }
                Segment::Paste(_) => {
                    // Atomic: the whole block goes.
                    self.segments.remove(i - 1);
                    self.cursor = (i - 1, 0);
                    self.merge_around(i - 1);
                }
            }
        }
    }

    pub fn delete(&mut self) {
        let (i, off) = self.cursor;
        match self.segments.get_mut(i) {
            Some(Segment::Text(t)) if off < char_count(t) => {
                let start = byte_at(t, off);
                let end = byte_at(t, off + 1);
                t.replace_range(start..end, "");
                if t.is_empty() {
                    self.segments.remove(i);
                    self.merge_around(i);
                }
            }
            Some(Segment::Paste(_)) => {
                self.segments.remove(i);
                self.cursor = (i.min(self.segments.len()), 0);
                self.merge_around(i.min(self.segments.len()));
            }
            _ => {
                // Cursor at the very end of a text segment: deleting the
                // boundary merges with the next segment (or eats a paste).
                let (i, off) = self.cursor;
                let at_end =
                    matches!(self.segments.get(i), Some(Segment::Text(t)) if off == char_count(t));
                if at_end && i + 1 < self.segments.len() {
                    if matches!(self.segments[i + 1], Segment::Paste(_)) {
                        self.segments.remove(i + 1);
                    }
                    self.merge_around(i + 1);
                }
            }
        }
    }

    /// After a segment removal at/around `idx`, join Text neighbors so a
    /// removed boundary (separator or paste block) doesn't strand two text
    /// halves. Leaves the cursor at the join point.
    fn merge_around(&mut self, idx: usize) {
        if idx == 0 || idx >= self.segments.len() {
            return;
        }
        let both_text = matches!(
            (self.segments.get(idx - 1), self.segments.get(idx)),
            (Some(Segment::Text(_)), Some(Segment::Text(_)))
        );
        if !both_text {
            return;
        }
        if let Some(Segment::Text(b)) = self.segments.get(idx) {
            let b = b.clone();
            let join;
            if let Some(Segment::Text(a)) = self.segments.get_mut(idx - 1) {
                join = char_count(a);
                a.push_str(&b);
            } else {
                return;
            }
            self.segments.remove(idx);
            self.cursor = (idx - 1, join);
        }
    }

    // ------------------------------------------------------------------
    // Movement
    // ------------------------------------------------------------------

    pub fn left(&mut self) {
        let (i, off) = self.cursor;
        if i >= self.segments.len() {
            if let Some(seg) = self.segments.last() {
                let n = self.segments.len() - 1;
                self.cursor = match seg {
                    Segment::Text(t) => (n, char_count(t)),
                    Segment::Paste(_) => (n, 0),
                };
            }
            return;
        }
        match &self.segments[i] {
            Segment::Text(t) if off > 0 => self.cursor = (i, off - 1),
            _ => {
                if i > 0 {
                    self.cursor = match &self.segments[i - 1] {
                        // Hop the whole paste block; or land at text end.
                        Segment::Text(t) => (i - 1, char_count(t)),
                        Segment::Paste(_) => (i - 1, 0),
                    };
                }
            }
        }
    }

    pub fn right(&mut self) {
        let (i, off) = self.cursor;
        if i >= self.segments.len() {
            return;
        }
        match &self.segments[i] {
            Segment::Text(t) if off < char_count(t) => self.cursor = (i, off + 1),
            // Hop the whole paste block; or step into the next segment.
            _ => self.cursor = (i + 1, 0),
        }
    }

    /// Move to the previous word boundary in the current text segment.
    /// Paste placeholders are atomic, so crossing one takes the same single
    /// segment step as ordinary left movement.
    pub fn word_left(&mut self) {
        let (i, off) = self.cursor;
        if i >= self.segments.len() {
            if let Some(Segment::Text(text)) = self.segments.last() {
                self.cursor = (i - 1, char_count(text));
            } else if i > 0 {
                self.cursor = (i - 1, 0);
            }
            return;
        }
        let Some(Segment::Text(text)) = self.segments.get(i) else {
            if i > 0 {
                self.cursor = (i - 1, 0);
            }
            return;
        };
        if off == 0 {
            if i > 0 {
                self.cursor = match &self.segments[i - 1] {
                    Segment::Text(previous) => (i - 1, char_count(previous)),
                    Segment::Paste(_) => (i - 1, 0),
                };
            }
            return;
        }

        let chars: Vec<char> = text.chars().collect();
        let mut boundary = off;
        // First cross whitespace immediately before the cursor, then the
        // non-whitespace run. This also handles a cursor in the middle of a
        // word by landing at that word's start.
        if chars[boundary - 1].is_whitespace() {
            while boundary > 0 && chars[boundary - 1].is_whitespace() {
                boundary -= 1;
            }
        }
        while boundary > 0 && !chars[boundary - 1].is_whitespace() {
            boundary -= 1;
        }
        self.cursor = (i, boundary);
    }

    /// Move to the next word boundary in the current text segment.
    pub fn word_right(&mut self) {
        let (i, off) = self.cursor;
        let Some(Segment::Text(text)) = self.segments.get(i) else {
            if i < self.segments.len() {
                self.cursor = (i + 1, 0);
            }
            return;
        };
        let chars: Vec<char> = text.chars().collect();
        if off >= chars.len() {
            if i < self.segments.len() {
                self.cursor = (i + 1, 0);
            }
            return;
        }
        let mut boundary = off;
        if chars[boundary].is_whitespace() {
            while boundary < chars.len() && chars[boundary].is_whitespace() {
                boundary += 1;
            }
        } else {
            while boundary < chars.len() && !chars[boundary].is_whitespace() {
                boundary += 1;
            }
        }
        self.cursor = (i, boundary);
    }

    /// Delete back to the previous word boundary. As with plain backspace,
    /// a paste immediately to the left is removed as one atomic block.
    pub fn backspace_word(&mut self) {
        let (i, off) = self.cursor;
        if off == 0 {
            if i > 0 && matches!(self.segments[i - 1], Segment::Paste(_)) {
                self.segments.remove(i - 1);
                self.cursor = (i - 1, 0);
                self.merge_around(i - 1);
            }
            return;
        }
        let Some(Segment::Text(text)) = self.segments.get(i) else {
            return;
        };
        let chars: Vec<char> = text.chars().collect();
        let mut boundary = off.min(chars.len());
        while boundary > 0 && chars[boundary - 1].is_whitespace() {
            boundary -= 1;
        }
        while boundary > 0 && !chars[boundary - 1].is_whitespace() {
            boundary -= 1;
        }
        if boundary == off {
            return;
        }
        let empty = if let Some(Segment::Text(text)) = self.segments.get_mut(i) {
            let start = byte_at(text, boundary);
            let end = byte_at(text, off);
            text.replace_range(start..end, "");
            text.is_empty()
        } else {
            return;
        };
        self.cursor = (i, boundary);
        if empty {
            self.segments.remove(i);
            self.cursor = (i.min(self.segments.len()), 0);
            self.merge_around(i.min(self.segments.len()));
        }
    }

    pub fn up(&mut self) {
        self.vertical(-1);
    }

    pub fn down(&mut self) {
        self.vertical(1);
    }

    fn vertical(&mut self, dir: i32) {
        let rows = self.rows_cache.borrow().clone();
        if rows.is_empty() {
            return;
        }
        let (ri, col) = self.locate_cursor(&rows);
        let target = ri as i32 + dir;
        if target < 0 || target >= rows.len() as i32 {
            return;
        }
        self.move_to_row(&rows[target as usize], col);
    }

    pub fn home(&mut self) {
        let rows = self.rows_cache.borrow().clone();
        if rows.is_empty() {
            return;
        }
        let (ri, _) = self.locate_cursor(&rows);
        self.move_to_row(&rows[ri], 0);
    }

    pub fn end(&mut self) {
        let rows = self.rows_cache.borrow().clone();
        if rows.is_empty() {
            return;
        }
        let (ri, _) = self.locate_cursor(&rows);
        let row = &rows[ri];
        if row.paste {
            self.cursor = (row.seg + 1, 0);
        } else {
            self.cursor = (row.seg, row.off + char_count(&row.text));
        }
    }

    fn move_to_row(&mut self, row: &Row, col: usize) {
        if row.paste {
            // Landing on a paste row: sit before it; the block is atomic.
            self.cursor = (row.seg, 0);
            return;
        }
        let off = col.min(char_count(&row.text));
        self.cursor = (row.seg, row.off + off);
    }

    pub fn clear(&mut self) {
        self.segments.clear();
        self.cursor = (0, 0);
    }

    /// Expand the current composer contents without clearing it. Paste blocks
    /// are replaced by their stored text, matching what submission sends.
    pub fn text(&self, pastes: &[StoredPaste]) -> String {
        self.segments
            .iter()
            .map(|segment| match segment {
                Segment::Text(text) => text.clone(),
                Segment::Paste(id) => Self::display_text(*id, pastes),
            })
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// Replace the entire composer contents, used when accepting a command
    /// completion. The replacement is plain text by design.
    pub fn replace_text(&mut self, text: &str) {
        self.clear();
        self.insert_str(text);
    }

    // ------------------------------------------------------------------
    // Display & submission
    // ------------------------------------------------------------------

    /// Display lines: text as-is, paste blocks as their placeholder label.
    /// Refreshes the row cache used by the line-aware movement methods.
    pub fn lines(&self, pastes: &[StoredPaste]) -> Vec<String> {
        self.lines_with_width(pastes, usize::MAX)
    }

    /// Display lines wrapped to the available terminal width.
    pub fn lines_with_width(&self, pastes: &[StoredPaste], width: usize) -> Vec<String> {
        let rows = self.build_rows(pastes, width);
        *self.rows_cache.borrow_mut() = rows.clone();
        rows.into_iter().map(|r| r.text).collect()
    }

    pub fn submission(&self, pastes: &[StoredPaste]) -> Option<ComposerSubmission> {
        if self.is_empty() {
            return None;
        }

        let mut plain_parts = Vec::new();
        let mut message_parts = Vec::new();
        let mut saw_image = false;
        for segment in &self.segments {
            match segment {
                Segment::Text(text) if !text.is_empty() => {
                    plain_parts.push(text.clone());
                    message_parts.push(MessagePart::Text(text.clone()));
                }
                Segment::Text(_) => {}
                Segment::Paste(id) => match pastes.get(id.saturating_sub(1)) {
                    Some(StoredPaste::Text(text)) if !text.is_empty() => {
                        plain_parts.push(text.clone());
                        message_parts.push(MessagePart::Text(text.clone()));
                    }
                    Some(StoredPaste::Image(image)) => {
                        saw_image = true;
                        message_parts.push(MessagePart::Image(ImagePart::from_base64(
                            image.media_type.clone(),
                            image.data_base64.clone(),
                        )));
                    }
                    _ => {}
                },
            }
        }

        if !saw_image {
            let text = plain_parts.join("\n");
            if text.trim().is_empty() {
                return None;
            }
            return Some(ComposerSubmission::Text(text));
        }

        if message_parts.is_empty() {
            return None;
        }
        Some(ComposerSubmission::Message(Message::with_parts(
            MessageRole::User,
            message_parts,
        )))
    }

    /// Submit: expand paste blocks back to their stored content. Returns
    /// `None` when there's nothing but whitespace. Clears the composer.
    pub fn take_submission(&mut self, pastes: &[StoredPaste]) -> Option<ComposerSubmission> {
        let submission = self.submission(pastes)?;
        self.clear();
        Some(submission)
    }

    /// Cursor position in display coordinates, for the terminal cursor.
    /// Agrees with `lines()` — both derive from the same row layout.
    pub fn cursor_pos(&self, pastes: &[StoredPaste]) -> (usize, usize) {
        self.cursor_pos_with_width(pastes, usize::MAX)
    }

    pub fn cursor_pos_with_width(&self, pastes: &[StoredPaste], width: usize) -> (usize, usize) {
        let rows = self.build_rows(pastes, width);
        *self.rows_cache.borrow_mut() = rows.clone();
        self.locate_cursor(&rows)
    }

    fn display_text(id: usize, pastes: &[StoredPaste]) -> String {
        match pastes.get(id.saturating_sub(1)) {
            Some(StoredPaste::Text(text)) => text.clone(),
            Some(StoredPaste::Image(image)) => format!(
                "[Pasted image #{id} {}x{} {}]",
                image.width, image.height, image.media_type
            ),
            None => String::new(),
        }
    }

    fn placeholder(id: usize, pastes: &[StoredPaste]) -> String {
        match pastes.get(id.saturating_sub(1)) {
            Some(StoredPaste::Text(content)) => {
                let n = content.lines().count();
                format!("[Pasted text #{id} +{n} lines]")
            }
            Some(StoredPaste::Image(image)) => format!(
                "[Pasted image #{id} {}x{} {} · {} bytes]",
                image.width, image.height, image.media_type, image.bytes
            ),
            None => format!("[Missing paste #{id}]"),
        }
    }

    fn build_rows(&self, pastes: &[StoredPaste], width: usize) -> Vec<Row> {
        if self.segments.is_empty() {
            return vec![Row {
                seg: 0,
                off: 0,
                text: String::new(),
                paste: false,
            }];
        }
        let mut rows = Vec::new();
        for (i, seg) in self.segments.iter().enumerate() {
            match seg {
                Segment::Text(t) => {
                    let mut off = 0;
                    for line in t.split('\n') {
                        let chunks = wrap_line(line, width);
                        for chunk in chunks {
                            rows.push(Row {
                                seg: i,
                                off,
                                text: chunk.clone(),
                                paste: false,
                            });
                            off += char_count(&chunk);
                        }
                        off += 1;
                    }
                }
                Segment::Paste(id) => {
                    for chunk in wrap_line(&Self::placeholder(*id, pastes), width) {
                        rows.push(Row {
                            seg: i,
                            off: 0,
                            text: chunk,
                            paste: true,
                        });
                    }
                }
            }
        }
        rows
    }

    /// Map the logical cursor onto (row, col) of the given row layout.
    fn locate_cursor(&self, rows: &[Row]) -> (usize, usize) {
        let (ci, off) = self.cursor;
        for (ri, row) in rows.iter().enumerate() {
            if row.seg > ci {
                break;
            }
            if row.seg == ci {
                if row.paste {
                    return (ri, 0);
                }
                if off <= row.off + char_count(&row.text) {
                    return (ri, off.saturating_sub(row.off));
                }
            }
        }
        // Cursor at the very end, or between rows of one segment: clamp.
        rows.last()
            .map(|r| (rows.len() - 1, char_count(&r.text)))
            .unwrap_or((0, 0))
    }
}

fn wrap_line(line: &str, width: usize) -> Vec<String> {
    let width = width.max(1);
    if line.is_empty() {
        return vec![String::new()];
    }
    let mut rows = Vec::new();
    let mut current = String::new();
    let mut used = 0;
    for ch in line.chars() {
        let ch_width = ch.width().unwrap_or(1);
        if used > 0 && used + ch_width > width {
            rows.push(std::mem::take(&mut current));
            used = 0;
        }
        current.push(ch);
        used += ch_width;
    }
    rows.push(current);
    rows
}

#[cfg(test)]
impl Composer {
    /// Test helper: refresh the layout cache then move to the start.
    fn home_refresh(&mut self, pastes: &[StoredPaste]) {
        self.lines(pastes);
        self.cursor = (0, 0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn store() -> Vec<StoredPaste> {
        vec![
            StoredPaste::Text("line one\nline two\nline three".to_string()),
            StoredPaste::Text("short paste".to_string()),
            StoredPaste::Image(PastedImage {
                media_type: "image/png".to_string(),
                data_base64: "Zm9v".to_string(),
                width: 2,
                height: 3,
                bytes: 16,
            }),
        ]
    }

    #[test]
    fn typing_and_backspace_with_multibyte_chars() {
        let mut c = Composer::new();
        c.insert_str("héllo 🚀");
        c.backspace();
        let out = c.take_submission(&[]).unwrap();
        assert_eq!(out, ComposerSubmission::Text("héllo ".into()));
    }

    #[test]
    fn paste_block_renders_placeholder_line() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_str("before");
        c.insert_paste_block(1);
        c.insert_str("after");
        let lines = c.lines(&pastes);
        assert_eq!(lines.len(), 3);
        assert_eq!(lines[0], "before");
        assert!(lines[1].starts_with("[Pasted text #1 +3 lines]"));
        assert_eq!(lines[2], "after");
    }

    #[test]
    fn backspace_after_block_removes_whole_block() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_paste_block(1);
        c.backspace();
        assert!(c.lines(&pastes)[0].is_empty());
        assert!(c.is_empty());
    }

    #[test]
    fn delete_before_block_removes_whole_block() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_paste_block(1);
        c.home_refresh(&pastes);
        c.delete();
        assert!(c.is_empty());
    }

    #[test]
    fn left_right_hop_over_block() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_str("ab");
        c.insert_paste_block(1);
        c.insert_str("cd");
        // Cursor after "cd". Walk left: 'd', 'c', hop the whole block,
        // land at the end of "ab", then one step into it.
        for _ in 0..5 {
            c.left();
        }
        let (row, col) = c.cursor_pos(&pastes);
        assert_eq!((row, col), (0, 1));
        // Walk right: to end of "ab", before the block, then hop it.
        for _ in 0..3 {
            c.right();
        }
        let (row, col) = c.cursor_pos(&pastes);
        assert_eq!((row, col), (2, 0));
    }

    #[test]
    fn word_navigation_stops_at_whitespace_boundaries() {
        let mut c = Composer::new();
        c.insert_str("foo bar  baz");
        let pastes = store();
        c.home_refresh(&pastes);
        c.word_right();
        assert_eq!(c.cursor_pos(&pastes), (0, 3));
        c.word_right();
        assert_eq!(c.cursor_pos(&pastes), (0, 4));
        c.word_right();
        assert_eq!(c.cursor_pos(&pastes), (0, 7));
        c.word_right();
        assert_eq!(c.cursor_pos(&pastes), (0, 9));
        c.word_right();
        assert_eq!(c.cursor_pos(&pastes), (0, 12));

        c.end();
        c.word_left();
        assert_eq!(c.cursor_pos(&pastes), (0, 9));
        c.word_left();
        assert_eq!(c.cursor_pos(&pastes), (0, 4));
        c.word_left();
        assert_eq!(c.cursor_pos(&pastes), (0, 0));
    }

    #[test]
    fn backspace_word_deletes_current_word_prefix_and_paste_atomically() {
        let mut c = Composer::new();
        c.insert_str("foo bar");
        c.left(); // middle of "bar"
        c.backspace_word();
        assert_eq!(c.lines(&[]), ["foo r"]);

        let mut c = Composer::new();
        let pastes = store();
        c.insert_paste_block(1);
        c.backspace_word();
        assert!(c.is_empty());
        assert_eq!(c.lines(&pastes), [""]);
    }

    #[test]
    fn take_expands_pastes_and_clears() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_str("pre\n");
        c.insert_paste_block(1);
        c.insert_str("\npost");
        let out = c.take_submission(&pastes).unwrap();
        let ComposerSubmission::Text(out) = out else {
            panic!("expected text submission")
        };
        assert!(out.starts_with("pre\n"));
        assert!(out.contains("line one\nline two\nline three"));
        assert!(out.ends_with("post"));
        assert!(c.is_empty());
        assert!(c.take_submission(&pastes).is_none());
    }

    #[test]
    fn whitespace_only_is_none() {
        let mut c = Composer::new();
        c.insert_str("   \n  ");
        assert!(c.take_submission(&[]).is_none());
    }

    #[test]
    fn vertical_movement_and_home_end_across_lines() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_str("first line\nsecond line");
        c.lines(&pastes); // refresh layout cache
        c.home();
        let (row, col) = c.cursor_pos(&pastes);
        assert_eq!((row, col), (1, 0));
        c.up();
        let (row, col) = c.cursor_pos(&pastes);
        assert_eq!((row, col), (0, 0));
        c.end();
        let (row, col) = c.cursor_pos(&pastes);
        assert_eq!((row, col), (0, "first line".chars().count()));
        c.down();
        let (row, col) = c.cursor_pos(&pastes);
        assert_eq!(
            (row, col),
            (
                1,
                "second line"
                    .chars()
                    .count()
                    .min("first line".chars().count())
            )
        );
    }

    #[test]
    fn cursor_pos_agrees_with_lines_after_mixed_ops() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_str("hello world");
        c.left(); // cursor between 'l' and 'd' of "world"
        let lines = c.lines(&pastes);
        let (row, col) = c.cursor_pos(&pastes);
        let line_chars: Vec<char> = lines[row].chars().collect();
        assert!(col <= line_chars.len());
        assert_eq!(line_chars[col], 'd');
    }

    #[test]
    fn no_panic_edge_cases() {
        let mut c = Composer::new();
        c.backspace();
        c.delete();
        c.left();
        c.right();
        c.up();
        c.down();
        c.home();
        c.end();
        assert!(c.is_empty());
        assert_eq!(c.cursor_pos(&[]), (0, 0));
        assert_eq!(c.lines(&[]), vec!["".to_string()]);
    }

    #[test]
    fn backspacing_separator_merges_text_segments() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_str("head");
        c.insert_paste_block(1);
        c.insert_str("tail");
        // Cursor is at end of "tail": left through 'l','i','a','t' (4),
        // hop the whole block (1), land at end of "head" (1).
        for _ in 0..6 {
            c.left();
        }
        c.delete(); // eats the paste, merges texts
        let lines = c.lines(&pastes);
        assert_eq!(lines, vec!["headtail".to_string()]);
    }

    #[test]
    fn insert_paste_block_splits_text_segment() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_str("abcdef");
        c.left();
        c.left();
        c.left(); // cursor between "abc" and "def"
        c.insert_paste_block(2);
        let lines = c.lines(&pastes);
        assert_eq!(lines[0], "abc");
        assert!(lines[1].starts_with("[Pasted text #2"));
        assert_eq!(lines[2], "def");
        let out = c.take_submission(&pastes).unwrap();
        assert_eq!(
            out,
            ComposerSubmission::Text("abc\nshort paste\ndef".into())
        );
    }

    #[test]
    fn long_line_wraps_and_cursor_uses_wrapped_rows() {
        let mut c = Composer::new();
        c.insert_str("abcdefgh");
        assert_eq!(c.lines_with_width(&[], 3), ["abc", "def", "gh"]);
        assert_eq!(c.cursor_pos_with_width(&[], 3), (2, 2));
    }

    #[test]
    fn image_paste_renders_image_placeholder_and_submits_message_parts() {
        let mut c = Composer::new();
        let pastes = store();
        c.insert_str("describe this");
        c.insert_paste_block(3);
        let lines = c.lines(&pastes);
        assert!(lines[1].contains("Pasted image #3"));
        let submission = c.take_submission(&pastes).unwrap();
        let ComposerSubmission::Message(message) = submission else {
            panic!("expected message submission")
        };
        assert_eq!(message.role, MessageRole::User);
        assert!(matches!(message.content[0], MessagePart::Text(ref t) if t == "describe this"));
        assert!(matches!(message.content[1], MessagePart::Image(_)));
    }
}
