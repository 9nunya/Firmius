# Usage

This is the practical guide for getting Firmius running and using the parts that matter first.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build --parallel
```

## Launch

Start the main TUI:

```bash
./build/packages/cli/firmius     # macOS/Linux
./build/packages/cli/firmius.exe # Windows
```

Useful variants:

```bash
# Continue the last session
./build/packages/cli/firmius -c
./build/packages/cli/firmius.exe -c

# Start a fresh thread and immediately send a prompt
./build/packages/cli/firmius --prompt "map this repo"
./build/packages/cli/firmius.exe --prompt "map this repo"

# Read the prompt from a file
./build/packages/cli/firmius --prompt-file task.md

# Start in a specific working directory
./build/packages/cli/firmius --cwd /path/to/repo

# Exit automatically once the launched prompt settles
./build/packages/cli/firmius --prompt "do the thing" --quit-when-idle

# Permission modes
./build/packages/cli/firmius --permission-mode request
./build/packages/cli/firmius --permission-mode always-allow
./build/packages/cli/firmius --permission-mode deny-all
```

Notes:

- `request` is the default permission mode
- `always-allow` and `deny-all` also accept `allow` and `deny`

## Commands worth learning first

| Command | Purpose |
| --- | --- |
| `/new` | Create a new thread |
| `/threads` | Switch to another thread |
| `/models` | Change the focused model |
| `/undo [count]` | Rewind transcript turns; when omitted, rewinds to the last user message boundary (same as Alt+Backspace) |
| `/undo_turn` | Undo the last agent turn (same as Ctrl+Z) |
| `/redo` | Redo last transcript undo (same as Ctrl+Shift+Z) |
| `/history` | Show transcript rewind console |
| `/edits` | Show persisted edit history and rollback tools |
| `/config` | View current config |
| `/memory` | Configure rolling-memory behavior |
| `/connect <provider>` | Add a provider account |
| `/accounts <provider>` | Inspect stored accounts or keys |
| `/quotas <provider>` | View provider quota state when supported |
| `/router` | Manage routing categories |
| `/purposes` | Map purposes to routes |
| `/mcp` | Configure MCP servers and connections |
| `/benchmarks <id> [task_id]` | Run a benchmark lane |
| `/quit` | Exit cleanly |

## Purpose lanes

A purpose in Firmius is not just flavor text. It defines how an agent participates in the session.

The default cast:

- **Lead** — routing, review, and high-level decisions
- **Executor** — code changes and chunk execution
- **Planner** — execution structure for larger work
- **Plan Checker** — plan validation and pushback
- **Worker** — bounded delegated execution
- **Scout** — read-only exploration

Use:

- `/purposes` to decide which route a purpose uses
- `/router` to define which model lane each route points to

This is how you stop treating every model like it should do every job equally well.

## Workflows

Firmius turns workflow markdown into slash commands.

Built-in files ship in `workflows/`, are bootstrapped into `~/.firmius/workflows/`, and then registered by filename.

Examples in this repo:

- `/explore`
- `/deep_interview`
- `/evidence_sweep`
- `/repair_wave`

You can add your own `.md` files under:

```text
~/.firmius/workflows/
```

Or override the directory entirely:

```bash
FIRMIUS_WORKFLOWS_DIR=/some/other/folder ./build/packages/cli/firmius     # macOS/Linux
FIRMIUS_WORKFLOWS_DIR=/some/other/folder ./build/packages/cli/firmius.exe # Windows shells that honor this env form
```

Minimal example:

```md
---
name: Repo Explore
description: Map the repo and report the sharp edges
args:
  - name: target
    type: string
    description: What to inspect
---

Investigate $1 and explain the real shape of the codebase.
```

Save that as `repo_explore.md`, restart Firmius, and `/repo_explore` becomes available.

## Providers and model routing

Firmius supports multiple providers and does not force the entire product through one account or one model lane.

Current provider IDs include:

- `nanogpt`
- `nvidia`
- `openrouter`
- `zai`
- `zen`
- `chutes`
- `codex`
- `antigravity`
- `qwen`
- `kimi`
- `kilo`

Typical flow:

1. `/connect <provider>`
2. complete the setup flow
3. `/accounts <provider>` to inspect what is stored
4. `/quotas <provider>` when the provider exposes quota state

## Memory and recall

Open `/memory` to configure the rolling-memory system.

Current controls include:

- mode: `rolling_forever`, `legacy_compaction`, or `disabled`
- preset: `aggressive`, `balanced`, `extended`, or `custom`
- target, buffer, and emergency occupancy thresholds
- retained tail ratio
- minimum retained tail tokens
- minimum chunk tokens
- dedicated model picks for:
  - observer
  - reflector
  - working-memory updater

Firmius also maintains runtime overlays for:

- active work state
- watched files
- loaded skills
- loaded MCP state
- rolling-memory status
- durable user and project memory

Durable memory lives under `~/.firmius/user/` and includes `USER.md`, `BEHAVIOR.md`, and project-specific logs.

## MCP flow

Firmius supports MCP over `stdio` and `http`.

Recommended first run:

1. copy an example from `examples/mcp/`
2. configure a filesystem server
3. confirm the configured server is enabled
4. call the resulting dynamic tool name such as `mcp__<server>__<tool>`

For the current MCP runtime model, see [`docs/MCP.md`](MCP.md).

## Benchmarks and audits

Benchmarks are available directly from the TUI:

```text
/benchmarks mbpp
/benchmarks swebench
/benchmarks agentbench
```

Notes:

- benchmark runs use a Docker-backed worker lane
- Docker needs to be available
- the `firmius-sandbox:latest` image needs to exist first

There is also a dedicated audit CLI:

```bash
./build/packages/audits/firmius_audit --list
./build/packages/audits/firmius_audit.exe --list
```

The repository CI workflow `.github/workflows/cross-platform-ci.yml` builds the main binaries and runs the migrated proof slice with:

`test_json_rpc_transport`
`test_lsp_server_manager`
`test_engine_shutdown`
`test_lsp_audit`

That surface covers workflow, provider, MCP, LSP, quota, harness, benchmark, and other validation lanes.

## Persistence

Firmius persists more than messages. Threads keep:

- metadata
- agent turns
- plans
- todos
- live state
- rolling-memory state
- compaction snapshots
- artifacts

That is why resume, undo, recall, and handoffs stay coherent across longer sessions.

## Next stops

- Want the product-level overview? [`docs/TOUR.md`](TOUR.md)
- Want MCP specifics? [`docs/MCP.md`](MCP.md)
- Want examples you can paste into config? [`examples/mcp/`](../examples/mcp/)
