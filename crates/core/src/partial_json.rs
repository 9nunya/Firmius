use std::collections::HashMap;

use serde_json::Value;

/// Represents the parsing state of a single top-level field in a streaming JSON object.
#[derive(Debug, Clone, PartialEq)]
pub enum Field {
    /// The key has not yet been encountered in the input.
    Missing,
    /// A string value whose closing quote has not arrived yet.
    /// The contained string is unescaped and always a prefix of the final value.
    Partial(String),
    /// A fully parsed value (string, number, bool, null, or nested object/array).
    Complete(Value),
}

/// An incremental, best-effort parser for streaming JSON tool-call arguments.
///
/// Designed for rendering partial progress in a TUI.  It does **not** aim to be a
/// general incremental JSON parser — it extracts top-level key/value pairs and
/// returns [`Field::Missing`] for nested structures that are still streaming.
pub struct PartialJson {
    fields: HashMap<String, Field>,
}

impl PartialJson {
    /// Parse `input`, which may be a prefix of a JSON object.
    ///
    /// If the input is fully valid JSON the fast path via [`serde_json::from_str`] is
    /// used and every top-level key will be [`Field::Complete`].  Otherwise a
    /// hand-rolled tokenizer extracts whatever it can.
    pub fn parse(input: &str) -> Self {
        // Fast path: complete, valid JSON.
        if let Ok(value) = serde_json::from_str::<Value>(input) {
            if let Value::Object(map) = value {
                let fields = map
                    .into_iter()
                    .map(|(k, v)| (k, Field::Complete(v)))
                    .collect();
                return PartialJson { fields };
            }
            // Non-object JSON (array, string, …) — nothing to expose.
            return PartialJson {
                fields: HashMap::new(),
            };
        }

        // Slow path: hand-rolled tokenizer.
        let chars: Vec<char> = input.chars().collect();
        let len = chars.len();
        let mut pos: usize = 0;
        let mut fields: HashMap<String, Field> = HashMap::new();

        // Skip optional leading whitespace / BOM and the opening brace.
        while pos < len && chars[pos].is_whitespace() {
            pos += 1;
        }
        if pos < len && chars[pos] == '{' {
            pos += 1;
        }

        loop {
            // Whitespace between pairs.
            while pos < len && chars[pos].is_whitespace() {
                pos += 1;
            }
            if pos >= len || chars[pos] == '}' {
                break;
            }

            // ── key ──────────────────────────────────────────────
            if chars[pos] != '"' {
                // Garbled input; skip one char and try to recover.
                pos += 1;
                continue;
            }
            pos += 1; // opening quote

            let mut key = String::new();
            let mut key_complete = false;
            while pos < len {
                if chars[pos] == '\\' {
                    pos += 1;
                    if pos < len {
                        match chars[pos] {
                            'n' => key.push('\n'),
                            'r' => key.push('\r'),
                            't' => key.push('\t'),
                            '\\' => key.push('\\'),
                            '"' => key.push('"'),
                            other => {
                                key.push('\\');
                                key.push(other);
                            }
                        }
                        pos += 1;
                    }
                } else if chars[pos] == '"' {
                    pos += 1; // closing quote
                    key_complete = true;
                    break;
                } else {
                    key.push(chars[pos]);
                    pos += 1;
                }
            }

            if !key_complete {
                // Key name itself is still streaming; stop here.
                break;
            }

            // ── colon ────────────────────────────────────────────
            while pos < len && chars[pos].is_whitespace() {
                pos += 1;
            }
            if pos >= len || chars[pos] != ':' {
                // Colon hasn't arrived yet.
                break;
            }
            pos += 1; // skip ':'
            while pos < len && chars[pos].is_whitespace() {
                pos += 1;
            }
            if pos >= len {
                break; // value not started
            }

            // ── value ────────────────────────────────────────────
            let field = parse_value(&chars, &mut pos);
            fields.insert(key, field);

            // ── comma / end ──────────────────────────────────────
            while pos < len && chars[pos].is_whitespace() {
                pos += 1;
            }
            if pos < len && chars[pos] == ',' {
                pos += 1;
            }
        }

        PartialJson { fields }
    }

