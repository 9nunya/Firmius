# Firmius Hooks

Hooks are user-space programs that fire on Firmius lifecycle events. They can
block tool calls, mutate persistent state, spawn validator subagents, and even
**define their own tools**. The Promise/Ralph loop ships as one such pack —
see `example/promise/`.

## Mental model

A hook is a YAML file with three layers:

1. **Trigger** — which event fires it, plus a match predicate.
2. **State** *(optional)* — what scoped KV the hook reads/writes.
3. **Action** — what the hook does. One of:
   - `shell` — run a command. Receives a JSON envelope on stdin
     (Claude Code / opencode compatible). Decision via stdout JSON or exit
     code (exit 2 = block).
   - `prompt` — inject a system reminder.
   - `agent` — spawn a branched subagent of a named persona, await its
     trophy, surface as `{{subagent.return.*}}`.
   - `workflow` — invoke another workflow id with templated args.
   - `tool_intercept` — rewrite the originating tool's args.
   - `state` — mutate hook KV without other side effects.
   - `script` — run sandboxed **Luau**. The most expressive option.
   - `compose` — sequential chain of any of the above; threads outcomes.
   - `tool` — define a brand-new user-space tool whose dispatch routes
     into a hook chain.

A pack is a directory of hook YAML files. Drop it under
`~/.firmius/hooks/<pack_name>/`, restart, done.

## Compatibility shim — porting Claude Code / opencode hooks

Shell hooks receive a JSON envelope on stdin and may emit a JSON outcome on
stdout. The envelope shape mirrors Claude Code's:

```json
{
  "hook_id": "your.hook.id",
  "event": "pre_tool_use",
  "payload": {
    "thread_id": "...",
    "agent_id": "...",
    "persona": "forge",
    "active_mode": "forge:apply",
    "tool": "Files.Edit",
    "tool_args": { "path": "...", "patch": "..." },
    "tool_result": null,
    "tool_success": null,
    "extra": {}
  },
  "state": { "global": {}, "thread": {}, "agent": {}, "hook": {} },
  "firmius_version": "1.0.0",
  "claude_code_compat": true
}
```

Outcome JSON (any subset; missing keys default sensibly):

```json
{
  "decision": "block" | "allow" | "replace",
  "reason": "shown to the user when blocked",
  "reminder": "injected into the agent's next prompt",
  "outcome": "named outcome label (used for compose chaining)",
  "replacement_args": { "path": "...", "patch": "..." },
  "state_writes": [
    { "scope": "thread", "path": "promise.iteration", "value": 2 }
  ]
}
```

Exit codes (when the hook does not emit JSON):
- `0` with non-empty stdout → reminder injected.
- `2` → block (Claude Code convention; stderr or stdout becomes the reason).
- other non-zero → soft fail; reminder injected, no block.

That's the entire contract. Existing Claude Code hooks that exit 2 to block
work as-is.

## Luau quickstart

```yaml
id: stale-edit-guard
trigger:
  on_event: pre_tool_use
  match: { tool: { equals: Files.Edit } }
  block: true
action:
  kind: script
  language: luau
  body: |
    local target = event.payload.tool_args.path
    local last_read = state.read("agent", "last_read." .. target)
    local turns_ago = event.payload.extra.turn_index - (last_read or -99)
    if turns_ago > 3 then
      return outcome.block{
        reason = "Stale read: " .. target .. " was last read " ..
                 turns_ago .. " turns ago. Reread before editing.",
      }
    end
    return outcome.allow{}
```

Luau runs in a sealed sandbox: no `loadstring`, `getfenv`, `setfenv`,
`require`, `dofile`, `os.execute`, `io.*`. Hard caps on instructions
(1M default), wall time (250ms default), and memory (8MB default).

## Discoverability

Slash command and CLI surfaces are wired in a follow-up scaffolding pass:

- `/hooks` — modal in the TUI listing registered hooks, last fire result.
- `firmius hooks list` — terminal listing.
- `firmius hooks state <id>` — dump persistent KV.
- `firmius hooks test <id> --event=pre_tool_use --tool=Files.Edit` — replay
  a synthetic event against a hook for debugging.

## Layout convention

```
~/.firmius/hooks/
├── promise/                 # the Ralph loop pack
│   ├── pack.yaml            # metadata + dependencies + install hook
│   ├── flows/               # event-triggered hooks
│   │   └── agent-stop-with-open-promise.yaml
│   ├── scripts/
│   │   └── agent_stop_validator.lua
│   └── skin/                # optional custom TUI renderers (Luau)
│       └── status.lua
└── house-doctrine/          # an example team-wide doctrine pack
    └── ...
```

Packs are pure data. No restart sequencing, no per-pack code paths in
firmius_core. TUI hook status lines are rendered by pack-provided
`skin/status.lua` files reading durable hook state.
