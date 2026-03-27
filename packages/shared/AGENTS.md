# SHARED PACKAGE

**Purpose**: Interface definitions, data models, and shared utilities.

---

## STRUCTURE

```
packages/shared/
├── include/
│   ├── IAgent.hpp              # Agent interface
│   ├── ITool.hpp               # Tool interface + TypedTool
│   ├── IProvider.hpp           # LLM provider interface
│   ├── IHost.hpp               # Execution host interface
│   ├── IEnvironment.hpp        # Environment interface
│   ├── Events.hpp              # AppEvent, StreamEvent
│   ├── Context.hpp             # AgentContext, Message, Turn
│   └── utils/                  # Utilities (JSON, strings, logging)
└── src/utils/                  # Implementations
```

---

## KEY INTERFACES

### IAgent
- **File**: `include/IAgent.hpp`
- **Purpose**: Core agent lifecycle contract
- **Key Methods**:
  - `run(task, onEvent, images)` → Execute task
  - `resume(onEvent)` → Continue from history
  - `interrupt()` / `isInterrupted()` / `clearInterrupt()`
  - `compactNow(onEvent)` → Context compaction
  - `setModel(providerId, modelId, variant)`
  - `getEnvironment()` / `getPermissions()` / `getHost()`

### ITool / TypedTool
- **File**: `include/ITool.hpp`
- **Purpose**: Tool metadata + execution contract
- **Key Types**:
  - `ToolMetadata`: name, description, scope
  - `ToolResult`: success/error/data
  - `ToolContext`: host, agent, cancelSignal
- **TypedTool<T>**: Base for strongly-typed tools with JSON schema

### IProvider
- **File**: `include/IProvider.hpp`
- **Purpose**: LLM streaming contract
- **Key Methods**:
  - `stream(context, onEvent)` → Stream LLM response
  - `listModels()` → Available models
  - `getModelInfo(modelId)` → Model metadata

### IHost / IHostProcess
- **File**: `include/IHost.hpp`, `include/IHostProcess.hpp`
- **Purpose**: Execution abstraction (Local vs Docker)
- **Implementations**: `LocalHost`, `DockerHost` (in core/)

### IEnvironment
- **File**: `include/IEnvironment.hpp`
- **Purpose**: Execution context composition
- **Composes**: `ProcessManager`, `Workspace`

---

## EVENT SYSTEM

### AppEvent
- **File**: `include/Events.hpp`
- **Purpose**: Core → UI communication
- **Types**: StreamEvent, ToolEvent, AgentEvent, etc.

### StreamEvent
- LLM token chunks
- Tool call start/complete
- Error events

---

## UTILITIES

| Utility | Purpose |
|---------|---------|
| `JSONSchema.hpp` | JSON validation |
| `StringUtil.hpp` | String manipulation |
| `Hashline.hpp` | Output cleaning (strip hashline prefixes) |
| `Panic.hpp` | Fatal error handling (`FIRMIUS_PANIC`) |
| `Logger.hpp` | Logging infrastructure |
| `FrontmatterParser.hpp` | Markdown frontmatter parsing |

---

## CONVENTIONS

- All interfaces use `I` prefix
- All interfaces have virtual destructors
- Data structs are plain C++ structs
- Events use variant types for polymorphism
- Utilities are stateless functions
