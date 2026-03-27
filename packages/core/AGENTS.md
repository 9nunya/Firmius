# CORE PACKAGE

**Purpose**: Orchestration engine managing agent lifecycles, tool execution, and session state.

---

## STRUCTURE

```
packages/core/
├── include/
│   ├── Engine.hpp              # Fleet commander singleton
│   ├── harness/Harness.hpp     # Session/thread management
│   ├── agents/                 # Agent implementations
│   ├── tools/                  # 30+ tool implementations
│   ├── environment/            # Execution context (Host, Workspace, ProcessManager)
│   ├── persistence/            # Journaling, ThreadManager, HistoryEditor
│   ├── hosts/                  # LocalHost, DockerHost
│   ├── benchmarks/             # MBPP, SWE-bench, AgentBench
│   └── workflow/               # Workflow system
└── src/                        # Corresponding implementations
```

---

## KEY COMPONENTS

### Engine (The Fleet Commander)
- **File**: `include/Engine.hpp`
- **Pattern**: Singleton
- **Responsibilities**: 
  - Summon/resume agents in dedicated `std::jthread`s
  - Manage `ToolRegistry` (owned by Engine, outlives agents)
  - Broadcast events to listeners
  - Handle agent lifecycle (cancel, terminate, compact)

### Harness (Session Layer)
- **File**: `include/harness/Harness.hpp`
- **Pattern**: Singleton
- **Responsibilities**:
  - Manage threads (logical work units)
  - Handle session focus switching
  - PID-based file locking via `ThreadLockManager`
  - Restore last sessions on startup

### Agent
- **File**: `include/agents/Agent.hpp`
- **Pattern**: Implements `IAgent` interface
- **Responsibilities**:
  - Run LLM loop (prompt → tool call → execute)
  - Compose `Environment`, `Permissions`, `Journaler`
  - Handle interruption and compaction

### Tools (30+ implementations)
- **Registry**: `include/tools/ToolRegistry.hpp`
- **Base**: Inherit from `TypedTool<T>` (from `shared`)
- **Categories**:
  - File: `FileReadTool`, `FileEditTool`, `GlobTool`, `GrepTool`
  - Process: `ProcessExecuteTool`, `ProcessSpawnTool`, `ProcessWaitTool`
  - Subagent: `SubagentTool`, `SubagentWaitTool`, `SubagentTerminateTool`
  - Plan/Chunk: `PlanCreateTool`, `ChunkAddTool`, `TodoWriteTool`
  - Web: `WebFetchTool`
  - Python: `PythonExecuteTool`
  - Artifact: `ArtifactWriteTool`, `ArtifactReadTool`, `ArtifactListTool`

### Environment
- **File**: `include/environment/Environment.hpp`
- **Pattern**: Composition of `ProcessManager`, `Workspace`, targets `IHost`
- **Purpose**: Decouple agent logic from execution location (local vs Docker)

---

## ANTI-PATTERNS

| Pattern | Why Forbidden | Alternative |
|---------|---------------|-------------|
| Raw pointers for agents/tools | Memory safety | `std::shared_ptr<IAgent>` |
| Direct host access from tools | Security | Go through `ToolContext::host` |
| Modifying `chunk_*` authority fields | Runtime security | Authority checks in `SubagentTool` |
| Allowing vulnerable commands | Safety | `CommandIntentAnalyzer` blocks `rm -rf /`, `format` |

---

## CONVENTIONS

- Agents run in `std::jthread` (C++20)
- Tools use `ToolContext` for cancellation signals
- History persisted via `Journaler` (append-only)
- Thread metadata managed by `ThreadManager`
- Error handling: `FIRMIUS_PANIC` for fatal, exceptions for recoverable
