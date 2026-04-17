# HOW TO DRIVE THIS THING WITHOUT YELLING AT IT TOO MUCH

OK... so you actually wanna USE Firmius now. SICK.

This doc is the practical surface: how to launch it, how to move around, and where the “oh wait that feature is real now?” stuff lives.

## 1) Start the beast

Build it first:

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Then run the normal TUI:

```bash
./build/packages/cli/firmius
```

Useful launch variants:

```bash
# Continue the last session
./build/packages/cli/firmius -c

# Start a fresh thread and immediately send a prompt
./build/packages/cli/firmius --prompt "map this repo"

# Same thing, but read the prompt from a file
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

- `request` is the default permission mode.
- `always-allow` and `deny-all` also accept the short aliases `allow` and `deny`.

## 2) Slash commands that matter

These are the core built-in commands registered by the TUI right now:

| Command | What it does |
| --- | --- |
| `/new` | Create a new thread. |
| `/threads` | Switch to an existing thread. |
| `/models` | Switch the focused model. |
| `/undo [count]` | Undo the last N turns. |
| `/config` | Show current config. |
| `/memory` | Configure rolling memory models and occupancy presets. |
| `/connect <provider>` | Attach a provider with OAuth or API-key flow. |
| `/accounts <provider>` | List accounts/keys for a provider. |
| `/quotas <provider>` | Show provider quotas when supported. |
| `/router` | Manage model routing categories. |
| `/purposes` | Map personas to route categories. |
| `/mcp` | Open the MCP connections/config UI. |
| `/benchmarks <id> [task_id]` | Launch a benchmark run. |
| `/quit` | Exit cleanly. |

## 3) Purposes are the job system

In Firmius, a “purpose” is the system prompt + operating contract for an agent. It's not just flavor text. It decides what lane that agent owns.

The default cast you should know:

- **Lead** — discovery, routing, review, final decisions.
- **Executor** — edits code and lands chunk work.
- **Planner** — drafts execution structure when work gets bigger.
- **Plan Checker** — yells at bad plans until they stop being bad.
- **Worker** — executor-owned bounded subtask goblin.
- **Scout** — read-only reconnaissance gremlin.

Use:

- `/purposes` to map a persona to a routing category.
- `/router` to define what those route categories actually point at.

So yeah — you can decide that `planner` should go to one model lane, `executor` to another, and `lead` to the “think harder, be less stupid” lane.

## 4) Workflows are just markdown, which rules

Firmius bootstraps built-in workflow files into `~/.firmius/workflows/` and then registers **every workflow markdown file as its own slash command**.

Built-in examples in this repo:

- `/explore`
- `/deep_interview`
- `/evidence_sweep`
- `/repair_wave`

The command name comes from the file stem, not the pretty display title.

You can also add your own `.md` files under:

```text
~/.firmius/workflows/
```

Or override the workflow directory entirely with:

```bash
FIRMIUS_WORKFLOWS_DIR=/some/other/folder ./build/packages/cli/firmius
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

Investigate $1 and give me the real shape of the codebase.
```

Save that as `repo_explore.md`, restart Firmius, and boom: `/repo_explore` exists.

## 5) Providers, accounts, quotas, and other API nonsense

Firmius supports a mix of OAuth-backed and API-key-backed providers.

Current lazy-registered provider IDs in code:

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

Practical flow:

1. `/connect <provider>`
2. complete the wizard
3. `/accounts <provider>` if you wanna inspect what got stored
4. `/quotas <provider>` if the provider tracks quota buckets

Also important: Firmius is not stuck on one account/key per provider. The provider layer has retry/switch logic, so this isn't “paste one secret and pray”.

## 6) Memory is not just truncation anymore

`/memory` opens the rolling-memory config UI.

Stuff you can tune there right now:

- mode: `rolling_forever`, `legacy_compaction`, or `disabled`
- preset: `aggressive`, `balanced`, `extended`, or `custom`
- target / buffer / emergency occupancy thresholds
- retained tail ratio
- minimum retained tail tokens
- minimum chunk tokens
- dedicated model picks for:
  - observer
  - reflector
  - working-memory updater

Under the hood, Firmius also keeps runtime overlays for:

- active work state
- watched files (`read`, `fully read`, `edited`)
- loaded skills
- loaded MCP capabilities
- rolling-memory status
- durable user/project memory

The durable memory workspace lives under `~/.firmius/user/` and includes:

- `USER.md`
- `BEHAVIOR.md`
- per-project fix logs

So yes... it remembers more than “the last few turns before the context window explodes”.

## 7) Benchmarks and audits

Benchmark mode is built in.

In the TUI:

```text
/benchmarks mbpp
/benchmarks swebench
/benchmarks agentbench
```

Notes:

- benchmark runs spin up a Docker-backed worker lane
- Docker needs to be alive
- the image `firmius-sandbox:latest` needs to exist first

There is also a dedicated audit CLI:

```bash
./build/packages/audits/firmius_audit --list
```

That surface includes workflow audits, LSP audits, provider audits, quota audits, web fetch/search audits, harness chaos audits, benchmark audits, and more.

## 8) Threading / persistence / recovery

Firmius persists way more than just “chat messages”.

The thread database tracks:

- thread metadata
- agent turns
- plans
- todos
- live agent state
- rolling-memory state
- compaction snapshots
- artifacts

Which is why artifacts, undo, resume, memory recall, and planning state all feel like part of the same machine instead of a pile of disconnected hacks.

## 9) Where to go next

- Want the big feature tour? `docs/TOUR.md`
- Want MCP specifics? `docs/MCP.md`
- Want credential-free MCP starter files? `examples/mcp/`