    /// Get a reference to the [`Field`] for `key`, or [`Field::Missing`] if the key
    /// has not been seen.
    pub fn get(&self, key: &str) -> &Field {
        static MISSING: Field = Field::Missing;
        self.fields.get(key).unwrap_or(&MISSING)
    }

    /// Return the string content for both [`Field::Complete`] (when the value is a
    /// JSON string) and [`Field::Partial`] fields.
    pub fn str(&self, key: &str) -> Option<&str> {
        match self.get(key) {
            Field::Complete(Value::String(s)) => Some(s.as_str()),
            Field::Partial(s) => Some(s.as_str()),
            _ => None,
        }
    }

    /// Return the string content only for [`Field::Complete`] fields whose value is a
    /// JSON string.
    pub fn complete_str(&self, key: &str) -> Option<&str> {
        match self.get(key) {
            Field::Complete(Value::String(s)) => Some(s.as_str()),
            _ => None,
        }
    }

    /// Returns `true` when the field is fully parsed ([`Field::Complete`]).
    pub fn is_key_complete(&self, key: &str) -> bool {
        matches!(self.get(key), Field::Complete(_))
    }

    /// Iterate over all key / [`Field`] pairs that have been observed so far.
    pub fn iter(&self) -> impl Iterator<Item = (&String, &Field)> {
        self.fields.iter()
    }
}

// ── value parser helpers ───────────────────────────────────────────────────────

/// Parse a single JSON value starting at `pos`.  Advances `pos` past the value
/// (or as far as possible).
fn parse_value(chars: &[char], pos: &mut usize) -> Field {
    let len = chars.len();
    if *pos >= len {
        return Field::Missing;
    }

    match chars[*pos] {
        '"' => parse_string_value(chars, pos),
        '{' | '[' => parse_nested_value(chars, pos),
        '0'..='9' | '-' => parse_number_value(chars, pos),
        't' | 'f' | 'n' => parse_literal_value(chars, pos),
        _ => {
            // Unrecognised – skip and hope for recovery.
            *pos += 1;
            Field::Missing
        }
    }
}

/// `"…"` — may be complete or still streaming.
fn parse_string_value(chars: &[char], pos: &mut usize) -> Field {
    let len = chars.len();
    *pos += 1; // opening quote
    let mut s = String::new();
    let mut complete = false;

    while *pos < len {
        if chars[*pos] == '\\' {
            *pos += 1;
            if *pos < len {
                match chars[*pos] {
                    'n' => s.push('\n'),
                    'r' => s.push('\r'),
                    't' => s.push('\t'),
                    '\\' => s.push('\\'),
                    '"' => s.push('"'),
                    other => {
                        s.push('\\');
                        s.push(other);
                    }
                }
                *pos += 1;
            }
        } else if chars[*pos] == '"' {
            *pos += 1; // closing quote
            complete = true;
            break;
        } else {
            s.push(chars[*pos]);
            *pos += 1;
        }
    }

    if complete {
        Field::Complete(Value::String(s))
    } else {
        Field::Partial(s)
    }
}

/// `{…}` or `[…]` — Complete only when the brackets are balanced.
fn parse_nested_value(chars: &[char], pos: &mut usize) -> Field {
    let len = chars.len();
    let start = *pos;
    let mut expected_closers = Vec::new();
    let mut in_string = false;
    let mut complete = false;

    while *pos < len {
        let c = chars[*pos];
        if in_string {
            if c == '\\' {
                // Skip the escaped character as well, so an escaped quote does
                // not end the string (and an escaped bracket does not affect
                // the nesting stack).
                *pos += 1;
                if *pos < len {
                    *pos += 1;
                }
                continue;
            } else if c == '"' {
                in_string = false;
            }
        } else {
            match c {
                '"' => in_string = true,
                '{' => expected_closers.push('}'),
                '[' => expected_closers.push(']'),
                '}' | ']' => {
                    if expected_closers.pop() != Some(c) {
                        // A mismatched closer cannot complete this value.  Do
                        // not attempt to interpret anything inside it as a
                        // top-level field during recovery.
                        *pos = len;
                        return Field::Missing;
                    }
                    if expected_closers.is_empty() {
                        *pos += 1;
                        complete = true;
                        break;
                    }
                }
                _ => {}
            }
        }
        *pos += 1;
    }

    if complete {
        let slice: String = chars[start..*pos].iter().collect();
        if let Ok(value) = serde_json::from_str::<Value>(&slice) {
            return Field::Complete(value);
        }
    }
    Field::Missing
}

