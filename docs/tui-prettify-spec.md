# TUI Prettify — Implementation Spec

Status: planning complete, ready to implement (by the user, in-agent).
Scope: `crates/firmius/src/tui/*` (+ small additions to `crates/core`).

This spec is the full write-up of everything decided in planning. It is
ordered as a dependency-respecting sequence of phases; each phase is a
mergeable unit with its own tests. Read top to bottom once, then use it as a
checklist.

---

## 0. Baseline facts (read this before touching anything)

- Build is green today (`cargo build`, ~21s clean).
- The TUI is a clean Elm-style split: `model.rs` (state + pure `update`),
  `view.rs` (pure `draw`), `present.rs` (per-tool line rendering),
  `style.rs` (palette), `composer.rs` (input), `modal.rs` (dialogs),
  `command.rs` (slash commands + completion data), `mod.rs` (event loop,
  I/O, async side effects).
- Render pipeline: `AgentEvent` → `fold_event` (model.rs) mutates
  `Vec<Item>` per agent → `view::draw` walks `focused_transcript()` through
  `item_lines` → `present::tool_lines_with_window` for tool calls,
  `markdown::render` for text/thinking → `wrap_lines` reflows everything to
  terminal width → cached in `RenderCache` keyed on `(focused_id, width)`.
- 33ms tick drives ticks/spinner/animation; input events are batched (up to
  128) per redraw, with `ToolCallDelta` events forcing an early flush so
  streaming tool args don't lag behind a burst.

---

## 1. Theme system (new requirement, precedes everything else)

### 1.1 Why first
Every other visual phase (transcript blocks, live-row gradient, status bar,
modals) reads colors from the palette. Build the theme abstraction before
touching any of them, or it all gets rewritten twice.

### 1.2 Data model

New file `crates/firmius/src/tui/theme.rs`:

```rust
use ratatui::style::Color;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Theme {
    pub name: &'static str,
    pub accent: Color,
    pub ok: Color,
    pub err: Color,
    pub warn: Color,
    pub dim: Color,
    pub dim_bg: Color,       // NEW: user-message block background
    pub thinking: Color,
    pub gradient_lo: Color,  // NEW: live-row gradient sweep, cool end
    pub gradient_hi: Color,  // NEW: live-row gradient sweep, warm end
    pub fg: Color,           // NEW: default text (was Style::default())
    pub bg: Color,           // NEW: terminal background reference, for fades
    pub border: Color,
    pub selection_bg: Color, // NEW: modal/completion selected-row highlight
}
```

All colors are `Color::Rgb(r, g, b)` — no named `Color::Cyan` etc. Named
colors don't interpolate for gradients and don't let a theme override a
"dim gray" a user might want warmer.

### 1.3 Five themes

Define as `const` `Theme` values in `theme.rs`:

1. **`firmius`** (default) — today's cyan/green/red/yellow/magenta feel,
   translated to RGB. This preserves the current identity while everything
   else upgrades around it.
2. **`monochrome`** — grayscale only. `accent`/`ok`/`err`/`warn` are
   different *weights* of gray/white, not hues (err gets brightest white +
   bold in practice, but the const itself is still a gray). Good stress
   test: if the UI is legible in monochrome, the state grid (icon/verb/
   subject/meta) is doing its job without relying on color.
3. **`jelly`** — purplish. Accent violet, ok teal-green, err magenta-red,
   warn amber, thinking deep purple, gradient sweeps violet→pink.
4. **`nord`** — the actual Nord palette (nordtheme.com): `#88C0D0` accent,
   `#A3BE8C` ok, `#BF616A` err, `#EBCB8B` warn, `#4C566A` dim,
   `#2E3440`/`#3B4252` bg tones, `#B48EAD` thinking (nord purple).
5. **`gruvbox`** — gruvbox dark palette: `#83A598` accent (blue),
   `#B8BB26` ok (green), `#FB4934` err (red), `#FABD2F` warn (yellow),
   `#928374` dim (gray), `#282828`/`#3C3836` bg tones, `#D3869B` thinking
   (purple).

Each theme gets a unit test asserting every field is `Color::Rgb` (catches
someone slipping a named color back in) and that `gradient_lo != gradient_hi`.

### 1.4 Wiring: current-theme state + switching

- `Model` gains `pub theme: Theme`, initialized from `UserSettings`.
- `UserSettings` (core) gains `pub theme: Option<String>` (theme name,
  `None` = default `firmius`). Persisted like `default_model`.
- New slash command `/theme [name]`: bare lists available themes as a
  completion menu (reuse the `CompletionItem` machinery already in
  `refresh_completion`, same shape as `/model`); with an argument, switches
  immediately and persists to `UserSettings`.
