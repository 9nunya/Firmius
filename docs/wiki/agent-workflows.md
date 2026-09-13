# Getting substantial work done with Firmius

Give Firmius an outcome and the constraints that matter. For example:

> Investigate startup latency across file loading and network initialization. Run those investigations independently, synthesize the evidence, implement the smallest useful fix, and independently review it. Preserve unrelated changes and show measured validation.

The Lead chooses direct execution, delegation, or a durable workflow. Independent branches can run concurrently; dependent stages receive earlier results automatically. Workers share the working tree, so changes to overlapping files should be ordered. They can also send durable messages to their parent, a named agent, siblings, or the fleet while they work. Firmius uses those messages to surface a blocker, coordinate file ownership, or route an important finding before a worker reaches its final report.

The model-facing `workflow` tool creates and launches a graph in one call:

```json
{
  "title": "Investigate startup latency",
  "brief": "Identify measured causes, cite evidence, and recommend focused fixes.",
  "steps": [
    {"key": "files", "persona": "general", "prompt": "Investigate startup file loading."},
    {"key": "network", "persona": "general", "prompt": "Investigate startup network requests."},
    {"key": "synthesis", "persona": "general", "prompt": "Rank the causes using both investigations.", "depends_on": ["files", "network"]}
  ]
}
```

It returns a `run_id` and `graph_id`. `workflow` with `action: "status"` inspects progress; `action: "wait"` returns the report; `action: "cancel"` stops the run. Each takes `run_id`. The TUI renders this as a swarm card with its title, concurrency budget, progress, and rejected review count; the durable work panel remains the detailed source of truth. A result's execution status and outcome are separate: a review can execute successfully and reject the implementation. Read the outcome and evidence before declaring success.

Each shortcut step gets one attempt. For automatic review/correction loops, use the advanced `task` graph API with bounded feedback edges. For substantial solo work, use a small `task` checklist. For one bounded assignment, use `delegate`. Routine checklist changes do not need revision bookkeeping; explicit revisions remain available for compare-and-swap operations.

## Inspect the prompts you are actually loading

```sh
firmius prompt
firmius prompt coder
```

This prints JSON containing the startup system prompt, its SHA-256 fingerprint, persona source, bundled/custom status, scopes, and built-in tool schemas. It does not open a model connection. Active sessions can additionally contain custom operator policy, generated assignments, provider tools, and connected MCP tools; this command is not a dump of a live model request.

New stock persona files contain a pointer such as `@firmius/bundled/lead`. It selects the definition bundled with the executable, so rebuilding updates the behavior. Exact known historical stock files also select the current bundled definition in memory, without overwriting their disk contents. Edited Markdown personas remain custom overrides. To customize a stock persona, copy its bundled Markdown from `crates/core/src/personas/` into the corresponding file under `~/.firmius/personas/` and edit it. To return to stock, replace that file with its bundled pointer.

The former unmodified default operator prompt is also normalized when a saved session makes its next model request. Custom operator policy remains intact. Restart the running Firmius process/daemon after upgrading the executable so it loads the new code and personas.

Large tool results in attached sessions are stored under `artifact://tool-results/`. Use `read` with `start_line` and `limit`, or search with `grep`, to retrieve relevant portions without leaving the agent's filesystem scope.
