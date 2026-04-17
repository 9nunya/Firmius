# FIRMIUS

Firmius is a C++-native agent cockpit for people who want **actual orchestration** instead of one polite assistant pretending to be a whole company.

It is still pre-alpha. It is still a little feral. And the old README was straight-up out of date — because a LOT more of this thing is real now.

## OK, SO WHAT ACTUALLY EXISTS NOW?

- **Real multi-agent execution** — subagents, plan/chunk workflows, worker handoffs, and file-lock coordination for parallel work.
- **Workflow markdown that turns into slash commands** — built-ins bootstrap into `~/.firmius/workflows/`, and every `.md` becomes a `/command`.
- **Rolling memory that is more than dumb truncation** — occupancy presets, observer/reflector passes, and a reserved working-memory updater slot.
- **Watched-file overlays + exact turn recall** — agents keep live awareness of what they read, fully read, and edited.
- **Thread-scoped artifacts** — real handoff files with references like `@artifact:lead/REPORT.md`.
- **MCP over both `stdio` and `http`** — not vapor, not “coming soon”, actually implemented.
- **Real LSP-backed code intel** — hover, definition, references, implementation, symbols, and diagnostics.
- **Web fetch + web search built in** — plus a pluggable search-provider registry.
- **Localhost OR Docker execution hosts** — depending on how spicy you want the run to be.
- **Provider/account plumbing that is way fatter than the old README admitted** — `nanogpt`, `nvidia`, `openrouter`, `zai`, `zen`, `chutes`, `codex`, `antigravity`, `qwen`, `kimi`, `kilo`.
- **Benchmark mode** for `mbpp`, `swebench`, and `agentbench`.

## QUICK START, NO TED TALK

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Run the TUI:

```bash
./build/packages/cli/firmius
```

Continue the last session:

```bash
./build/packages/cli/firmius -c
```

Fire a one-shot prompt into a fresh thread:

```bash
./build/packages/cli/firmius --prompt "audit this repo" --quit-when-idle
```

See the audit/benchmark CLI surfaces:

```bash
./build/packages/audits/firmius_audit --list
```

## COMMANDS YOU'LL PROBABLY TOUCH FIRST

- `/new` — new thread, clean slate, go crazy.
- `/threads` — switch threads.
- `/models` — switch the focused model.
- `/connect` — attach a provider with OAuth or API-key flow.
- `/accounts` / `/quotas` — inspect provider state instead of guessing.
- `/memory` — rolling-memory settings, presets, and maintenance-model picks.
- `/router` — define routing categories.
- `/purposes` — map personas to routing categories.
- `/mcp` — open MCP config/connections UI.
- `/benchmarks` — launch Docker-backed benchmark runs.
- `/undo` — rip back the last N turns.

## DOCS, BUT LIKE... THE USEFUL ONES

- `docs/USAGE.md` — how to drive the thing without fighting it.
- `docs/TOUR.md` — the under-advertised stuff Firmius already has.
- `docs/MCP.md` — current MCP behavior, caveats, and flow.
- `examples/mcp/` — credential-free starter configs and tool-call examples.

## HONESTY HOUR

- **Linux / POSIX first.** This repo still has POSIX energy all over it.
- **Still pre-alpha.** Fast, capable, useful... also sharp in places.
- **Not every provider/model combo is equally battle-tested.** Some paths are way more cooked than others.
- **MCP TLS extras are parsed, but not fully wired yet.** `stdio` and plain `http` are the happy paths right now.
- **This is not the polished SaaS product page version of itself yet.** It is the garage-built “why is this kinda cracked already?” version.