- `theme::all() -> &'static [Theme]` and `theme::by_name(&str) -> Option<Theme>`
  for the command handler and completion provider.

### 1.5 Call-site migration

Everything in `style.rs` (`user()`, `assistant()`, `tool()`, `dim()`, etc.)
changes signature from `fn() -> Style` (module-level consts) to
`fn(theme: &Theme) -> Style`. Every call site (`view.rs`, `present.rs`,
`modal.rs`, `markdown.rs`) threads `&model.theme` through. This is a
mechanical but wide-reaching diff — expect to touch every file in `tui/`.

`markdown.rs`'s `FirmiusMarkdownStyle` currently reads `style::ACCENT`/
`style::DIM` as bare consts at struct-impl time with no theme access,
plus two hardcoded `Color::LightCyan`/`Color::Cyan` for heading levels
2/3. Fix: `FirmiusMarkdownStyle` holds a `Theme` field (`Copy`, cheap to
carry), heading levels 2–4 derive from `theme.accent` at different
lightness rather than hardcoded named colors — needs an `Theme::lighten`/
`darken` helper (see 1.6) so headings stay theme-consistent instead of
always being cyan regardless of theme.

### 1.6 Color math helpers (used by gradient + fades + headings)

```rust
pub fn lerp_color(a: Color, b: Color, t: f32) -> Color; // t clamped [0,1]
pub fn lighten(c: Color, amount: f32) -> Color;  // toward white
pub fn darken(c: Color, amount: f32) -> Color;   // toward black
```

Implement against `Color::Rgb` only; `unreachable!()` (or fall back
unchanged) for other variants since the theme system guarantees RGB.
Unit-test `lerp_color` at t=0, t=1, t=0.5 with known values.

### 1.7 Tests for this phase
- `lerp_color`/`lighten`/`darken` numeric correctness.
- Every theme validates (RGB-only, distinct gradient ends).
- `/theme nord` round-trips through `UserSettings` save/load.
- Snapshot-ish test: render one transcript with each of the 5 themes into
  a `TestBackend`, assert no panic and buffer is non-empty (cheap smoke
  test, not pixel-perfect).

---

## 2. Transcript blocks + word wrap (Phase 1 + Phase 14, combined)

### 2.1 Word-aware wrapping
Replace the char-based `wrap()` (`view.rs:192`) and the char-based
reflow in `wrap_lines()` (`view.rs:238`) with word-aware wrapping:
break at whitespace boundaries when possible, hard-break only when a
single "word" exceeds the width. Must preserve per-span `Style` (including
background) across the reflow — `wrap_lines` today rebuilds spans
char-by-char already, so this is closer to "don't break inside a word"
than a full rewrite; the bg-preservation behavior already works and must
not regress.

### 2.2 User message blocks
`Item::User(t)` (`view.rs:218`) changes from
`wrap(&format!("you: {t}"), style::user(), width)` to a block renderer:

- No `"you: "` prefix.
- Background = `theme.dim_bg`, painted full width including the padding
  columns on short lines (same trick `present.rs:634` already uses for
  diff rows: pad remaining width with a styled space run).
- One blank padding line above, one below, both also full-width
  `dim_bg`-filled (not just empty/transparent lines) so the block reads as
  a solid card, not text-with-background-color.
- Foreground text color is `theme.fg` (not `theme.accent` — the block
  itself is the marker now, the text inside doesn't need to shout).

### 2.3 One-char page gutter
Every `Item` variant's rendered lines get one column of padding on the
left and right of the transcript pane. Implementation: reduce the wrap
width fed into `item_lines`/`wrap_lines` by 2, then prepend/append a
single unstyled space span to every finished line. Must apply *after*
width-dependent budgeting in `present.rs` (`budget_for`, tool line
truncation) so tool lines don't overflow by 2 — thread the gutter-reduced
width down from `draw_transcript`, don't patch it on as a second wrap
pass.

### 2.4 Interaction with nested delegate/bash-tail indentation
The existing `"  │ "` prefixes (delegate nesting, bash live tail) sit
inside the gutter, not outside it — verify visually that nested lines
don't get double-indented once the outer gutter is added.

### 2.5 Tests
- `wrap_lines` word-boundary cases (long word forces hard break, normal
  prose breaks at spaces).
- User block: full-width bg fill on a short one-line message, on a
  multi-line message, verify blank padding lines are bg-filled not empty.
- Gutter: assert every rendered line's first and last cell (in a
  `TestBackend` render) is an unstyled space, for a representative mix of
  Item variants.

---

## 3. `PartialJson` (core, per your decision)

### 3.1 Location
`crates/core/src/partial_json.rs`, exported from `lib.rs`. Reusable and
testable independent of the TUI; the TUI is just the first (only, for
now) consumer.

