# FIRMIUS TUI SKINS — UX DESIGN DOCUMENT

## Overview

Firmius will support two TUI skins: **Firmius** (the original, richly-instrumented experience) and **Claudex** (a streamlined, Claude-Code-meets-Codex experience that strips the chrome and lets the terminal breathe). Each skin defines its own layout philosophy, component visibility rules, rendering style, and per-skin configuration options. Users switch skins via `/skin` command or `preferences.json`.

The skin system sits **above** the existing theme system. A theme controls _colors_. A skin controls _structure, density, visibility, and behavior_. Any theme can be used with any skin.

---

## Competitor Analysis (from screenshots)

### Claude Code (Screenshot 1)
- **Layout**: Full-width single scroll. No panels, no sidebars.
- **Tool calls**: Inline `● Icon(target)` with `└` tree connectors for results. Collapsed by default with `ctrl+o to expand`.
- **Status**: Single bottom line: `»» bypass permissions on (shift+tab to cycle)`.
**Input**: Promptless blinking cursor. No box, no border, no background.
- **Density**: Extremely tight. No blank lines between agent output blocks. No turn separators.
- **Colors**: Pink for tool icons, green for success, white for content, gray for metadata.
- **Feel**: Hacker terminal. Fast. Raw. Every pixel is content.

### Codex (Screenshot 2)
- **Layout**: Full-width single scroll. No sidebars.
- **Tool calls**: `● Ran command` with `└` indented output. Collapsed with `… +16 lines`.
- **Plan rendering**: Inline in chat as `Updated Plan` with `□` / `■` checkboxes. No separate panel.
- **Status**: Single bottom line: `gpt-5.4 high · 99% left · ~/biz/code/demos/recipe-generator`.
**Input**: Promptless blinking cursor with hint text.
- **Density**: Compact. Subtle spacing. Plan items are part of the chat flow.
- **Colors**: Cyan/teal accents, muted grays, bold white headers.
- **Feel**: Polished CLI tool. Opinionated simplicity. Context in the status line.

### OpenCode (Screenshot 3)
- **Layout**: Split — chat left (70%), info panel right (30%).
- **Info panel**: Context (tokens, cost), MCP connections, LSP status, Todo items.
- **Tool calls**: `✱ Grep` / `→ Read` prefix style with inline args.
- **Thinking**: `Thinking:` label in yellow/orange, shown as conversational text.
- **Agent identity**: `Sisyphus - Ultraworker · minimax-m2.5-free · interrupted` as footer.
- **Status**: Bottom bar with keyboard shortcuts, agent selector, provider info.
- **Feel**: IDE-adjacent. Information-rich without being cluttered.

### Firmius (Screenshots 4, 5)
- **Layout**: Title bar → chat → work panel (tabbed PLAN/TODO/CONTEXT) → agent strip → input → status bar.
- **Tool calls**: Bordered blocks with colored headers, icons, syntax-highlighted content.
- **Turn footers**: `✓ done · turn 215 · 18s · ↑133.7k/918 ↓799` — full telemetry.
- **Live footer**: `aster · gpt-5.2 · 35m35s` with elapsed timer.
- **Status bar**: Powerline with glint animation, responsive breakpoints at 110/70 chars.
- **Work panel**: Tabbed with drag-to-resize, plan cuts with status icons.
- **Agent strip**: Multi-agent roster with hierarchy indicators, context bars, pills.
- **Feel**: Mission control. Dashboard for orchestration. Every agent, every metric, always visible.

---

## Skin 1: CLAUDEX

> **Name origin**: **Claud**e Code + Cod**ex** + Firmi**us**. Sounds like a Latin word (cf. _codex_, _index_). Also a nod to the Roman tradition — if Firmius is the patrician general, Claudex is the populist tribune who strips the ceremony and talks to the people directly.

### Philosophy

