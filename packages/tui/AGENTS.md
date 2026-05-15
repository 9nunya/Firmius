# TUI PACKAGE

**Purpose**: Terminal user interface built on FTXUI library.

---

## STRUCTURE

```
packages/tui/
├── include/
│   ├── TUIState.hpp            # Central UI state singleton
│   ├── components/             # UI components (ChatWindow, InputBar, etc.)
│   ├── modals/                 # Popup modals (IModal interface)
│   ├── commands/               # Slash commands (ICommand interface)
│   ├── tools/                  # Tool presentation components
│   └── utils/                  # UI utilities
└── src/
    └── main.cpp                # Entry point
```

---

## KEY COMPONENTS

### TuiState (Central State)
- **File**: `include/TUIState.hpp`
- **Pattern**: Singleton
- **Responsibilities**:
  - Hold view models (TitleBarModel, InputBarModel, etc.)
  - Manage modal stack
  - Bridge with `firmius::core::Harness`
  - Process `AppEvent` queue from core threads

### Components (FTXUI-based)
- **Base**: Extend `ftxui::ComponentBase`
- **Key Components**:
  - `ChatWindow`: Message display with markdown
  - `InputBar`: User input with autocomplete
  - `TodoLane`: Task visualization
  - `ToolBlock`: Tool execution display
  - `Markdown`: Syntax highlighted rendering
  - `DiffRenderer`: File diff visualization

### Modals
- **Interface**: `include/modals/IModal.hpp`
- **Registry**: `ModalRegistry` (singleton)
- **Examples**: `ThreadPickerModal`, `ModelPickerModal`, `ConfigDisplayModal`

### Commands (Slash Commands)
- **Interface**: `include/commands/ICommand.hpp`
- **Registry**: `CommandManager` (singleton)
- **Examples**: `/threads`, `/config`, `/compact`, `/workflows`

---

## EVENT SYSTEM

The TUI integrates with core via thread-safe event queue:

```
Core Threads → AppEvent → EventQueue → TuiState::drain() → UI Update
```

- `StreamStateManager` tracks incremental LLM response state
- Real-time rendering of partial markdown and diffs

---

## RESPONSIVE LAYOUT

`WorkPanelLayout.cpp` implements decision engine:
- Toggle `PLAN`/`TODO` panels based on terminal width
- User preference overrides

---

## ANTI-PATTERNS

| Pattern | Why Forbidden | Alternative |
|---------|---------------|-------------|
| Direct core calls from UI thread | Thread safety | Use `EventQueue` |
| Blocking UI during tool execution | Responsiveness | Async event handling |
| Raw FTXUI element manipulation | State consistency | Go through `TuiState` |