### 3.2 API

```rust
#[derive(Debug, Clone, PartialEq)]
pub enum Field {
    Missing,
    Partial(String),   // string value still streaming, no closing quote yet
    Complete(serde_json::Value),
}

pub struct PartialJson {
    fields: std::collections::HashMap<String, Field>,
}

impl PartialJson {
    pub fn parse(input: &str) -> Self;
    pub fn get(&self, key: &str) -> &Field;
    pub fn str(&self, key: &str) -> Option<&str>;       // Complete(String) or Partial
    pub fn complete_str(&self, key: &str) -> Option<&str>; // only Complete
    pub fn is_key_complete(&self, key: &str) -> bool;
}
```

### 3.3 Parsing strategy
Do not try to write a general incremental JSON parser. Instead:
1. Attempt `serde_json::from_str::<Value>(input)`. If it succeeds, every
   top-level key is `Complete`.
2. On failure (the common case mid-stream), scan top-level `"key": value`
   pairs with a small hand-rolled tokenizer that tracks brace/bracket/quote
   depth (reuse the approach `partial_string_field` in `present.rs` already
   uses for finding a marker, but generalize it to walk *all* keys, not one
   named field at a time). For each key found:
   - If its value is a string literal with a closing quote → `Complete`.
   - If its value is a string literal with no closing quote before EOF →
     `Partial(text-so-far, unescaped)`.
   - If its value is a number/bool/null token that's fully formed → `Complete`.
   - If the key name itself is still streaming (no `:` yet) → not added
     (equivalent to `Missing`).
3. Values that are nested objects/arrays: treat as `Complete` only if
   brace/bracket-balanced within the input; otherwise report the field as
   present but not string-decodable — acceptable to punt these to `Missing`-like
   handling for v1, since bash/delegate args are flat.

### 3.4 Tests
Feed the tokenizer every prefix-length substring of a real bash/delegate
tool-call JSON blob (`for i in 0..full.len() { parse(&full[..i]) }`) and
assert: no panics, every `Complete` field's value matches the final
parse's value for that key, `Partial` text is always a prefix of the
final string value. This is the load-bearing test — it's what proves the
progressive UI states (phase 6) won't glitch mid-stream.

---

## 4. `intent` field (core schema, required)

### 4.1 Bash (`crates/core/src/tools/bash.rs`)
Add to `BashArgs`:
```rust
/// One short phrase describing what this command accomplishes, e.g.
/// "run the test suite" or "start the dev server". Required for `exec`
/// and `spawn`; shown to the user in place of the raw command while it
/// runs. Not needed for poll/wait/input/resize/kill/list.
#[serde(default)]
intent: Option<String>,
```
Required means: the tool description instructs the model it must always
be provided for `exec`/`spawn`, and `handle()`'s `Mode::Exec`/`Mode::Spawn`
arms call `require(&a.intent, "intent")` alongside `command`. This is a
behavior change for the model, not just presentation — validate against
existing prompts/tests in `crates/core/tests/tools.rs` since some fixture
tool calls will now fail validation without it. Update those fixtures.

Update the tool's top-level description string to state the requirement
explicitly and give 2-3 example intents, since tool-use models follow
worked examples more reliably than a schema comment alone.

### 4.2 Delegate (`crates/core/src/tools/delegate.rs`)
Same shape on `DelegateArgs`:
```rust
/// One short phrase describing the subagent's task, e.g. "integrate auth
/// flow" or "investigate flaky test". Required for `run` and `spawn`.
/// Shown to the user in place of the raw prompt.
#[serde(default)]
intent: Option<String>,
```
Required in `Mode::Run`/`Mode::Spawn` handling. `Mode::Poll`/`Wait`/`Send`
don't need it (they reference an existing `delegate_id`).

### 4.3 Backward compatibility for old sessions
Persisted tool-call history from before this change has no `intent` key.
`PartialJson::get("intent")` on that JSON returns `Missing` — every
presenter that reads intent must fall back to deriving a short label from
`command`/`prompt` (existing `one_line()`/`trunc()` helpers) rather than
panicking or showing an empty string. This fallback path needs its own
test using a pre-existing fixture JSON blob with no `intent` key.

### 4.4 Tests
- `crates/core/tests/tools.rs`: exec/spawn without `intent` now rejected
  with `ToolError::InvalidArguments`; with `intent` succeeds as before.
- Delegate run/spawn: same pattern.
- Schema snapshot test (if one exists) updated for the new required-ish
  field — since it's `Option<String>` with a runtime `require()` check
  (matching the existing pattern for `command`/`proc_id`/etc., not a
  `serde` "required field" — flat-optional-struct-plus-runtime-validation
  is this codebase's established idiom per `bash.rs`'s own doc comment).

---

