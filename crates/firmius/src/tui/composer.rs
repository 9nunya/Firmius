//! Input composer: segment-based so pasted blocks stay atomic (Claude Code
//! style). Contract is pinned; internals are a first pass to be hardened.

pub const PASTE_BLOCK_THRESHOLD: usize = 120;

#[derive(Debug, Clone)]
pub enum Segment {
    Text(String),
    /// Reference into the paste store (`Model.pastes`); label is the id.
    Paste(usize),
}

#[derive(Debug, Default)]
pub struct Composer {
    segments: Vec<Segment>,
    /// (segment index, char offset within). `segments.len()` means "at end".
    /// A cursor right after a paste block at index i is canonicalized to
    /// (i + 1, 0).
    cursor: (usize, usize),
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

    /// Insert a paste-block placeholder; the real content lives in the paste
    /// store under `id` (1-based).
    pub fn insert_paste_block(&mut self, id: usize) {
        let (i, _) = self.cursor;
        let at = i.min(self.segments.len());
        self.segments.insert(at, Segment::Paste(id));
        self.cursor = (at + 1, 0);
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
                self.cursor = (i.min(self.segments.len()), 0);
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
                        } else {
                            self.cursor = (i - 1, n - 1);
                        }
                    }
                }
                Segment::Paste(_) => {
                    // Atomic: the whole block goes.
                    self.segments.remove(i - 1);
                    self.cursor = (i - 1, 0);
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
                }
            }
            Some(Segment::Paste(_)) => {
                self.segments.remove(i);
                self.cursor = (i.min(self.segments.len()), 0);
            }
            _ => {}
        }
    }

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
            _ => self.cursor = (i + 1, 0),
        }
    }

    // Line-aware vertical movement and line jumps: to be hardened.
    pub fn up(&mut self) {}
    pub fn down(&mut self) {}
    pub fn home(&mut self) {
        let (i, _) = self.cursor;
        if i < self.segments.len() {
            self.cursor = (i, 0);
        }
    }
    pub fn end(&mut self) {
        let (i, _) = self.cursor;
        if let Some(Segment::Text(t)) = self.segments.get(i) {
            self.cursor = (i, char_count(t));
        }
    }

    pub fn clear(&mut self) {
        self.segments.clear();
        self.cursor = (0, 0);
    }

    /// Display lines: text as-is, paste blocks as their placeholder label.
    pub fn lines(&self, pastes: &[String]) -> Vec<String> {
        if self.segments.is_empty() {
            return vec![String::new()];
        }
        let joined: Vec<String> = self
            .segments
            .iter()
            .map(|s| match s {
                Segment::Text(t) => t.clone(),
                Segment::Paste(id) => {
                    let content = pastes.get(id - 1).map(String::as_str).unwrap_or("");
                    let n = content.lines().count();
                    format!("[Pasted text #{id} +{n} lines]")
                }
            })
            .collect();
        joined.join("\n").split('\n').map(str::to_string).collect()
    }

    /// Submit: expand paste blocks back to their stored content. Returns
    /// `None` when there's nothing but whitespace. Clears the composer.
    pub fn take(&mut self, pastes: &[String]) -> Option<String> {
        if self.is_empty() {
            return None;
        }
        let parts: Vec<String> = self
            .segments
            .iter()
            .map(|s| match s {
                Segment::Text(t) => t.clone(),
                Segment::Paste(id) => pastes.get(id - 1).cloned().unwrap_or_default(),
            })
            .filter(|p| !p.is_empty())
            .collect();
        self.clear();
        Some(parts.join("\n"))
    }

    /// Cursor position in display coordinates, for the terminal cursor.
    pub fn cursor_pos(&self, pastes: &[String]) -> (usize, usize) {
        let (ci, off) = self.cursor;
        let mut chars = 0usize;
        for (idx, seg) in self.segments.iter().enumerate() {
            if idx == ci {
                break;
            }
            chars += match seg {
                Segment::Text(t) => char_count(t),
                Segment::Paste(id) => {
                    let content = pastes.get(id - 1).map(String::as_str).unwrap_or("");
                    let n = content.lines().count();
                    format!("[Pasted text #{id} +{n} lines]").chars().count()
                }
            } + 1; // segment separator newline
        }
        if let Some(Segment::Text(t)) = self.segments.get(ci) {
            chars += off.min(char_count(t));
        }
        let display = self.lines(pastes).join("\n");
        let mut row = 0;
        let mut col = 0;
        for (i, c) in display.chars().enumerate() {
            if i >= chars {
                break;
            }
            if c == '\n' {
                row += 1;
                col = 0;
            } else {
                col += 1;
            }
        }
        (row, col)
    }
}