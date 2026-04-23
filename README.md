# Firmius

**Firmius is the terminal-native control room for serious agent orchestration.**

Most agent apps give you one assistant with a nicer skin. Firmius gives you a real operating surface: multiple agents, explicit roles, runtime memory, MCP, semantic code intel, durable threads, artifacts, workflows, benchmarks, and enough control to run actual work instead of roleplaying it.

If you want a polished toy, this is not that.
If you want a fast, opinionated, C++-native system that can coordinate real agent work without pretending the hard parts do not exist, you're in the right repo.

## Why Firmius is better

Because orchestration is the product here — not a checkbox.

- **Real multi-agent execution** with subagents, handoffs, plans, chunking, and worker coordination.
- **Terminal-first by design** with no split focus on a web package or browser wrapper.
- **MCP that actually works** across discovery, loading, dynamic tool exposure, prompts, resources, and tool calls.
- **LSP-backed code intelligence** so agents can navigate code semantically instead of doing grep cosplay.
- **Rolling memory with structure** instead of blunt truncation and wishful thinking.
- **Thread-scoped artifacts and exact recall** so long-running work stays inspectable and recoverable.
- **Workflow markdown that becomes slash commands** because extensibility should not require a recompile.
- **Provider and routing control** so different roles can use different models for different jobs.
- **Docker-backed audit and benchmark lanes** when you want proof, not vibes.

## What you get

### Orchestration that feels like a system, not a demo

Firmius ships with the primitives that matter when agents stop being a novelty:

- lead / planner / executor / scout / worker purpose lanes
- subagent spawning, waiting, and termination
- plan and chunk lifecycle tools
- file-aware coordination and locking
- thread-scoped artifacts like `@artifact:lead/REPORT.md`
- undo, resume, and durable thread state

### Memory that holds up under real sessions

Firmius keeps more than a shrinking transcript:

- rolling memory modes and occupancy presets
- observer and reflector passes
- a dedicated working-memory updater lane
- watched-file overlays for read / fully-read / edited state
- durable user and project memory under `~/.firmius/user/`
- exact turn recall when summaries are not enough

### Tooling that can actually move work forward

Out of the box, Firmius agents can work with:

- files, directories, grep, glob, and patch-style edits
- process execution and interactive subprocess control
Python execution, including optional project virtualenv selection
- MCP servers over `stdio` and `http`
- LSP hover, definition, references, symbols, and diagnostics
- web fetch and search tools inside the agent runtime
- artifact, planning, todo, and memory tooling

## Quick start

```bash
cmake -S . -B build -G Ninja
cmake --build build --parallel
```

Launch the TUI:

```bash
./build/packages/cli/firmius     # macOS/Linux
./build/packages/cli/firmius.exe # Windows
```

Continue your last session:

```bash
./build/packages/cli/firmius -c     # macOS/Linux
./build/packages/cli/firmius.exe -c # Windows
```

Run a one-shot prompt in a fresh thread:

```bash
./build/packages/cli/firmius --prompt "audit this repo" --quit-when-idle
./build/packages/cli/firmius.exe --prompt "audit this repo" --quit-when-idle
```

See available audit surfaces:

```bash
./build/packages/audits/firmius_audit --list
./build/packages/audits/firmius_audit.exe --list
```

Cross-platform CI in `.github/workflows/cross-platform-ci.yml` builds the shipped binaries and runs the migrated proof targets `test_json_rpc_transport`, `test_lsp_server_manager`, `test_engine_shutdown`, and `test_lsp_audit` on Linux, macOS, and Windows.

## Core commands

| Command | What it does |
| --- | --- |
| `/new` | Start a fresh thread |
| `/threads` | Switch threads |
| `/models` | Change the focused model |
| `/connect` | Attach a provider account |
| `/accounts` / `/quotas` | Inspect provider state |
| `/memory` | Tune rolling-memory behavior |
| `/router` | Define routing categories |
| `/purposes` | Map roles to routes |
| `/mcp` | Manage MCP servers and connections |
| `/benchmarks` | Launch benchmark runs |
| `/undo` | Rewind the last N turns |

## Repo map

```text
packages/
├── audits   - benchmark and evaluation harnesses
├── cli      - entrypoint and application launch surface
├── core     - engine, agents, tools, hosts, persistence
├── provider - model providers and search providers
├── shared   - interfaces, events, serialization, config
└── tui      - terminal UI components, commands, modals
```

## Documentation

- [`docs/USAGE.md`](docs/USAGE.md) — how to run Firmius day to day
- [`docs/TOUR.md`](docs/TOUR.md) — the feature tour
- [`docs/MCP.md`](docs/MCP.md) — MCP architecture, config, and workflow
- [`examples/mcp/`](examples/mcp/) — ready-to-steal MCP examples

## Current status

Firmius is pre-alpha, but it is already useful in exactly the places where most agent products still feel fake:

- long-running repo work
- multi-step execution with recoverable state
- provider routing and role specialization
- tool-rich coding and analysis sessions
- benchmark and audit flows that need containment

## Philosophy

Firmius is built around one belief:

> if agents are going to do real work, the runtime has to be honest about state, tools, coordination, and failure.

That is why this repo is opinionated.
That is why it is terminal-first.
That is why MCP, memory, orchestration, and verification are treated like first-class systems instead of launch-day copy.