## 5. `proc_id` keying fix + intent resolver

### 5.1 The bug, precisely
`LocalHost` (`crates/core/src/host.rs:332`) builds `cmdline` as
`"bash -lc <command>"` for the modern single-`command` form. The TUI's
live-tail lookup (`view.rs`'s `bash_cmdline`, `present.rs`'s
`bash_cmdline_from`) reconstructs just `<command>` from the tool-call
JSON. These never match except in the legacy argv form (`args` non-empty,
where program/args pass through unjoined and happen to agree). Net
effect: the live output window silently never appears for the common case.

### 5.2 Fix
Key everything on `proc_id` instead of a reconstructed command string.
- `Model.host_tails` changes from `HashMap<String, String>` (cmdline →
  tail) to `HashMap<ProcId /* or its string repr */, String>`.
- Populate it in `refresh_async` (`mod.rs:766`) keyed on `info.id`
  (already iterating `ProcInfo`, already has `.id` — this is a smaller
  diff than the current cmdline-join code, not larger).
- Presenters look up the tail by the `proc_id` parsed out of the tool's
  **result** (not its args — `proc_id` isn't known until the spawn
  response streams back, unlike `intent` which is known from the args).
  For a `Preparing`/`Running` bash call, the proc_id isn't available yet
  until the call transitions out of preparing with a result containing
  `proc_id=<n>`. Before that, no tail lookup is attempted (this is already
  implicitly true today since preparing has no id either, just needs to
  survive the refactor).

### 5.3 Intent resolver
New `Model` fields:
```rust
pub proc_intents: HashMap<String, String>,      // proc_id -> intent
pub delegate_intents: HashMap<String, String>,   // delegate_id -> intent
```
Populated when a spawning call's args (`intent` field) become available
via `PartialJson` during streaming — don't wait for the call to finish,
since the whole point is showing the intent *while* still preparing. Keyed
provisionally by tool-call `stream_id`/`stream_index` until the real
`proc_id`/`delegate_id` shows up in the result, then re-keyed. (Mirrors
the existing id-reconciliation pattern `fold_event` already uses for
merging `ToolCallDelta` into `ToolCallStarted` — same shape, different
map.)

### 5.4 Tests
- Unit test: spawn a process with `command` form (not legacy argv),
  verify `host_tails` lookup by `proc_id` succeeds (this is the regression
  test for the bug itself — should fail today, pass after the fix. Write
  it *before* the fix as a red test if practical).
- `proc_intents`/`delegate_intents` populate correctly from a streamed
  partial args string containing `intent` before `command`/`proc_id`
  arrive.

---

## 6. Presenter redesign language (Phase 5)