Strip everything to its bones, but keep the single most important magical surface: a **persistent live row status bar inside the transcript**. Claudex is not just "less UI" — it is a chat-first interface where one live updating row becomes the user's emotional and operational anchor. The terminal IS the experience — no chrome, no panels, no visual noise. Everything the user needs to know is either in the chat stream, in the persistent live row, or compressed into the bottom status line. Make it feel _magical_ through animation, glint, cheeky runtime copy, and information density — not through UI furniture.

### Layout

```
╔══════════════════════════════════════════════╗
║                                              ║
║  [Chat scroll - takes ENTIRE terminal]       ║
║                                              ║
║  ● Read packages/core/src/Engine.cpp         ║
║    └ 342 lines                               ║
║                                              ║
║  Now I understand the lifecycle. Let me...    ║
║                                              ║
║  ● Edit packages/core/src/Engine.cpp         ║
║    └ 3 changes · +12 -4                      ║
║                                              ║
║  ● Bash cmake --build build --target core    ║
║    └ Running...                              ║
║                                              ║
║  ✦ Thinking through the blast radius...      ║
║  aster · Claude Opus 4 · streaming · 2m14s   ║
║                                              ║
║▊                                             ║
║aster · opus-4 · ↑45.2k ↓3.1k · 34% · ASK     ║
╚══════════════════════════════════════════════╝
```

**Four zones only:**
1. **Chat area** — takes all available height minus live row, input line, and bottom status line
2. **Persistent live row** — always present while an agent is active; lives directly above the input, updates in place
3. **Input line** — promptless blinking cursor, sits directly below the live row with no separator
4. **Status line** — single row at the very bottom, no powerline segments

**Removed entirely:**
- Title bar
- Agent strip
- Work panel (plan/todo/context tabs)
- All horizontal separators between zones
- Drag-to-resize handles

### Persistent Live Row — The Source of Truth

This is the core Claudex surface.

It is a **single persistent, live-updating transcript-adjacent row** that sits above the input at all times whenever the focused agent is active. It replaces the need for a work pane, replaces the need for verbose live footer spam, and gives the user one glanceable source of truth.

It combines:
the **Claude-style cheeky live phrase**
the **Codex-style gradient/glint treatment**
the **actual runtime state** from Firmius: status, tool phase, elapsed time, focused persona, and optional mini plan/todo excerpt

Example states:

```text
✦ Thinking through the blast radius...                     aster · opus-4 · 2m14s
✦ Naming the uncertainty before we touch code.             aster · opus-4 · provider_waiting · 41s
✦ Reading just enough to avoid doing something stupid.     aster · opus-4 · read 3 files · 19s
✦ Tightening the cut. No heroics.                          aster · opus-4 · editing Engine.cpp · 1m08s
✦ Running the proof, not just admiring the patch.          aster · opus-4 · bash test_core · 33s
```

Behavior:
**Persistent**: it does not scroll away while the turn is still live
**Live-updating**: message, elapsed time, and activity label update in place
**Glint-heavy**: the leading phrase and/or active segment use `GlintEffect`
**Cheeky but truthful**: the fun phrase cycles independently, but the right-hand facts are runtime truth
**Optional projection area**: on wider terminals, the far right can temporarily show `next: ...` from todo or `cut: ...` from plan

Phrase system:
Rotates every 4–8 seconds while the agent is active
Chooses from a per-skin phrase bank, grouped by mode: thinking, reading, editing, verifying, waiting
Can be deterministic-per-turn or randomized
Must never replace factual status; it decorates the left half only

Important rule:
This row is **not** a completion footer and **not** a normal transcript line. It is a persistent HUD row rendered by layout, not appended to history.


### Chat Rendering
#### User Messages
```
> What's the architecture of the engine?
```
- Simple `> ` prefix, bold, in user color
- **No background bubble** — raw text on terminal background
- **No blank line above/below** — content flows tightly
- Images shown as inline `[IMAGE 1]` tags (no change from current, but no blank spacer lines)

#### Agent Text
```
  The engine manages agent lifecycles through a singleton
  pattern. Each agent runs in a dedicated jthread...
```
- 2-character indent (matches current `IndentAgentRow`)
- No prefix character — the indent itself distinguishes agent from user
- Markdown rendering preserved (bold, code blocks, headers, links)
- **No blank line after** agent text blocks

