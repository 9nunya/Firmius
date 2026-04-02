# FIRMIUS KNOWLEDGE BASE

**Project**: Firmius — AI Agent Orchestration System  
**Stack**: C++20, CMake, FTXUI, RapidJSON, GoogleTest  
**Structure**: Monorepo (5 packages, ~366 source files, ~68k LOC)

---

## OVERVIEW

Firmius is a terminal-based AI agent orchestration platform. It manages fleets of LLM-powered agents that can execute tools, spawn subagents, and interact with local or Docker-based execution environments. The architecture separates concerns into interface definitions (`shared`), orchestration logic (`core`), LLM provider implementations (`provider`), terminal UI (`tui`), and evaluation harnesses (`audits`).

---

## STRUCTURE

```
.
├── packages/
│   ├── shared/          # Interfaces (IAgent, ITool, IProvider), events, utilities
│   ├── core/            # Engine, Harness, Agents, Tools, Hosts, Persistence
│   ├── provider/        # LLM provider implementations (15+ providers)
│   ├── tui/             # Terminal UI (FTXUI-based components, modals, commands)
│   └── audits/          # Benchmarking and evaluation harnesses
├── tests/               # Unit tests, integration tests, mocks
├── prompts/             # Agent persona definitions
├── workflows/           # Workflow command definitions
└── cmake/               # Build configuration
```

---

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| **Add new tool** | `packages/core/include/tools/` | Inherit from `TypedTool<T>` |
| **Add LLM provider** | `packages/provider/include/providers/` | Inherit from `BaseOpenAIProvider` or `IProvider` |
| **UI component** | `packages/tui/include/components/` | Extend `ftxui::ComponentBase` |
| **UI modal** | `packages/tui/include/modals/` | Implement `IModal` interface |
| **Slash command** | `packages/tui/include/commands/` | Implement `ICommand` interface |
| **Agent interface** | `packages/shared/include/IAgent.hpp` | Core agent lifecycle contract |
| **Tool interface** | `packages/shared/include/ITool.hpp` | Tool metadata + execution contract |
| **Provider interface** | `packages/shared/include/IProvider.hpp` | LLM streaming contract |
| **Event system** | `packages/shared/include/Events.hpp` | AppEvent, StreamEvent definitions |
| **Engine** | `packages/core/include/Engine.hpp` | Fleet commander singleton |
| **Harness** | `packages/core/include/harness/Harness.hpp` | Session/thread management |
| **Tool registry** | `packages/core/include/tools/ToolRegistry.hpp` | Tool discovery |
| **Provider registry** | `packages/provider/include/providers/ProviderRegistry.hpp` | Provider discovery |

---

## CODE MAP

### Key Singletons

| Singleton | Package | Purpose |
|-----------|---------|---------|
| `Engine` | core | Manages agent lifecycles, task execution |
| `Harness` | core | Session management, thread switching |
| `AgentRegistry` | core | Agent instance tracking |
| `ToolRegistry` | core | Tool discovery and execution |
| `ProviderRegistry` | provider | LLM provider management |
| `TuiState` | tui | UI state management |
| `CommandManager` | tui | Slash command routing |
| `ModalRegistry` | tui | Modal stack management |

### Package Dependencies

```
tui ─────┐
audits ──┼──► core ──► shared ◄── provider
         │           ▲
         └───────────┘
```

---

## CONVENTIONS

### Naming
- **Files**: `PascalCase.hpp` / `PascalCase.cpp`
- **Classes/Structs**: `PascalCase`
- **Methods/Functions**: `camelCase`
- **Variables**: `camelCase`
- **Interfaces**: `I` prefix (e.g., `IAgent`, `ITool`, `IProvider`)

### Namespaces
- Root: `firmius`
- Packages: `firmius::core`, `firmius::shared`, `firmius::provider`, `firmius::tui`, `firmius::audits`
- Implementation detail: anonymous namespaces in `.cpp` files

### Headers
- Use `#ifndef` guards: `FIRMIUS_[PACKAGE]_[FILE]_HPP`
- Include order: system headers → 3rd party → project headers

### Error Handling
- **Fatal errors**: `FIRMIUS_PANIC(msg)` → prints backtrace + extra debug info
- **Recoverable errors**: Standard exceptions (`throw`/`catch`)

### Smart Pointers
- Use `std::shared_ptr` for shared ownership (agents, providers)
- Use `std::unique_ptr` for exclusive ownership

---

## ANTI-PATTERNS (THIS PROJECT)

| Pattern | Why Forbidden | Alternative |
|---------|---------------|-------------|
| Raw `new`/`delete` | Memory safety | Smart pointers only |
| Vulnerable commands (`rm -rf /`, `format`) | Security | Blocked by `CommandIntentAnalyzer` |
| LineRange prefixes in tool outputs | Parsing errors | Strip before returning |
| Silent failure | Debug difficulty | Use `FIRMIUS_PANIC` or explicit error handling |
| Global mutable state | Thread safety | Use singletons with mutex protection |

---

## COMMANDS

```bash
# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Test
ctest --output-on-failure

# Run
./build/packages/tui/firmius          # Start TUI
./build/packages/tui/firmius -c       # Continue last session
./build/packages/audits/firmius_audit # Run audits

# Install
sudo make install  # Installs to /usr/local/bin/firmius
```

---

## NOTES

- **Threading**: Agents run in dedicated `std::jthread`s managed by `Engine`
- **Persistence**: Thread history stored in `~/.firmius/threads/`
- **Configuration**: User config at `~/.firmius/config.json`
- **Workflows**: Custom slash commands loaded from `~/.firmius/workflows/`
- **Personas**: Agent personalities from `prompts/` directory
- **Locking**: PID-based file locking via `ThreadLockManager` prevents concurrent thread access
