<p align="center">
  <img src="src/web/public/logo-both-text-white.webp" alt="Firmius Logo" width="500"/>
</p>

# Firmius

**Firmius** is an enterprise-grade multi-agent system for automated software engineering. It orchestrates a team of specialized LLM-driven agents—Orchestrator, Mapper, Architect, Executor, Coder, and Verifier—to plan, implement, verify, and merge code changes through a disciplined, auditable workflow.

Built for scale, security, and extensibility, Firmius combines event-driven runtime, hierarchical delegation, and persistent thread state to deliver reliable, production-quality results.

---

## Core Architecture

### Engine
The singleton runtime that manages the entire system. Responsible for:
- Loading and registering all **Tools** and **Providers**
- Maintaining global `threads` registry
- Emitting typed events (`agent_spawned`, `agent_thinking`, `tool_call_start/end`, `agent_metrics`, etc.)
- Providing LSP utilities per host and project root

### Thread
A session container that holds a collection of agents. Two implementations:
- **PersistentThread**: Disk-backed with journaling, checkpointing, and branching. Stores state in `~/.firmius/threads/<id>/`.
- **InMemoryThread**: Ephemeral threads for testing or temporary sessions.

Each thread owns:
- `AgentRegistry`: thread-scoped map of agents and their hierarchies
- `ThreadManager`: lifecycle control (interrupt, dispose)
- `ThreadJournal`: append-only log of turns and messages
- `ThreadPersistence`: checkpoint serialization

### Agent
An autonomous LLM-driven worker with a defined **purpose** (role), permissions (tool scopes, file access limits), and context. Agents execute in a ReAct loop: stream LLM output, call tools concurrently, and repeat until a final response.

Key capabilities:
- **File watching**: auto-refreshes attached files modified externally (within context budget)
- **Context management**: enforces token limits, auto-summarizes old turns, blocks delegation when capacity critical
- **Interruption**: graceful cancellation with partial state capture
- **Event streaming**: real-time reporting to Engine

### PurposeRegistry
Loads agent definitions from markdown files (YAML frontmatter + system prompt). Defines:
- `name`, `title`, `description`
- `scopes`: allowed tool categories
- `canSpawn`: which child purposes this agent may create
- `systemPrompt`: the agent’s identity and operating instructions

Default purposes live in `src/core/defaults/purposes/`; users can override or add new ones in `~/.firmius/purposes/`.

### Tool
A validated function that agents can invoke. Each tool declares:
- `metadata`: name, description, `scope` (e.g., `fs:read`, `proc`, `lsp`, `git`)
- `input`: Zod schema for argument validation
- `output`: return type
- `execute(params, { host, agent })`: implementation

Tools are grouped by domain: FileTools, ProcessTools, LSPTools, GitTools, WebTools, TodoTools, CodeTools, etc.

### Provider
Adapter for LLM backends. Supports streaming with events:
- `reasoning` (thinking tokens)
- `content` (final message)
- `tool_call` (function call request)
- `usage` (token count)

Built-in providers: ZAI (default), LMStudio, Zen, NanoGPT.

### Host
Execution environment abstraction:
- **LocalHost**: direct Bun/Node APIs
- **DockerHost**: run inside a Docker container (`firmius-sandbox`)
- **RemoteSSHHost**: execute over SSH

---

## Agent Architecture

Firmius employs a hierarchy of specialized agents, each with strict constraints and clear responsibilities.

### 1. Orchestrator
The user-facing lead agent. Never writes code. Responsibilities:
- Understand requirements and maintain `.firmius/` project directory
- Spawn Mapper, Architect, and Executor agents
- Present Architect's Plan for user approval before execution
- Commit changes directly to the main branch
- Enforce workflow discipline and transparency

**Can spawn:** `architect`, `executor`, `mapper`

### 2. Mapper
Context analysis agent. Produces documentation for other agents to consume:
- `STACK.md`: technology stack, dependencies, build tools
- `ARCHITECTURE.md`: directory map, entry points, data flow, patterns
- `CODING_STYLE.md`: naming conventions, error handling, testing patterns

**Can spawn:** none (leaf)

### 3. Architect
Read‑only planning agent. Explores the codebase and writes a structured Plan in XML-in-Markdown format. The Plan decomposes work into **waves** (parallelizable task groups) and **tasks** (imperative instructions with file declarations and verification criteria).

**Output:** `.firmius/phases/<phase-id>/plan.md`

**Can spawn:** none (leaf)

### 4. Executor
Sub‑lead that operates in the project root directory. Manages implementation of a Plan:
- Decompose Plan into Coder tasks
- Spawn Coders with precise file scope restrictions
- Aggregate results and spawn Verifier
- Retry failed tasks (max 3 attempts)
- Stage changes and report summary (Orchestrator commits)

**Can spawn:** `coder`, `verifier`

