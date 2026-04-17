# STUFF THAT ACTUALLY EXISTS NOW

This is the doc the old README should've had.

If you're trying to figure out what Firmius can do **today**, and not what past-me forgot to brag about, start here.

## 1) This is not “one assistant with tools”

Firmius has a real orchestration surface.

That means:

- subagents can be summoned, waited on, and terminated
- plans can be created, updated, and activated
- chunks can be added, inspected, and marked ready for execution
- workers can coordinate with **fleet/file locks** so two agents don't body-slam the same shared file at once
- agents can hand off work through **thread-scoped artifacts**

The artifact bit is especially nice.

You can hand things around with references like:

```text
@artifact:lead/REPORT.md
```

Which means you are not forced to stuff every intermediate report into one giant wall of chat sludge.

## 2) Memory does more than politely forget stuff

Firmius has rolling memory with actual structure:

- occupancy targets
- buffer/emergency thresholds
- balanced/aggressive/extended/custom presets
- observer passes
- reflector passes
- a reserved working-memory updater slot

It also injects runtime overlays for:

- current work state
- watched files
- loaded skills
- loaded MCP state
- rolling-memory status
- durable user/project memory

Durable memory gets its own workspace under `~/.firmius/user/` with:

- `USER.md`
- `BEHAVIOR.md`
- workspace-specific fix logs

So yeah... there is a real “learned memory / project memory” lane here now.

## 3) Watched files are first-class context

Firmius keeps a live overlay of what the agent:

- read
- fully read
- edited

That sounds small until you realize how much less dumb the agent gets when it can see what parts of the repo it already touched.

This is one of those features that doesn't sound flashy, but it quietly makes the whole system less chaotic.

## 4) Exact turn recall exists too

There is a `memory_recall` tool for pulling exact preserved turns by:

- exact turn range
- cursor turn id
- simple paging

Which is VERY different from “ehhh just summarize it and hope nothing important got vaporized”.

## 5) The tool surface is kind of nasty now (compliment)

Firmius agents currently have real access to stuff like:

- file read/edit
- process execute/spawn/status/wait/input
- Python execution
- glob / grep / directory listing
- web fetch
- web search
- MCP discovery/load/call/read/get
- LSP semantic queries
- LSP diagnostics
- plan/chunk/todo tools
- artifact tools
- memory recall

That's not a toy toolkit anymore.

## 6) MCP is not pretend anymore

MCP support is implemented for:

- `stdio`
- `http`

The rough shape:

- discover with `mcp_list` / `mcp_search`
- load what you care about with `mcp_load`
- call tools with `mcp_call`
- read loaded resources/prompts with `mcp_read_resource` / `mcp_get_prompt`
- loaded tools can also appear as dynamic `mcp__<server>__<tool>` entries

See `docs/MCP.md` and `examples/mcp/` for the exact flow.

## 7) LSP is here, which means the agents can stop guessing

Firmius has real LSP-backed semantic ops:

- hover
- definition
- references
- implementation
- document symbols
- workspace symbols
- call hierarchy helpers
- diagnostics

This matters because “grep + vibes” is not the same as semantic code intel.

## 8) Local or Docker execution? Pick your poison.

Threads can run on:

- localhost
- Docker sandboxes

That means you can keep normal repo work local, and still shove benchmark runs / dirtier execution lanes into containers when needed.

## 9) Providers are not a tiny hardcoded afterthought

Current provider IDs wired into engine startup:

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

There is also provider/account state, quota tracking where supported, and a separate search-provider registry used by web search.

So the provider layer is not just “paste API key, pray, and if it dies it dies”.

## 10) Benchmark mode is built into the main app

`/benchmarks` in the TUI can kick off:

- `mbpp`
- `swebench`
- `agentbench`

And the dedicated audit CLI exposes a whole extra verification lane:

```bash
./build/packages/audits/firmius_audit --list
```

There are audits for workflows, providers, LSP, quotas, web fetch/search, resume behavior, harness chaos, benchmarks, and more.

## 11) Workflow markdown -> slash command is one of the coolest parts

Built-in workflows ship in `workflows/`.
On startup, Firmius bootstraps them into `~/.firmius/workflows/` if needed.
Then each markdown file becomes its own slash command.

That means the workflow surface is:

- user-editable
- easy to version
- dead simple to extend
- not locked behind a recompile

Honestly? This deserves way more bragging than it used to get.

## 12) Under the hood, persistence is doing real work

Thread storage tracks:

- metadata
- manifests / permission rules / fleet state
- agent turns
- plans
- todos
- live state
- rolling-memory state
- compaction snapshots
- artifacts

Which is why artifacts, undo, resume, memory recall, and planning state all feel like part of the same machine instead of a pile of disconnected hacks.

## In summary

Firmius is still rough.
But it is rough in the fun way now.

The repo already has a lot of the “oh damn, that's actually in here?” features people normally don't expect until way later.