#### Thinking Blocks
```
  Thinking: Let me check the provider registry to understand
  how factories are registered...
```
- `Thinking:` label in a warm color (amber/yellow, configurable via theme)
- Dimmed text, italic feel (via `dim` decorator)
- Can be hidden entirely via config

#### Tool Calls — The Core Innovation

Instead of bordered blocks with headers, tools render as **compact inline rows** inspired by Claude Code's `● Icon(target)` pattern:

**Read/List/Grep (quick inspection — already clustered):**
```
  ⊕ read packages/core/src/Engine.cpp, Agent.cpp, Harness.hpp
  ⊕ searched "registerProvider" in packages/provider/src
```
- Colored pill icon (`⊕`) + action verb + targets
- Glint animation on the icon when live/in-progress
- Grouped by category (same as current quick-tool clustering logic)

**File Edit:**
```
  ⊙ Edit(packages/core/src/Engine.cpp)
    └ 3 hunks · +12 -4 · diff hidden
    ▸ press ctrl+g to reveal diff
```
- `⊙` icon for edits (distinct from reads)
Diffs are **collapsed by default, never removed**
`ctrl+g` reveals the diff inline for the focused edit block
Expanded state shows syntax-aware diff rendering with `+`/`-` prefixes
On wide terminals, expanded diff can show up to 12 lines before secondary collapsing
On narrow terminals, show first 6 lines plus `... +N more`
- **No bordered box** — tree connector `└` and `│` for visual grouping

Expanded example:
```
  ⊙ Edit(packages/core/src/Engine.cpp)
    └ 3 hunks · +12 -4
    │+ #include <optional>
    │-   int retries = 0;
    │+   int retries = 1;
    │+   logRetryBudget(retries);
    │  ... +8 more lines (ctrl+g to collapse)
```

**Process Execution (Bash/Shell):**
```
  ⊘ Bash cmake --build build --target core
    └ exit 0 · 2.7s
      pygame 2.6.1 (SDL 2.28.4, Python 3.13.13)
      Hello from the pygame community.
      {'steps': 10, 'output_dir': 'runs/text_smoke2'}
    ... +12 lines (ctrl+g to toggle)
```
- `⊘` icon for terminal commands
- Command shown in full on first line
- Exit code and duration on result line
- Last N lines of output shown, with `... +N lines` for overflow
- Glint on icon while running
- When running: `⊘ Bash cmake --build build ── Running... (ctrl+c to interrupt)`

**Subagent Delegation:**
```
  ◈ Spawned Forge (ember) · claude-sonnet-4 · implementing auth module
    └ done · 45s · 12 tool calls
```
- `◈` icon for subagent work
- Shows persona, model, and task summary
- Result shows duration and tool call count
- Click/interact to focus that agent (preserved from current behavior)

**Generic/Unknown Tools:**
```
  ◉ WebSearch("firmius TUI patterns")
    └ 3 results
```
- `◉` icon for anything not specifically categorized
- Tool name + args on one line, result summary indented

#### Plan/Todo Inline Rendering