### 6.1 The grid
Every tool-call line: `[icon][verb][subject][meta]`.
- **icon**: one glyph + one space, always occupies the same column no
  matter the tool, so verbs align vertically down the whole transcript.
  States: preparing (hollow, dim), running (spinner, accent), done-ok
  (check, ok color), done-err (x, err color), interrupted (⊘, err color,
  but *dim* not bright — it didn't fail, it was cut off).
- **verb**: present tense while running/preparing, past tense once done.
  `reading`/`read`, `listing`/`listed`, `searching`/`searched`,
  `running`/`ran`, `delegating`/`delegated`, `editing`/`edited`. Verb
  color: dim while preparing, `theme.fg` (not accent) while running so
  color is reserved for icon-state, neutral dim once done. This is the
  "one accent per state, not per tool" rule — kills the current
  everything-is-cyan sameness.
- **subject**: the thing acted on — path(s), search pattern, command,
  delegate intent. Paths get directory-dim + basename-bright styling: split
  on the last `/`, style the head with `theme.dim`, the tail (filename)
  with `theme.fg` or bold. This is the single highest-value micro-detail
  since so many lines are paths.
- **meta**: right-aligned, always `theme.dim`, always last — elapsed time
  while running, byte count + elapsed once done. Existing `fmt_bytes`/
  `elapsed_secs` helpers stay as-is, just repositioned consistently.

### 6.2 Nesting gutter, unified
One indent primitive replacing the two current ad hoc `"  │ "` prefixes
(delegate nested lines in `present.rs:75`, bash tail lines in
`present.rs:393`/`view.rs`'s generic tail rendering). Single function:
```rust
fn nest(lines: Vec<Line<'static>>, theme: &Theme) -> Vec<Line<'static>>
```
applied identically regardless of *why* something is nested (bash tail vs
delegate child vs future nesting need).

### 6.3 Builder helper
Given the grid is now used by every presenter, factor a shared line
builder:
```rust
fn tool_line(icon: (&str, Style), verb: (&str, Style), subject: Vec<Span<'static>>, meta: &str, theme: &Theme, width: u16) -> Line<'static>
```
Rewrite `bash_lines`, `delegate_lines`, `edit_lines`, `quick_lines`,
`generic_lines` on top of it. Mechanical but touches every function in
`present.rs`.

### 6.4 Tests
Per presenter: preparing/running/done-ok/done-err/interrupted line shape
(plain-text extraction + style assertions), plus a width-budget test
(narrow terminal doesn't overflow or panic).

---

## 7. Progressive preparing states (Phase 6)

Built entirely on `PartialJson` (§3) + `intent` (§4) + the resolver (§5).

### 7.1 Bash
State derivation, evaluated fresh each render from the accumulating args
string via `PartialJson::parse`:

| condition | text |
|---|---|
| `mode` field not yet complete | *(no line yet, or icon+nothing — avoid key-name noise)* |
| `mode == "wait"`, no `proc_id` yet | `waiting for process` |
| `mode == "wait"`, `proc_id` complete, resolvable via `proc_intents` | `waiting for "Build and run tests"` |
| `mode == "poll"` | same pattern: `polling process` → `polling "…"` |
| `mode == "kill"` | `killing process` → `killing "…"` |
| `mode == "list"` | `listing processes` (terminal immediately, no id wait) |
| `mode == "exec"`/`"spawn"`, `intent` streaming | show the intent text as it grows (this *is* the subject) |

Critical constraint from your spec: **once the call leaves `Preparing`,
the text does not change** — only the icon swaps from hollow/dim to
spinner/accent. Implementation: compute the "settled" text once, at the
moment `ToolCallStarted` fires (transition to `Running`), and store it
*on the `Item::ToolCall`* (new field, e.g. `settled_label: Option<String>`)
rather than recomputing from args on every render. Preparing-phase
renders read live from `PartialJson` each frame; running/done-phase
renders read the frozen `settled_label`. This avoids flicker if args
happen to still be present but reformat differently on relookup.

### 7.2 Delegate
| condition | text |
|---|---|
| args just starting | `delegating` |
| `intent` streaming, no `persona` yet | `delegating "Integrate auth fl"` |
| `intent` + `persona` (persona may arrive first or second — check both independently, do not assume order) | `delegating to Coder: "Integrate auth fl"` |
| left preparing (running) | `delegated to Coder: "Integrate auth flow" [deepseek-v4-pro, high]` — model/effort appended only here, since they're not decided until the subagent config is built |
| `mode == "send"` | separate line shape entirely: `messaging <parent|child>: "…"` |

Same freeze-on-leaving-preparing rule as bash.

### 7.3 Tests
Drive each state table above with `PartialJson` fed progressively longer
prefixes of real args JSON (reuse the §3.4 fixture technique), asserting
the rendered text at each checkpoint matches the table, and that once
`Running` is reached the label never changes on subsequent re-renders
even if (hypothetically) args were re-parsed.

---

## 8. Quick-tool grouping (Phase 7)

### 8.1 Grouping rule
At the point `view.rs::draw_transcript` walks `focused_transcript()` into
`item_lines`, fold **consecutive** `Item::ToolCall` entries where
`name == "read"` (or all `"list"`) into one group, rendered as one
presentation block instead of N separate lines. Non-consecutive same-tool
calls (something else in between) render individually as today.

### 8.2 Read grouping format
```
read src/this.rs:100-247, src/is.rs:309-490, src/cool.rs:202-800,
     src/bro.rs
```
- `read` args are `path` + optional `offset`/`limit` — compute the
  displayed range as `offset..offset+limit` (or "full read" / no range
  suffix when `offset`/`limit` are absent, per your `src/bro.rs` example).
- Continuation lines align under the verb's start column (`read ` width),
  not under column 0.
- Wrap the comma-joined list at the transcript width, same as prose wrap.

### 8.3 List grouping format
```
listed src/lib, include/wow/long/path, hey/bro/this/is/cool
```
Same fold, no line-range suffix (list has no offset/limit).

### 8.4 Streaming + mixed-state handling
A group may have its **last member still preparing/running** while
earlier members are done. Render: earlier members' paths in `theme.fg`,
the in-flight member's path with the running icon's accent color, group
icon overall reflects the *worst* state present (any failure → err icon;
otherwise running if any member running; otherwise ok). Mixed ok/fail
within a finished group: failed paths get `theme.err` fg individually
(inline per-item color, not a separate line) rather than splitting the
group — keeps the "one line" promise from your spec intact.

### 8.5 Explicitly excluded
`grep` and `glob` are quick tools but **do not group** — stated
requirement, keep them on `quick_lines` individually.

### 8.6 Tests
- 3 consecutive reads fold into one grouped line with correct ranges.
- A read, then a grep, then two more reads → two separate read groups
  (not one group of 3, correctly broken by the grep).
- Grouped read where the last item is still `Preparing` renders with
  mixed styling and doesn't panic on incomplete args.
- `list` grouping mirrors the read tests without ranges.

---

## 9. Compact edit presenter for delegate windows (Phase 8)

### 9.1 Problem
`edit_diff_lines` (`present.rs:579`) renders the *entire* patch — headers,
hunk markers, every added/removed line with syntax highlighting. Inside a
top-level transcript that's the desired rich view. Inside a **nested
delegate child window** (`nested_tool_lines`, `view.rs:338`, capped at 3
lines via `.take(3)`) it blows the budget and the diff preview leaks
through anyway since `take(3)` just truncates mid-diff rather than
summarizing.

Also: `EDIT_DIFF_MAX` (`present.rs:22`) is declared and never referenced
— dead code, confirms this was already flagged as needed and never wired.

### 9.2 Compact mode
New function, used only by the nested/delegate-child rendering path:
```rust
fn edit_lines_compact(args: &str, state: &ToolState, width: u16) -> Vec<Line<'static>>
```
- No diff body, ever.
- Per file: `name +N -M`, counts computed the same way
  `file_change_counts` already does, but **streamed live** — as more of
  the patch JSON arrives, recompute counts from however much patch text
  has streamed in so far (this works today's `file_change_counts` as-is,
  since it just scans lines; feed it the partial patch string).
- Multiple files packed onto one line separated by `"  "` when they fit
  within `width`; overflow wraps to a second line; hard cap 3 lines total
  (drop/collapse further files into a `+N more` suffix past 3 lines).
- Use `edit_file_summary`'s existing `~`/`+`/`-` markers per file.

### 9.3 Wiring
`nested_tool_lines` (`view.rs:338`) calls `edit_lines_compact` instead of
the full `tool_lines` when `name == "edit"`; every other tool in that
nested path keeps using the regular (already-compact) presenters.

### 9.4 Tests
- Single-file patch streamed progressively: line counts update, never
  exceeds 1 line for a single small file.
- Multi-file patch (5 files): confirms pack-then-wrap-then-cap-at-3
  behavior, with a `+N more` tail when truncated.

---

## 10. Composer fixes (Phase 10, independent — can land anytime)

### 10.1 Word navigation
New `Composer` methods:
```rust
pub fn word_left(&mut self);
pub fn word_right(&mut self);
pub fn backspace_word(&mut self);
```
Word boundary = transition between `char::is_whitespace` and not,
scanning within the current text segment; a `Segment::Paste` boundary
counts as a full word-stop (matches the existing atomic-paste rule used
by `left`/`right`/`backspace`) — i.e. word movement never steps into the
middle of a paste placeholder, it hops over the whole block same as today's
char-level movement does.

### 10.2 Key wiring
In `model.rs::key`, the existing plain arms:
```rust
C::Backspace => { self.composer.backspace(); ... }
C::Left => { self.composer.left(); ... }
C::Right => { self.composer.right(); ... }
```
gain modifier-gated variants checked *before* the plain arm (since match
arms are order-sensitive and Rust match doesn't guard-merge automatically
here — use `if m.contains(KeyModifiers::ALT)` guards, mirroring the
existing `C::Enter` arm's `if m.contains(KeyModifiers::ALT) || ...`
pattern already in the file):
```rust
C::Backspace if m.contains(KeyModifiers::ALT) => { self.composer.backspace_word(); Action::Continue }
C::Left if m.contains(KeyModifiers::ALT) => { self.composer.word_left(); Action::Continue }
C::Right if m.contains(KeyModifiers::ALT) => { self.composer.word_right(); Action::Continue }
```

### 10.3 Height-clip fix (the "long text doesn't render" bug)
Root cause is **not** width truncation — `wrap_line`/`lines_with_width`
wrap correctly with no cap. The bug is in `view.rs`:
```rust
let composer_h = (composer_lines.len() as u16 + 2).clamp(3, 10).min(area.height / 2 + 3);
```
Composer viewport caps at 10 total rows (8 content rows) and
`draw_composer` hands the **entire** line vec to one `Paragraph` with no
scroll offset — ratatui silently clips anything beyond the widget height,
with zero visual indicator.

Fix: composer needs its own `Viewport`-style follow-the-cursor scroll,
same pattern the transcript already uses (`view.rs:329-333`):
- Track a `composer_scroll_offset` on `Model` (or recompute each frame
  from cursor row vs. visible height — recomputing is simpler and avoids
  another piece of persistent state to keep in sync).
- When content rows exceed the visible cap, offset so the cursor's row
  stays within the visible window (scroll up as the user types past the
  bottom, scroll down on backspace/up-arrow past the top).
- Add a truncation indicator when clipped either direction — e.g. a dim
  `⋯` glyph in the composer border/corner, or a `(+N lines above/below)`
  hint — so a user editing a long paste can *tell* there's more content
  rather than silently losing it off-screen.

### 10.4 Tests
- `word_left`/`word_right` over `"foo bar  baz"` land on expected offsets
  (multiple spaces, leading/trailing whitespace, single-word buffer).
- `backspace_word` from various cursor positions, including right after a
  paste block (should delete the whole block, matching plain backspace's
  existing atomic behavior) and from the middle of a word (should still
  jump to the start of the *current* word, not the previous one — this is
  the standard word-backspace mental model, worth a targeted test since
  it is the easiest cusp condition to get wrong)
- Composer scroll: a 20-line buffer in an 8-row viewport, cursor moved to
  various rows, assert the visible window always contains the cursor row.

---

## 11. Live row: gradient + staggered fade (Phase 9)

### 11.1 State machine
On `Model`:
```rust
pub enum PhraseAnim {
    Steady,
    FadingOut { since: Instant, from: String },
    FadingIn  { since: Instant, to: String },
}
pub live_phrase_anim: PhraseAnim,
pub live_phrase_current: String, // committed text, drives Steady rendering
```
Trigger: whenever `activity_phrase()`'s underlying inputs change (verb
rotation tick, or a real transition like idle→"Running bash…"), instead
of directly overwriting the rendered phrase, enter `FadingOut { from: <old
committed phrase> }`. On completing fade-out (all chars gone), flip to
`FadingIn { to: <new phrase> }`. On completing fade-in, flip to `Steady`
and commit `live_phrase_current = to`.

### 11.2 Fade-out mechanic (per your spec, precisely)
Not per-char opacity independently — the string **shortens by one
character every `stagger_ms`**, and the currently-disappearing trailing
character fades from its live gradient color toward `theme.bg` over that
same stagger window (linear interpolation via `lerp_color`), so each
character gets one brief "about to vanish" frame before being dropped
from the string entirely:
```
Thinking...   (full)
Thinking..    (one char shorter; new trailing '.' mid-fade this instant)
Thinking.
Thinking
Thinkin
...
```
Fade-in mirrors this exactly in reverse: string **grows** by one char
every `stagger_ms`, newly-appended trailing char fades `theme.bg` → its
live gradient color.

Suggested constants: `stagger_ms = 40`, giving a ~10-char phrase a ~400ms
transition each direction — tune by feel once visible, not load-bearing
to get exactly right up front.

### 11.3 Gradient sweep (independent of the fade, composes with it)
While `model.busy`, every visible character (not just during a fade —
this runs continuously) is colored by a moving sweep position:
```rust
pub fn gradient_at(theme: &Theme, t: f32, len: usize, i: usize) -> Color
```
`t` derived from elapsed wall time (e.g. `(Instant::now() - start).as_secs_f32()`
mod some period), producing a triangular or cosine falloff so the sweep
loops forever without a visible seam. The character being faded (§11.2)
uses **this function's output as its fade source color**, not a flat
color — i.e. `lerp_color(gradient_at(...), theme.bg, fade_progress)`.
This is the one place the two mechanics compose and is worth its own
dedicated test.

Idle state (not busy): no gradient, no animation, single dim `theme.dim`
static text or nothing — per the earlier status-bar discussion, idle
should recede rather than say "idle" in prose.

### 11.4 Module placement
Extract from `draw_top_bar` (`view.rs:370`) into a new `live_row.rs`:
`pub fn draw(model: &Model, frame: &mut Frame, area: Rect)`. The existing
`activity_phrase()` function (`view.rs:413`) becomes the *input* to the
anim state machine rather than the direct render source.

### 11.5 Tests
- `gradient_at` produces a continuous loop (value at `t` and `t + period`
  match).
- Fade-out state machine: given a committed `stagger_ms`, after N stagger
  intervals the string is N chars shorter (pure function of elapsed time,
  no real sleep needed in tests — construct `Instant` deltas directly or
  parameterize the anim step function on an elapsed duration rather than
  wall-clock `Instant::now()` internally, specifically so this is testable
  without real time passing).
- Full transition: `Steady("Thinking…")` → new phrase arrives → after
  full fade-out+fade-in duration, state is `Steady("Cogitating…")`.

---

## 12. Status bar redesign (Phase 11)

### 12.1 Top row
Becomes the live row from §11 in the busy case; idle case per §11.3.

### 12.2 Context row
Keep the `▰▱` progress bar (`present::progress_bar`, unchanged mechanic)
but color it via `lerp_color(theme.gradient_lo, theme.gradient_hi, usage_fraction)`
once usage exceeds some threshold (suggest 70%), rather than the current
flat `style::user()`. Below threshold, stays `theme.accent` as today. No
new "warning" red — this reuses the gradient theme concept rather than
introducing a third color language, per your "no jarring red" note earlier
in planning discussion... *(cross-check: I don't think we actually
finalized "no jarring red" as a hard rule — treat the gradient-based
warning as the default proposal, open to revisiting to a harder err-color
cutover near 100% if a soft gradient reads too subtle in practice.)*

### 12.3 Bottom row
Split into a left cluster (model chip colored by provider — needs a
provider→color mapping, simplest is hashing the provider id string into a
stable hue rather than hand-maintaining a table) and a right cluster (key
hints, unchanged content). The transient flash (`model.note`) currently
**replaces** the whole line (`view.rs:511-527`); change it to live beside
the clusters (e.g. inserted between them, own dim/note-colored span)
rather than hiding the model/focus info while a flash is showing.

### 12.4 Tests
- Context bar color crosses from accent to gradient at the 70% threshold
  boundary (test the color function directly, not full render).
- Bottom row layout: flash present vs absent, assert both clusters still
  render (regression test for the "flash currently hides everything else"
  behavior being fixed).

---

## 13. Modal + completion polish (Phase 12)

- `ListInput::render_lines` (`modal.rs:103`): selected row gets a
  `theme.selection_bg` background span across the *full row width*, not
  just fg-colored text. Requires knowing the row's target width at render
  time — thread it in similarly to how tool lines get their width budget.
- Scroll indicator for any list modal (`AccountsModal`, `SettingsModal`,
  `PersonasModal`, the completion popup) when options exceed visible
  height: a small `▲`/`▼` in a corner, or a proportional scrollbar sliver
  along the border — pick whichever is cheaper to implement consistently
  across all of them; a shared helper function is preferable to
  reinventing per-modal.
- Completion popup (`draw_completion`, `view.rs:141`): same selection-bg
  treatment as modals; align `label`/`detail` into two columns (fixed
  label width, detail starting at a consistent column) instead of the
  current single concatenated string, so long labels don't push details
  unevenly.
- Unify modal chrome and composer chrome under the same rounded-border +
  title weight — largely already true (`draw_chrome` and the composer
  block both use `BorderType::Rounded`), audit for any remaining
  divergence once themed colors are in and visible together.

### 13.1 Tests
- Selected-row background renders full width in a `TestBackend` for both
  a short and a long option label.
- Scroll indicator appears only when options overflow the visible area,
  absent otherwise.

---

## 14. Welcome screen (Phase 13, waits on your logo)

- Add `pub const LOGO: &str = "";` (placeholder) somewhere sensible —
  `theme.rs` or a new `welcome.rs` — for you to paste the real ASCII/ANSI
  art into once ready.
- Replace the current five-flat-lines welcome (`view.rs:278`) with:
  centered logo block, then a weighted onboarding block below it — one
  primary CTA line styled distinctly (e.g. `theme.accent` + bold: "Set up
  an account with /login"), the rest as `theme.dim` secondary hints
  (model selection, resume).
- This phase has no hard technical dependencies beyond §1 (theme) — safe
  to do anytime after the theme lands, doesn't need to wait for the tool
  presenter track at all.

---

## Suggested commit sequence (mirrors the numbering above)

1. Theme system (§1) — mechanical, wide, but low-risk; compiles clean at
   each step if done as: add `theme.rs` → thread `&Theme` through
   `style.rs` fns → thread through call sites file-by-file → wire
   `/theme` command last.
2. Word wrap + transcript blocks (§2) — first visually obvious payoff.
3. Composer fixes (§10) — fully independent, good to interleave whenever
   there's a natural break.
4. `PartialJson` in core (§3) — pure, no UI wiring yet, heavy test focus.
5. `intent` field (§4) — core schema + fixture updates.
6. `proc_id` keying fix + intent resolver (§5) — fixes the live-window
   bug as a side effect; write the regression test red-then-green.
7. Presenter redesign language (§6) — the big mechanical rewrite of
   `present.rs`.
8. Progressive preparing states (§7) — now everything downstream exists.
9. Quick-tool grouping (§8).
10. Compact edit presenter (§9).
11. Live row gradient + fade (§11) — depends only on theme, could move
    earlier in the sequence if you want the flashy part sooner; ordered
    here because it was designed after the presenter work in planning,
    not because of a hard dependency.
12. Status bar redesign (§12).
13. Modal + completion polish (§13).
14. Welcome screen (§14) — whenever the logo is ready.

Every phase: `cargo build` clean, `cargo test -p firmius` (and `-p
firmius-core` for §3/§4/§5) green, before moving to the next. Commit per
phase, not per file — keeps `git log` legible for reviewing this later.
