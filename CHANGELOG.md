# Changelog

## 0.0.6 — 2026-09-13

Firmius v0.0.6 is the TUI-and-daemon release. It turns the terminal client
into an operator surface for durable work and gives the runtime a proper local
daemon boundary. The refreshed release also bundles the desktop client.

### Release refresh

- Build, package, and install `firmiusd` alongside the terminal client; reject
  incomplete older archives before replacing installed executables.
- Preserve Windows companion executables through deferred installation.
- Reconcile cancellation state and surface event-transport failures for recovery.
- Invalidate stale composer layout after edits to prevent cursor-navigation crashes.
- Remove the confirmed-YOLO action-kind whitelist and enable native todos for
  every bundled working persona, including Reviewer, but not the Memory Curator.
- Add light mode and fifteen additional themes, with accent-colored title glints
  rather than animated preview output.

### Highlights

- Added the `firmiusd` daemon, authenticated local client protocol, reconnecting
  client, endpoint metadata, profile lease, lifecycle controls, and snapshot
  recovery.
- Added durable native todos, cross-session memory, goals, project/workspace
  identity, edit coordination and history, permission boundaries, and explicit
  tool-path handling.
- Added managed workflow execution: dependency graphs, predecessor result
  binding, bounded feedback/retry loops, independent review, verification
  gates, result annotations, quality digests, and durable outbox/scheduler
  behavior.
- Reworked the TUI around typed projections for sessions, work, goals, todos,
  memory, permissions, provider/account state, reconnects, and run progress.
- Added SSH workspace targeting, daemon-aware goal commands, onboarding and
  install diagnostics, prompt inspection, and safer update/install behavior.
- Added protocol, client, daemon-boundary, work-graph, persistence,
  coordination, memory, todo, and scheduler test coverage.

### Runtime and daemon

- The daemon keeps accepted turns running when a client window disconnects.
- Local IPC is authenticated and protected by an exclusive profile lease.
- Event gaps and transport failures recover from an authoritative snapshot
  instead of reconstructing state from partial transcript output.
- Shutdown, reconnect, stale endpoint, and persistence paths now have explicit
  outcomes rather than silently presenting a false success state.

### TUI and operator controls

- Added focused work, todo, goal, permission, account/quota, session, and run
  views with compact layouts for smaller terminals.
- Added command palette/help surfaces, onboarding reset, clipboard support,
  settings, SSH host management, edit history, undo/redo, and workflow launch
  controls.
- Added live status for queued, running, waiting, blocked, reconnecting,
  failed, and unknown outcomes.
- Kept renderer state separate from daemon truth: UI projections do not infer
  durable state from assistant prose.

### Safety and quality

- Added scoped tool permissions, authenticated identities, focused-agent
  ownership, path containment, edit conflict checks, and durable audit trails.
- Added context budgets, selective compaction, memory safety boundaries, and
  typed artifact/evidence handling for long-running sessions.
- Installers now validate repository/version inputs, require checksums, stop a
  running daemon before replacement, and use staged binary/metadata updates.

### Release boundary

- v0.0.6 publishes only the CLI/TUI, daemon, protocol, client, core runtime,
  installers, and user documentation.
- The active desktop client, desktop design material, internal refinery specs,
  scenario fixtures, audit helper binaries, and session/agent result artifacts
  are not included in the public release.

## 0.0.5 — 2025-02-23

Firmius got dramatically better: this release delivers a substantially more
capable collaboration and workflow engine, with richer orchestration, durable
state, and a much sharper terminal experience.

### Highlights

- Added managed workflow runs, DAG planning, predecessor result binding, and
  bounded feedback/retry loops.
- Expanded task graph authorization, persistence, ownership, reconciliation,
  verification gates, annotations, and quality digests.
- Improved provider integrations, capabilities, web search responses, and
  context/reasoning handling.
- Refined the TUI, including run views, settings, clipboard support, and more
  useful assignment and workflow context.
- Added batched task completion and more reliable delegation and messaging.