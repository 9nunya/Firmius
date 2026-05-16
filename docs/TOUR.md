# Product tour

Firmius is built for people who want agent systems with actual runtime discipline.

This is the fast tour of what makes it different.

## 1. Multi-agent orchestration is the center of the product

Firmius is not a single assistant pretending to be a team.

It supports:

- subagent spawn, wait, and termination flows
- explicit role lanes like lead, planner, executor, scout, and worker
- plans and chunks as first-class execution structure
- thread-scoped artifacts for clean handoffs
- coordination primitives that keep parallel work from trampling shared files

If you need agents to do more than talk nicely, this is the layer that matters.

## 2. Terminal-native on purpose

Firmius is a terminal product.

That is not a temporary compromise. It is the advantage.

The UI stays close to the work, close to the repo, close to the tools, and close to the long-running sessions that make orchestration valuable. No browser wrapper, no split-brain product story, no fake simplicity that falls apart once the work gets serious.

## 3. MCP is a real runtime feature

Firmius supports MCP over `stdio` and `http`, including:

- automatic MCP server initialization for configured servers
- resource reads and prompt resolution through the runtime
- direct dynamic tool exposure as `mcp__<server>__<tool>`

That means MCP is integrated into the workflow, not stapled on top of it.

## 4. Rolling memory is engineered, not improvised

Firmius does not rely on “summarize harder” as its memory strategy.

You get:

- occupancy targets and thresholds
- balanced, aggressive, extended, and custom presets
- observer and reflector passes
- a dedicated working-memory updater lane
- watched-file overlays
- durable user and project memory
- exact turn recall when compressed context is not enough

This is the difference between an agent staying coherent and an agent slowly becoming fiction.

## 5. Watched files are first-class context

Firmius tracks what the runtime has:

- read
- fully read
- edited

That sounds small until you use it in a large repo. The agent stops wasting turns rediscovering its own work and gets much better at staying grounded in what actually changed.

## 6. The tool surface is built for real work

Firmius agents can use:

- file and directory tools
- grep, glob, and edit flows
- process execution and interactive subprocess control
- Python execution
- dynamic MCP tools from configured servers
- LSP semantic queries and diagnostics
- planning, todos, and artifacts
- web fetch and web search inside the runtime

This is not “LLM plus two accessories.” It is a working toolbelt.

## 7. LSP support keeps agents out of the guesswork lane

Firmius includes real semantic code intelligence:

- hover
- definition
- references
- implementation
- document symbols
- workspace symbols
- diagnostics
- call hierarchy helpers

That means agents can navigate code like a coding tool should, instead of pretending keyword search is good enough.

## 8. Provider routing is part of the architecture

Firmius supports multiple providers and lets you route different purposes to different model lanes.

Current provider set includes:

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

That matters because planning, execution, review, and memory maintenance do not all need the same model profile.

## 9. Workflows are editable markdown, not hardcoded ceremony

Built-in workflows live in `workflows/`.
Firmius bootstraps them into `~/.firmius/workflows/` and turns each markdown file into a slash command.

That gives you a workflow system that is:

- versionable
- inspectable
- user-editable
- fast to extend

The result is more open than a hidden prompt system and more practical than rebuilding the app for every workflow tweak.

## 10. Persistence is doing real product work

Firmius thread storage keeps:

- thread metadata
- agent turns
- plans
- todos
- live state
- rolling-memory state
- compaction snapshots
- artifacts

That is why undo, resume, recall, artifacts, and planning feel like one product instead of disconnected features.

## 11. Audits and benchmarks are built in

Firmius is opinionated about verification.

You can run benchmark flows like:

- `mbpp`
- `swebench`
- `agentbench`

And the audit CLI gives you a separate lane for validation:

```bash
./build/packages/audits/firmius_audit --list
```

That is a big part of the product story: not just doing work, but checking whether the runtime deserves your trust.

## Bottom line

Firmius is for people who are done pretending agent orchestration is solved by nicer chat chrome.

It is faster, more honest, and more capable because it is built around the runtime problems that other tools usually hide.