### 5. Coder
Leaf implementation agent. Receives a task specification (goal, files read/write, instructions, verification criteria). Must:
- Implement exactly what is specified
- Respect file scope (`allowPaths`)
- Run validation (tests, type checker, linter) before reporting
- Never make architectural decisions or add out‑of‑scope features

**Can spawn:** none (leaf)

### 6. Verifier
Quality auditor. Validates completed work against the Plan:
- Run automated checks (types, lint, tests, build)
- Review implementation for correctness, completeness, convention compliance
- Issue PASS or FAIL with specific, actionable feedback

**Can spawn:** none (leaf)

---

## Collaborative Workflow

1. **Discovery** – Orchestrator checks for existing context; if missing or stale, spawns Mapper.
2. **Planning** – Orchestrator breaks the request into Phases, spawns an Architect for each. Architect returns a Plan with Waves and Tasks. Orchestrator presents the Plan to the user for approval.
3. **Execution** – For each approved Phase, Orchestrator spawns an Executor in the project root. Executor processes waves sequentially, spawning Coders in parallel for independent tasks. Within each wave, Coders work in isolation (no overlapping writes).
4. **Verification** – After all waves complete, Executor spawns a Verifier. If verification fails, Executor retries failed tasks with feedback (up to 3 cycles). If still failing, Executor reports failure to Orchestrator.
5. **Commit** – Upon successful verification, Executor stages changes. Orchestrator commits to the main branch, spawns Merger agent if conflicts occur, and reports completion.
6. **Wrap‑up** – Orchestrator delivers final summary (what was done, files changed, follow‑up recommendations).

All decisions, plans, and status are recorded under `.firmius/` for auditability.

---

## Extensibility

Firmius is designed to be extended without modifying core code.

### Custom Purposes
Create a markdown file in `~/.firmius/purposes/`:

```markdown
---
name: security-auditor
title: Security Auditor
description: Reviews code for vulnerabilities.
scopes: ["fs:read", "proc", "web"]
canSpawn: []
---

# System Identity: Security Auditor
You are a security specialist...
```

Reload takes effect immediately.

### Custom Tools
Implement the `ITool` interface (input Zod schema, execute function) and register in `Engine.loadTools()`.

### Custom Hosts
Implement the `IHost` interface (file I/O, process spawn, init/destroy) and add to `HostFactory`.

### Custom Providers
Implement `IProvider` with `listModels()` and `stream()` and register in `Engine.loadProviders()`.

---

## Quick Start

```bash
# Install dependencies
bun install

# Run a thread (CLI demo)
bun run src/index.ts

# Start the TUI interface
bun run src/tui/index.tsx

# Start the Web UI (development)
bun run dev:frontend

# Run the backend API server
bun run dev:backend
```

Environment variables:
- `ZAI_API_KEY` (required for default provider)
- `LMSTUDIO_API_KEY` (optional)

---

## Technical Stack

- **Runtime**: Bun (with Node.js compatibility)
- **Language**: TypeScript (strict mode)
- **LLM Integration**: streaming providers with tool calling
- **Data validation**: Zod
- **LSP**: semantic code analysis via language servers
- **Frontend**: Next.js 15, React 19, Tailwind CSS, OpenTUI
- **Event system**: Node.js EventEmitter
- **Persistence**: JSON checkpoint + append-only journal
- **Git**: native commands for branching and merging

---

## Project Structure

```
firmius/
├── src/
│   ├── core/
│   │   ├── Engine.ts           # singleton runtime
│   │   ├── Agent.ts            # agent lifecycle & turn execution
│   │   ├── AgentFactory.ts     # agent creation & delegation rules
│   │   ├── ContextBuilder.ts   # prompt construction
│   │   ├── tools/              # all tool implementations
│   │   ├── hosts/              # Local, Docker, SSH hosts
│   │   ├── registry/           # PurposeRegistry, AgentRegistry
│   │   ├── lsp/                # LSP client utility
│   │   ├── defaults/purposes/  # built‑in agent definitions
│   │   └── Constants.ts
│   ├── threads/
│   │   ├── PersistentThread.ts # disk‑backed thread
│   │   ├── InMemoryThread.ts   # in‑memory thread
│   │   ├── ThreadManager.ts    # lifecycle manager
│   │   ├── ThreadJournal.ts    # append‑only log
│   │   └── ThreadPersistence.ts# checkpointing
│   ├── types/                  # TypeScript interfaces
│   ├── providers/              # LLM provider adapters
│   ├── api/                    # REST + SSE server
│   ├── web/                    # Next.js frontend
│   ├── tui/                    # terminal UI
│   └── index.ts                # CLI entry point
├── docker/
│   └── Dockerfile.sandbox      # sandbox image for isolated execution
├── .firmius/                   # user‑generated (threads, purposes, context)
├── package.json
├── tsconfig.json
└── README.md
```

---

## License

Private.

---

**Firmius** – Orchestrated intelligence. Executed with discipline.
