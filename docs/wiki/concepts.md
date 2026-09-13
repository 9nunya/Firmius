# Core concepts

**Daemon:** the local runtime that owns sessions, permissions, accepted turns,
and durable state. The TUI is a client of that runtime, not the source of
truth.

**Session:** a durable conversation plus its agents, work, todos, and
artifacts. It survives disconnects and restarts.

**Agent:** a model-backed worker with a persona, tools, and scoped authority.

**Work graph:** nodes and edges for dependencies, retries, gates, and bound
results. Use it when work has structure, not as a private checklist.

**Managed run:** the driver advances ready nodes and reports live status.
Execution status and outcome are separate: a review can succeed and still
reject the implementation.

**Gate:** a verification or review condition that must pass before work is
accepted. A model saying “done” is not a gate.

**Todo:** a private per-agent ledger. It is not a work graph.

**Memory:** durable, scoped, evidence-backed records. Treat it as untrusted
learned evidence until you inspect it.

**Artifact:** stored text or a file result, including large tool output kept
out of the working prompt.