When a plan or todo exists, instead of a side panel, render it **inline in the chat stream** as a distinct block (inspired by Codex's `Updated Plan` pattern):

**Plan (when updated):**
```
  ╭─ Plan: Full Windows/macOS migration
  │ ✓ Cut 1: Shared portability gate
  │ ✓ Cut 2: LSP portability architecture gate
  │ ✓ Cut 3: macOS clipboard completion
  │ ◉ Cut 4: Verification surface rehabilitation
  │ ○ Cut 5: Cross-platform CI codification
  ╰─
```
- Rendered once when plan state changes, not continuously visible
- Uses box-drawing characters for containment
- Status icons: `✓` done, `◉` in-progress, `○` pending

**Todo (when updated):**
```
  ╭─ Todo
  │ ✓ Explore TUI package structure
  │ ◉ Study TUI components
  │ ○ Design skin configurations
  ╰─
```
- Same treatment — inline, rendered on change, not a permanent panel

#### Turn Footers

Removed for Claudex. No `done · 23s`, no turn-number footer, no completion telemetry row. Completion is communicated by the transcript itself and by the persistent live row disappearing or settling back to idle.

### Status Bar

Single line. No powerline. No background segments.

**While working:**
```
aster · opus-4 · ↑45.2k/48k ↓3.1k · 34% · ASK · ⊘ 1 bg
```

**While idle:**
```
aster · opus-4 · ↑45.2k/48k ↓3.1k · 34% · ASK
```

Segments separated by ` · ` (middle dot with spaces). Color-coded:
- Agent name: theme highlight color
- Model: default text
- Token counts: dim
- Context percentage: green/yellow/red based on usage level
- Permission mode: colored by mode (ASK=neutral, AUTO=green, DENY=red)
- Process indicator: only shown when background processes exist

**Glint animation**:
 - The model name segment gets the glint sweep when streaming
 - The persistent live row phrase also uses glint/gradient treatment
 - The bottom status bar is factual; the persistent live row is where the flourish lives

### Input

```
▊
```

Just the cursor, no prompt glyph
- No background color, no border, no placeholder text
- Permission requests render as an inline block above the input line:
  ```
  ⚠ Permission Required: Write to packages/core/src/Engine.cpp
    [1: Allow] [2: Allow All] [3: Deny]     Esc to deny
  ▊
  ```

### Animations & Polish

**Glint on persistent live row text** while streaming/thinking/verifying
**Glint on status bar model name** while streaming
**Glint sweep on live tool icons** (`⊕`, `⊙`, `⊘`) while executing
- **Spinner character** cycling (`⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏`) next to running process commands
**Gradient-like treatment** on the persistent live row to evoke Codex's polished status surface
**Phrase cycling** on the persistent live row to evoke Claude Code's cheeky runtime narration
- **Smooth scroll** preserved from ScrollableBox
**GlintEffect is reusable** and should be treated as a general-purpose inline text component, not a status-bar-only flourish

### Claudex-Specific Configuration

```json
{
  "skin": "claudex",
  "claudex": {
    "show_thinking": true,
    "show_thinking_label": true,
    "tool_results": "collapsed",
    "tool_result_preview_lines": 4,
    "diffs_default": "collapsed",
    "diff_toggle_key": "ctrl+g",
    "show_plan_inline": true,
    "show_todo_inline": true,
    "show_persistent_live_row": true,
    "live_row_phrase_bank": "cheeky",
    "live_row_cycle_seconds": 6,
    "live_row_mode": "persistent",
    "live_row_show_elapsed": true,
    "live_row_show_activity": true,
    "live_row_show_plan_excerpt": true,
    "live_row_show_todo_excerpt": true,
    "live_row_glint": true,
    "live_row_gradient": true,
    "show_token_counts": true,
    "status_show_processes": true,
    "blank_lines_between_messages": false,
    "blank_lines_after_user": false,
    "show_turn_footers": false,
    "dim_metadata": true,
    "glint_tool_icons": true,
    "glint_status_bar": true,
    "spinner_style": "braille"
  }
}
```

---

## Skin 2: FIRMIUS (Enhanced Original)

### Philosophy

Keep the rich, information-dense, elegant dashboard feel that makes Firmius unique. The powerline status bar, the agent strip, the work panel with tabbed plan/todo/context — these are what distinguish Firmius from every other coding CLI. But now, make every element individually configurable so users can dial the experience to their exact preference.

### Layout (Default — Unchanged)

```
╔══════════════════════════════════════════════╗
║ Checking Plan, Repo, And CI Cut Status       ║ ← Title Bar
╠══════════════════════════════════════════════╣
║                                              ║
║  [Chat scroll area]                          ║
║  + 64  awl = avg_word_len(text)              ║
║  + 65  punct = has_sentence_punct_ratio(text) ║
║                                              ║
║  ✓ done · turn 215 · 18s · ↑133.7k ↓799     ║
║                                              ║
║  $ bash -lc 'python -m src.adaptive...'      ║
║    pygame 2.6.1 (SDL 2.28.4)                 ║
║    finished · exit 0 · 2.7s                  ║
║                                              ║
║  ✓ done · turn 217 · 6s · ↑137.1k ↓150      ║
║                                              ║
║  aster · gpt-5.2 · 35m35s                    ║
║                                              ║
╠══════════════════════════════════════════════╣
║ ⊞ PLAN  ☸                    Ctrl+O to cycle ║ ← Work Panel
║ ⊞ Full Windows/macOS migration    ❯ 1        ║
║ ✓ Cut 1: Shared portability gate             ║
║ ✓ Cut 2: LSP portability architecture gate   ║
║ ◉ Cut 5: Cross-platform CI codification      ║
╠══════════════════════════════════════════════╣
║ [Agent Strip rows]                           ║
╠══════════════════════════════════════════════╣
║ > Type a message...                          ║ ← Input Bar
╠══════════════════════════════════════════════╣
║ ⊙ Aster  aster  GPT 5.2 (Low)  ↑137k ↓62k  ║ ← Status Bar
╚══════════════════════════════════════════════╝
```

### What Changes

The layout and default rendering stay exactly as they are. What's new is **granular control** over every visual element:

#### Chat Row Configuration

| Option | Default | Effect |
|--------|---------|--------|
| `show_turn_numbers` | `true` | Include `turn N` in footer |
| `show_turn_footers` | `true` | Entire `✓ done · ...` line |
| `show_turn_timing` | `true` | Duration in footer |
| `show_turn_tokens` | `true` | `↑sent ↓completion` in footer |
| `show_live_footer` | `true` | `aster · model · elapsed` while running |
| `show_blank_lines` | `true` | Blank rows between content parts |
| `blank_lines_after_user` | `true` | Blank row after user message bubble |
| `blank_lines_after_agent` | `true` | Blank row after agent text |
| `blank_lines_after_tools` | `true` | Blank row after tool blocks |
| `show_thinking_blocks` | `true` | Render thinking content |
| `show_user_bubble_bg` | `true` | Background color on user messages |
| `indent_agent_rows` | `true` | 2-char left indent on agent content |
| `show_compaction_markers` | `true` | Compaction separators |

#### Tool Rendering Configuration

| Option | Default | Effect |
|--------|---------|--------|
| `tool_display` | `"full"` | `"full"` = bordered blocks, `"compact"` = Claude-style inline, `"minimal"` = icon + name only |
| `diff_display` | `"expanded"` | `"expanded"` / `"collapsed"` |
| `quick_tools_display` | `"grouped"` | `"grouped"` / `"individual"` / `"hidden"` |
| `process_output_lines` | `4` | Lines of process output shown collapsed |
| `show_tool_borders` | `true` | Bordered boxes around tool blocks |
| `show_tool_headers` | `true` | Colored header bar on tool blocks |
| `show_tool_body` | `true` | Tool body content |
| `show_tool_icons` | `true` | Icons in tool headers |

#### Layout Component Configuration

| Option | Default | Effect |
|--------|---------|--------|
| `show_title_bar` | `true` | Title bar at top |
| `show_agent_strip` | `true` | Multi-agent roster (already exists) |
| `show_work_panel` | `true` | Plan/Todo/Context tabs (already exists) |
| `status_bar_style` | `"powerline"` | `"powerline"` / `"minimal"` / `"hidden"` |
| `show_errors` | `true` | Error display blocks |
| `show_notices` | `true` | Notice display blocks |

#### Animation Configuration

| Option | Default | Effect |
|--------|---------|--------|
| `glint_enabled` | `true` | All glint animations |
| `glint_speed` | `"normal"` | `"slow"` (4s interval) / `"normal"` (2s) / `"fast"` (1s) |
| `glint_status_bar` | `true` | Glint on status bar model name |
| `glint_tool_blocks` | `true` | Glint on live tool block headers |
| `glint_quick_tools` | `true` | Glint on quick tool pills |

### Firmius-Specific Configuration

```json
{
  "skin": "firmius",
  "firmius": {
    "show_title_bar": true,
    "show_turn_numbers": true,
    "show_turn_footers": true,
    "show_turn_timing": true,
    "show_turn_tokens": true,
    "show_live_footer": true,
    "show_blank_lines": true,
    "blank_lines_after_user": true,
    "blank_lines_after_agent": true,
    "blank_lines_after_tools": true,
    "show_thinking_blocks": true,
    "show_user_bubble_bg": true,
    "indent_agent_rows": true,
    "show_compaction_markers": true,
    "tool_display": "full",
    "diff_display": "expanded",
    "quick_tools_display": "grouped",
    "process_output_lines": 4,
    "show_tool_borders": true,
    "show_tool_headers": true,
    "show_tool_body": true,
    "show_tool_icons": true,
    "show_agent_strip": true,
    "show_work_panel": true,
    "status_bar_style": "powerline",
    "show_errors": true,
    "show_notices": true,
    "glint_enabled": true,
    "glint_speed": "normal",
    "glint_status_bar": true,
    "glint_tool_blocks": true,
    "glint_quick_tools": true
  }
}
```

---

## Shared Infrastructure

### SkinConfig Architecture

```
UserPreferences (preferences.json)
  └─ "skin": "claudex" | "firmius"
  └─ "claudex": { ...claudex options... }
  └─ "firmius": { ...firmius options... }

SkinConfig struct
  └─ SkinKind enum { Firmius, Claudex }
  └─ All option fields with defaults per skin
  └─ Resolved at load time from preferences

SkinManager singleton
  └─ loadFromPreferences()
  └─ current() → const SkinConfig&
  └─ switchSkin(SkinKind)
  └─ Persists to preferences.json
```

### Files to Create

| File | Purpose |
|------|---------|
| `packages/tui/include/SkinConfig.hpp` | SkinKind enum, SkinConfig struct with all options |
| `packages/tui/include/SkinManager.hpp` | Singleton managing active skin |
| `packages/tui/src/SkinManager.cpp` | Load/save/switch logic |
| `packages/tui/include/commands/SkinCommand.hpp` | `/skin` slash command |
| `packages/tui/src/commands/SkinCommand.cpp` | Implementation |

### Files to Modify

| File | Changes |
|------|---------|
| `packages/tui/include/UserPreferences.hpp` | Add `skin` and per-skin config fields |
| `packages/tui/src/UserPreferences.cpp` | Serialize/deserialize skin config |
| `packages/tui/src/TUIState.cpp` | Layout composition conditioned on skin |
| `packages/tui/src/components/ChatWindow.cpp` | Turn footer, blank lines, tool rendering mode |
| `packages/tui/src/components/StatusBar.cpp` | Powerline vs minimal rendering |
| `packages/tui/src/components/ToolBlock.cpp` | Bordered vs inline rendering |
| `packages/tui/src/components/ToolPresentationBlock.cpp` | Bordered vs inline rendering |
| `packages/tui/include/components/StatusBar.hpp` | Minimal status bar model additions |
| `packages/tui/src/components/InputBar.cpp` | Prompt style (boxed vs raw) |

### Rendering Decision Points

Every rendering function that changes behavior checks `SkinManager::instance().current()` and branches on the active skin's options. The pattern is:

```cpp
const auto& skin = SkinManager::instance().current();

if (skin.show_blank_lines_after_agent) {
    rows_.push_back(ftxui::Make<RowComponent>(
        nullptr, [] { return ftxui::text(""); }));
}
```

This is lightweight — a single struct lookup per decision point. No virtual dispatch, no runtime polymorphism overhead.

### Claudex Tool Rendering — Implementation Notes

The Claudex inline tool style is a **new render path** in ToolBlock/ToolPresentationBlock that produces compact rows instead of bordered blocks:

```cpp
if (skin.kind == SkinKind::Claudex || skin.tool_display == "compact") {
    return RenderCompactToolRow(view, theme, skin);
} else {
    return RenderBorderedToolBlock(view, theme, skin);
}
```

The compact renderer produces:
1. A header line: `⊙ ToolName(target)` with colored icon
2. An optional result line: `  └ summary` with tree connector
3. Optional collapsed body: `  │ line1` / `  │ line2` / `  ... +N lines`

This reuses existing `ToolPresentation` data — just renders it differently.

### Claudex Inline Plan/Todo — Implementation Notes

When `show_plan_inline` or `show_todo_inline` is true in Claudex:
- `TUIState::onEvent` watches for plan/todo update events
- Instead of updating a side panel model, it injects a **synthetic chat row** into the live rows provider
- The row renders using box-drawing characters (`╭─`, `│`, `╰─`) with plan/todo items inside
- This row is ephemeral — it appears in the live stream when the state changes, but persisted turns store the actual plan/todo state

### Status Bar Modes

**Powerline** (Firmius default):
Current StatusBar.cpp implementation — no changes needed.

**Minimal** (Claudex default, optional for Firmius):
New render branch in StatusBarComponentBase::OnRender():
```
aster · opus-4 · ↑45.2k ↓3.1k · 34% · ASK
```
- No powerline separator characters
- No background color segments
- Just colored text separated by ` · `
- Glint on model name preserved

**Hidden**: Status bar returns an empty element.

---

## Switching UX

### `/skin` Command

```
/skin              → Show current skin and available skins
/skin claudex      → Switch to Claudex
/skin firmius      → Switch to Firmius
```

Switching triggers:
1. Update `SkinManager` state
2. Persist to preferences
3. Post `ThemeChanged` event to force full re-render
4. Notification toast: "Switched to Claudex skin"

### Config Override via `/config`

The existing `/config` modal can show skin-specific options in a new section. Users can toggle individual options without switching the entire skin.

---

## ASCII Art Comparison

### Claudex at 80 columns, idle

```
  I've analyzed the engine architecture. The singleton pattern
  manages agent lifecycles through dedicated jthreads, with
  each agent getting its own execution context.

  ⊕ read packages/core/src/Engine.cpp, Agent.cpp
  ⊕ searched "registerProvider" in packages/provider/src

  The provider registry uses a factory pattern with lazy
  instantiation. Both registration and creation happen under
  mutex protection.

  ✦ Reading just enough to avoid doing something stupid.   aster · opus-4 · edit · 1m08s

  ⊙ Edit(packages/core/src/Engine.cpp)
    └ 2 hunks · +8 -3 · diff hidden

  ⊘ Bash cmake --build build --target core
    └ exit 0 · 4.2s

> ▊
aster · opus-4 · ↑89.2k ↓2.4k · 42% · ASK
```
▊
### Firmius at 120 columns, same content

```
┌─ Checking Plan, Repo, And CI Cut Status ────────────────────────────────────────────┐
│                                                                                      │
│    I've analyzed the engine architecture. The singleton pattern manages agent         │
│    lifecycles through dedicated jthreads, with each agent getting its own             │
│    execution context.                                                                │
│                                                                                      │
│    ⊕ read packages/core/src/Engine.cpp, Agent.cpp                                   │
│    ⊕ searched "registerProvider" in packages/provider/src                            │
│                                                                                      │
│  ┌─ ⊙ Edit ─ packages/core/src/Engine.cpp ──────────────────────────────────────┐   │
│  │  @@ -14,3 +14,4 @@                                                          │   │
│  │  + #include <optional>                                                        │   │
│  │  @@ -48,3 +49,6 @@                                                          │   │
│  │  -   int retries = 0;                                                        │   │
│  │  +   int retries = 1;                                                        │   │
│  │  +   logRetryBudget(retries);                                                │   │
│  │  Diagnostics · Engine.cpp                                                     │   │
│  │  2 hunks · +8 -3                                                             │   │
│  └──────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                      │
│  ┌─ ⊘ Process ─ cmake --build build --target core ─────────────────────────────┐   │
│  │  $ cmake --build build --target core                                         │   │
│  │  [100%] Built target firmius_core                                             │   │
│  │  finished · exit 0 · 4.2s                                                    │   │
│  └──────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                      │
│    ✓ done · turn 215 · 23s · ↑89.2k/92k ↓2.4k                                     │
│                                                                                      │
│    aster · gpt-5.2 · 35m35s                                                        │
│                                                                                      │
├──────────────────────────────────────────────────────────────────────────────────────┤
│ ⊞ PLAN  ☸  ◇ TODO                                                  Ctrl+O to cycle │
│ ⊞ Full Windows/macOS migration with zero-caveat verification            ❯ 1         │
│ ✓ Cut 1: Shared portability gate                                                    │
│ ✓ Cut 2: LSP portability architecture gate                                          │
│ ◉ Cut 5: Cross-platform CI codification                                             │
├──────────────────────────────────────────────────────────────────────────────────────┤
│ > Type a message...                                                                  │
├──────────────────────────────────────────────────────────────────────────────────────┤
│ ⊙ Aster  aster  GPT 5.2 (Low)  ↑137.1k/11.3k ↓62.5k     AUTO ◀ 58%               │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

---

## Implementation Phases

### Phase 1: Infrastructure
1. Create `SkinConfig.hpp` and `SkinManager.hpp`/`.cpp`
2. Extend `UserPreferences` to load/save skin choice and per-skin options
3. Create `/skin` command

### Phase 2: Layout Branching
1. `TUIState::root()` — conditional layout composition based on skin
2. Title bar, agent strip, work panel visibility governed by skin
3. Separator visibility governed by skin

### Phase 3: Chat Rendering
1. `ChatWindow.cpp` — blank line control, turn footer control
2. Turn number visibility
3. Live footer styling
4. Inline plan/todo rendering for Claudex

### Phase 4: Tool Rendering
1. New compact tool renderer for Claudex inline style
2. `ToolBlock.cpp` / `ToolPresentationBlock.cpp` — skin-aware branching
3. Collapsible tool results in compact mode
4. Spinner characters for live processes

### Phase 5: Status Bar
1. Minimal status bar renderer
2. Status bar mode selection based on skin
3. Glint preservation in minimal mode

### Phase 6: Input & Polish
1. Input bar styling (boxed vs raw prompt)
2. Permission request rendering per skin
6. **The magic is in the details.** Claudex's magic comes from: the persistent live row, glint-treated cheeky runtime phrases, glint sweep on live tool icons, braille spinner on running processes, collapsible-but-available diffs, and the absolute removal of visual noise. Firmius's magic comes from: the powerline, the agent strip, the orchestration dashboard, and the sheer density of useful information.

7. **The persistent live row is the Claudex anchor.** If Claudex loses that row, it loses the skin's reason to exist.
4. Testing and refinement

---

## Key Design Decisions

1. **Skins are NOT themes.** A skin controls structure and behavior. A theme controls colors. They compose independently. You can run Claudex skin with any color theme.

2. **Claudex doesn't lose information.** Everything Firmius shows is still _available_ — it's just rendered differently (inline plan instead of panel) or hidden behind a config toggle (turn numbers). No runtime data is suppressed at the source.

3. **Firmius default doesn't change.** Existing users see zero difference. Every new option defaults to the current behavior. The enhanced configurability is opt-in.

4. **Per-skin config isolation.** Claudex options don't affect Firmius and vice versa. When you switch skins, you switch to that skin's full config. This prevents cross-contamination of preferences.

5. **Skin switching is instant.** No restart required. Post a `ThemeChanged` event (reuse existing path) and the entire UI re-renders from the new skin config in one frame.

6. **The magic is in the details.** Claudex's magic comes from: glint on the status bar model name, glint sweep on live tool icons, braille spinner on running processes, and the absolute removal of visual noise. Firmius's magic comes from: the powerline, the agent strip, the orchestration dashboard, and the sheer density of useful information.