/// Number literal (may be incomplete, e.g. `-` or `12.`).
fn parse_number_value(chars: &[char], pos: &mut usize) -> Field {
    let len = chars.len();
    let start = *pos;
    while *pos < len && !is_value_terminator(chars[*pos]) {
        *pos += 1;
    }
    // A valid prefix such as `4` may grow into `42`, `4.2`, or `4e2`.  Only a
    // delimiter proves that the number has ended; a number at the end of the
    // input is still streaming.  A complete object is handled by the fast
    // path, so this does not affect complete JSON values.
    if *pos == len {
        return Field::Missing;
    }
    let slice: String = chars[start..*pos].iter().collect();
    serde_json::from_str::<Value>(&slice)
        .map(Field::Complete)
        .unwrap_or(Field::Missing)
}

/// `true`, `false`, `null`.
fn parse_literal_value(chars: &[char], pos: &mut usize) -> Field {
    let len = chars.len();
    let start = *pos;
    while *pos < len && chars[*pos].is_ascii_alphabetic() {
        *pos += 1;
    }
    let slice: String = chars[start..*pos].iter().collect();
    match slice.as_str() {
        "true" => Field::Complete(Value::Bool(true)),
        "false" => Field::Complete(Value::Bool(false)),
        "null" => Field::Complete(Value::Null),
        _ => Field::Missing, // streaming literal like "fal"
    }
}

fn is_value_terminator(c: char) -> bool {
    matches!(c, ',' | '}' | ']' | '\x00'..='\x20')
}

// ── tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // ── helpers ────────────────────────────────────────────────────────────────

    /// Feed every prefix-length substring of `full` to the parser and verify:
    /// - No panics.
    /// - Every [`Field::Complete`] value matches the final parse's value for that key.
    /// - Every [`Field::Partial`] text is a prefix of the final string value for that key.
    fn assert_prefixes_sound(full: &str) {
        let final_parse = PartialJson::parse(full);
        // Prefixes must end on UTF-8 character boundaries.  Streaming input is
        // a Rust `&str`, so byte offsets in the middle of a code point are not
        // valid prefixes and would make the test helper panic before exercising
        // the parser.
        for i in full
            .char_indices()
            .map(|(i, _)| i)
            .chain(std::iter::once(full.len()))
        {
            let prefix = &full[..i];
            let parsed = PartialJson::parse(prefix);

            for (key, field) in parsed.iter() {
                match field {
                    Field::Complete(_) => {
                        let final_field = final_parse.get(key);
                        assert_eq!(
                            final_field,
                            field,
                            "prefix len {i}: key {key:?} Complete but mismatched"
                        );
                    }
                    Field::Partial(s) => {
                        // The partial text must be a prefix of the final string value.
                        if let Field::Complete(Value::String(final_s)) = final_parse.get(key) {
                            assert!(
                                final_s.starts_with(s.as_str()),
                                "prefix len {i}: key {key:?} Partial({s:?}) not a prefix of final {final_s:?}"
                            );
                        }
                    }
                    Field::Missing => {}
                }
            }
        }
    }

    // ── prefix-length (load-bearing) tests ─────────────────────────────────────

    #[test]
    fn prefix_bash_exec() {
        let full = r#"{"mode":"exec","command":"cargo test","intent":"run the test suite"}"#;
        assert_prefixes_sound(full);
    }

    #[test]
    fn prefix_bash_spawn() {
        let full = r#"{"mode":"spawn","command":"cargo run -- serve","cwd":"/home/user/project"}"#;
        assert_prefixes_sound(full);
    }

    #[test]
    fn prefix_delegate_run() {
        let full = r#"{"mode":"run","intent":"fix auth bug","persona":"coder","prompt":"fix the bug in auth.rs"}"#;
        assert_prefixes_sound(full);
    }

    #[test]
    fn utf8_prefixes_are_safe() {
        assert_prefixes_sound(r#"{"message":"修复 🚀"}"#);
    }

    // ── complete parse ─────────────────────────────────────────────────────────

    #[test]
    fn complete_parse_all_complete() {
        let json = r#"{"mode":"exec","command":"cargo test","timeout":5000,"verbose":true,"allow_null":null}"#;
        let parsed = PartialJson::parse(json);

        assert!(parsed.is_key_complete("mode"));
        assert_eq!(parsed.complete_str("mode"), Some("exec"));

        assert!(parsed.is_key_complete("command"));
        assert_eq!(parsed.complete_str("command"), Some("cargo test"));

        assert!(parsed.is_key_complete("timeout"));
        assert_eq!(
            parsed.get("timeout"),
            &Field::Complete(Value::Number(serde_json::Number::from(5000)))
        );

        assert!(parsed.is_key_complete("verbose"));
        assert_eq!(parsed.get("verbose"), &Field::Complete(Value::Bool(true)));

        assert!(parsed.is_key_complete("allow_null"));
        assert_eq!(parsed.get("allow_null"), &Field::Complete(Value::Null));
    }

    // ── partial string ─────────────────────────────────────────────────────────

    #[test]
    fn partial_string_no_closing_quote() {
        let parsed = PartialJson::parse(r#"{"command":"cargo"#);
        assert_eq!(parsed.get("command"), &Field::Partial("cargo".into()));
        assert!(!parsed.is_key_complete("command"));
    }

    // ── missing field ──────────────────────────────────────────────────────────

    #[test]
    fn missing_field() {
        let parsed = PartialJson::parse(r#"{"mode":"exec"}"#);
        assert_eq!(parsed.get("nonexistent"), &Field::Missing);
        assert!(!parsed.is_key_complete("nonexistent"));
        assert_eq!(parsed.str("nonexistent"), None);
        assert_eq!(parsed.complete_str("nonexistent"), None);
    }

    // ── str() helper ───────────────────────────────────────────────────────────

    #[test]
    fn str_helper_complete_and_partial() {
        // Complete string
        let parsed = PartialJson::parse(r#"{"mode":"exec"}"#);
        assert_eq!(parsed.str("mode"), Some("exec"));

        // Partial string (no closing quote)
        let parsed = PartialJson::parse(r#"{"mode":"exe"#);
        assert_eq!(parsed.str("mode"), Some("exe"));

        // Non-string complete (number)
        let parsed = PartialJson::parse(r#"{"count":42}"#);
        assert_eq!(parsed.str("count"), None); // str() only returns for string values
    }

    // ── complete_str() helper ──────────────────────────────────────────────────

    #[test]
    fn complete_str_only_complete() {
        let parsed = PartialJson::parse(r#"{"mode":"exec","cmd":"car"#);
        assert_eq!(parsed.complete_str("mode"), Some("exec"));
        assert_eq!(parsed.complete_str("cmd"), None); // Partial, not Complete
        assert_eq!(parsed.complete_str("nonexistent"), None);
    }

    // ── is_key_complete() ──────────────────────────────────────────────────────

    #[test]
    fn is_key_complete_semantics() {
        let parsed = PartialJson::parse(r#"{"mode":"exec","cmd":"car"#);
        assert!(parsed.is_key_complete("mode"));
        assert!(!parsed.is_key_complete("cmd")); // Partial
        assert!(!parsed.is_key_complete("nonexistent")); // Missing
    }

    // ── number / bool / null ───────────────────────────────────────────────────

    #[test]
    fn number_bool_null_complete() {
        let parsed = PartialJson::parse(r#"{"count":42,"pi":3.14,"neg":-7,"on":true,"off":false,"nil":null}"#);
        assert_eq!(
            parsed.get("count"),
            &Field::Complete(Value::Number(serde_json::Number::from(42)))
        );
        assert_eq!(
            parsed.get("pi"),
            &Field::Complete(Value::Number(
                serde_json::Number::from_f64(3.14).unwrap()
            ))
        );
        assert_eq!(
            parsed.get("neg"),
            &Field::Complete(Value::Number(serde_json::Number::from(-7)))
        );
        assert_eq!(parsed.get("on"), &Field::Complete(Value::Bool(true)));
        assert_eq!(parsed.get("off"), &Field::Complete(Value::Bool(false)));
        assert_eq!(parsed.get("nil"), &Field::Complete(Value::Null));
    }

    #[test]
    fn growing_number_is_not_complete_until_delimited() {
        let parsed = PartialJson::parse(r#"{"count":4"#);
        assert_eq!(parsed.get("count"), &Field::Missing);

        let parsed = PartialJson::parse(r#"{"count":4,"other":true"#);
        assert_eq!(
            parsed.get("count"),
            &Field::Complete(Value::Number(serde_json::Number::from(4)))
        );
    }

    #[test]
    fn nested_values_require_matching_balanced_delimiters() {
        // The nested object is complete even though the top-level object is
        // still streaming, so this exercises the slow path rather than the
        // complete-JSON fast path.
        let parsed = PartialJson::parse(r#"{"value":{"items":[{"id":1}]}"#);
        assert_eq!(
            parsed.get("value"),
            &Field::Complete(serde_json::json!({"items": [{"id": 1}]}))
        );

        // The array is not closed, so the object-looking closer cannot finish
        // the nested value.
        let parsed = PartialJson::parse(r#"{"value":{"items":[1}}"#);
        assert_eq!(parsed.get("value"), &Field::Missing);
    }

    // ── multiple fields streaming ──────────────────────────────────────────────

    #[test]
    fn multiple_fields_streaming() {
        // mode is complete, command key has not yet received its colon
        let parsed = PartialJson::parse(r#"{"mode":"exec","com"#);
        assert_eq!(parsed.get("mode"), &Field::Complete(Value::String("exec".into())));
        // command hasn't arrived yet (key still streaming)
        assert_eq!(parsed.get("command"), &Field::Missing);
    }

    #[test]
    fn multiple_fields_partial_second() {
        // mode complete, command has key and colon but partial value
        let parsed = PartialJson::parse(r#"{"mode":"exec","command":"cargo"#);
        assert_eq!(parsed.get("mode"), &Field::Complete(Value::String("exec".into())));
        assert_eq!(parsed.get("command"), &Field::Partial("cargo".into()));
    }

    // ── edge cases ─────────────────────────────────────────────────────────────

    #[test]
    fn empty_input() {
        let parsed = PartialJson::parse("");
        assert_eq!(parsed.iter().count(), 0);
    }

    #[test]
    fn just_opening_brace() {
        let parsed = PartialJson::parse("{");
        assert_eq!(parsed.iter().count(), 0);
    }

    #[test]
    fn just_key_no_colon() {
        let parsed = PartialJson::parse(r#"{"mode""#);
        // Key is complete (closing quote present) but colon hasn't arrived
        assert_eq!(parsed.get("mode"), &Field::Missing);
    }

    #[test]
    fn key_with_colon_no_value() {
        let parsed = PartialJson::parse(r#"{"mode":"#);
        // Key complete, colon present, but value hasn't started
        assert_eq!(parsed.get("mode"), &Field::Missing);
    }

    #[test]
    fn string_escape_handling() {
        // Complete string with escapes
        let parsed = PartialJson::parse(r#"{"text":"hello\nworld\t!"}"#);
        assert_eq!(
            parsed.get("text"),
            &Field::Complete(Value::String("hello\nworld\t!".into()))
        );
    }

    #[test]
    fn partial_string_with_escapes() {
        // Partial string that ends mid-escape
        let parsed = PartialJson::parse(r#"{"text":"hello\nwor\"#);
        assert_eq!(parsed.get("text"), &Field::Partial("hello\nwor".into()));
    }

    #[test]
    fn iter_yields_all_keys() {
        let parsed = PartialJson::parse(r#"{"a":"1","b":"2","c":"3"}"#);
        let mut keys: Vec<&String> = parsed.iter().map(|(k, _)| k).collect();
        keys.sort();
        assert_eq!(keys, vec!["a", "b", "c"]);
    }

    #[test]
    fn trailing_comma_is_handled() {
        // Some LLMs emit trailing commas — our parser should tolerate them.
        let parsed = PartialJson::parse(r#"{"mode":"exec","command":"ls",}"#);
        // The trailing comma means we'd try to parse another key but find '}'
        assert_eq!(parsed.get("mode"), &Field::Complete(Value::String("exec".into())));
        assert_eq!(
            parsed.get("command"),
            &Field::Complete(Value::String("ls".into()))
        );
    }
}