# Daemon Architecture And Readiness

## Purpose

This document is the hard gate for the Firmius daemon effort.

It exists to prevent the project from drifting into:
- fake daemon progress
- partial backend parity disguised as “good enough”
- premature TUI migration
- workflow-specific logic hardcoded into backend C++
- UI teams discovering backend truth gaps during cutover

The rule is simple:

**No serious TUI-to-daemon migration starts until the daemon can honestly replace direct core/runtime reads for the surfaces the TUI depends on.**

---

## Target Architecture

Firmius should converge on this shape:

```text
Clients
├── TUI
├── Web
└── Desktop
        │
        ▼
Typed client SDK / daemon protocol
        │
        ▼
firmiusd
├── session registry
├── workspace/repo presence
├── RPC dispatch
├── event fanout
├── runtime snapshots
├── config/catalog snapshots
└── orchestration bridge into core
        │
        ▼
core / provider / shared / persistence
```

### What the daemon must own

- client sessions and presence
- runtime control
- runtime snapshots
- reconnect-safe truth
- catalog/config/provider state
- hook/workflow/pact state
- process/tool/subagent observation
- history/edit-history/undo-redo surfaces
- benchmark control and observability

### What clients should own

- rendering
- keyboard/input behavior
- local optimistic UI behavior
- view/layout concerns
- client-local preferences

### What clients must not own

- direct `Harness` calls
- direct `AgentRegistry` reads
- direct `ProviderRegistry` reads
- direct `ThreadManager` reads
- runtime state reconstruction that should be daemon truth
- workflow-specific semantic synthesis that belongs in stored workflow state

---

## Current Daemon Shape

As of this checkpoint, the daemon has real progress in these areas:

- external `firmiusd` process exists
- real IPC + JSON-RPC transport exists
- session/client registration exists
- workspace/repo presence exists
- thread and agent control exists
- process snapshots exist
- transcript snapshots exist
- tool-call snapshots exist
- subagent activity snapshots exist
- model/provider/account/quota/config/router/purposes/MCP surfaces exist
- history/edit-history surfaces exist
- hook and pact surfaces exist

This is enough to say the daemon is real.

It is **not** enough to say clients are ready to stop calling core directly.

---

## Hard Readiness Checklist

Legend:
- `PASS`: acceptable for cutover
- `PARTIAL`: usable, but still risky or incomplete
- `FAIL`: blocks cutover

### 1. Daemon Boundary

- `PASS` Real external daemon exists.
  - `firmiusd` is separate from `firmius`
  - real IPC/JSON-RPC transport exists
  - clients do not instantiate the server in-process

- `PASS` Client session and presence model exists.
  - client identity
  - workspace/repo metadata
  - subscription lifecycle

### 2. Runtime Control

- `PASS` Thread/agent/session control exists.
  - create/open/focus/send work over daemon APIs
  - target-scoped daemon send and permission mutation no longer bridge through ambient `Harness` focus for these cutover-critical mutations
  - evidence:
    - target-scoped send path uses `Harness::sendToThreadAgent(...)` instead of `switchThread`/restore focus juggling in `packages/server/src/daemon/DaemonService.cpp`
    - target-scoped permission mutation uses `Harness::setThreadPermissionMode(...)` instead of `switchThread`/restore focus juggling in `packages/server/src/daemon/DaemonService.cpp`
    - daemon-boundary smoke tests verify omitted-thread send isolation and focus/send isolation across two clients
  - remaining note: `threads.open` still updates daemon session focus and runtime observation routing by design, but the cutover-critical mutation bridge called out here has been removed

- `PASS` Multi-client mutation isolation is proven for daemon thread send/focus-critical behavior.
  - one client cannot stomp another client’s omitted-thread send targeting or retarget a send by changing its own focus
  - required proof exists in concurrent daemon-boundary smoke coverage for:
    - send isolation with omitted thread ids
    - focus switching without cross-thread bleed
    - focus switching by one client not retargeting another client’s send
  - note: this is evidence-backed for the cutover-critical send/focus mutation surface exercised here, not a blanket claim about every future daemon mutation surface

### 3. Runtime Truth

- `PARTIAL` Transcript snapshot exists.
  - current status: daemon can provide transcript truth and compaction-expanded views
  - risk: live reconnect fidelity during active runtime is still under-proven
  - done when: reconnect during active streaming/compaction yields the same visible truth as continuous observation

- `PARTIAL` Tool-call snapshot exists.
  - current status: reconstructable historical truth exists
  - risk: some live transient/in-flight states remain partial
  - done when: active lifecycle phases and reconnect behavior are fully client-safe

- `PARTIAL` Subagent activity snapshot exists.
  - current status: reconnect-useful historical truth exists
  - risk: transient live fields remain incomplete
  - done when: waiting/retrying/provider-waiting/account-switch/current-run fields are daemon-authoritative

- `PARTIAL` Process snapshots exist.
  - current status: owned/blocking/background/running state exists
  - risk: live stdout/stderr/reconnect/process concurrency coverage is still too weak
  - done when: multi-process reconnect/runtime correctness is proven

### 4. Catalog And Config

- `PARTIAL` Models/providers/accounts/quotas/config/router/purposes/MCP surfaces exist.
  - current status: broad RPC coverage exists
  - risk: provider onboarding and some MCP runtime-health semantics are not yet fully daemon-first
  - done when: `/connect`, `/providers`, `/accounts`, `/quotas`, and MCP runtime status are entirely daemon-backed

### 5. Hook And Pact State

- `PARTIAL` Hook state exists as a typed daemon surface.
  - current status: typed-first, `snapshot_json` auxiliary
  - risk: reconnect-safe generic hook meaning still relies too much on raw persisted state and there is still no generic persisted hook-activity journal across hook kinds
  - done when: a more generic persisted hook-activity/state story exists across hook kinds and live hook semantics stop depending on state-shape-specific inference

- `PARTIAL` Pact state exists as a typed daemon surface.
  - current status: structural/pass-through only, no daemon-generated workflow text
  - risk: still constrained by current `thread.promise` persistence shape and only partially reconstructable from current core state
  - done when: pact truth is no longer effectively a narrow workflow-state projection and lifecycle/event semantics are available without shape-specific conventions

### 6. History And Edit History

- `PARTIAL` History/edit-history surfaces exist.
  - current status: typed RPCs exist for history/get/undo/redo and edits/list/undo/redo
  - risk: mutation still depends on live agent/environment semantics
  - done when: reopened-thread behavior is deterministic enough that clients do not need hidden runtime coupling

### 7. Operational Surfaces

- `PARTIAL` Benchmark backend exists, but is not yet proven complete enough for cutover.
  - current status:
    - benchmark start exists
    - benchmark status exists
    - benchmark log snapshot exists
    - benchmark id canonicalization and task validation/default selection exist
  - risk:
    - broader benchmark lifecycle semantics are still lightly verified
    - runtime observability and reconnect behavior are still under-proven
    - benchmark control is still core-bridged rather than explicitly session-safe
  - done when:
    - benchmark state/log behavior is reconnect-safe
    - benchmark lifecycle transitions are observable enough for client truth
    - concurrent/long-running benchmark daemon behavior is tested

### 8. Event Contract

- `PASS` Event contract is stable enough for current daemon cutover-critical consumers.
  - typed cutover-critical event payloads exist for runtime, hook, pact, and client-session events
  - subscribe/open/reconnect ordering rules are implemented and test-covered for focused-session runtime observation and session-driven hook/pact snapshots
  - typed event payload evidence:
    - `EventSubscriptionRequest` / `EventSubscriptionResponse` serialize as typed JSON objects in `ProtocolSerialization.cpp`
    - `DaemonEventEnvelope` serializes typed runtime/hook/pact/session fields in `ProtocolSerialization.cpp`
    - daemon service emits focused-session runtime events and typed hook/pact/client-session envelopes through `DaemonService.cpp`
  - current contract remains snapshot + live focused-session delivery with no replay/dedupe cursor protocol; that limitation is now explicit rather than implicit
  - done here means: snapshot + live event coherence is documented by implementation/tests for the currently exposed contract, not that a richer replay cursor API exists

### 9. Verification

- `PASS` Daemon-boundary smoke coverage exists.
  - current status: broad multi-client daemon-boundary smoke tests exist for create/open/send, realtime routing, focus switching, and focus/send isolation
  - covered smoke cases include:
    - existing daemon connect/create/open thread flow
    - two-client process-boundary send flow
    - realtime focused-session routing across two clients/threads
    - focus-switch rebinding without cross-thread bleed
    - omitted-thread send isolation
    - focus switching by one client not retargeting another client’s send
    - malformed `client.hello` and malformed `threads.create` params
  - proof command:
    - `cmake --build build --target firmius_server_daemon_ipc_smoke_tests -- -j1`
    - `ctest --test-dir build --output-on-failure -R firmius_server_daemon_ipc_smoke_tests`

- `PARTIAL` Live-runtime verification is insufficient.
  - current status: live daemon smoke now covers transcript-adjacent send growth signals and realtime focused-session event routing/focus rebinding
  - still missing for full completion:
    - process output live-runtime proof
    - subagent activity live-runtime proof
    - hook/pact reconnect parity proof
  - done when: at least one live end-to-end daemon test covers:
    - transcript growth
    - tool lifecycle
    - process output
    - subagent activity
    - hook/pact changes
    - reconnect parity

- `FAIL` Cross-platform verification is insufficient.
  - done when: Windows named-pipe lifecycle/connect/reconnect/concurrency tests run in CI

- `PARTIAL` No cutover regression matrix exists.
  - current mapping now exists in daemon smoke coverage for:
    - multi-client concurrent mutation on send/focus-critical paths
    - thread create/open/focus/send
    - focused-session runtime event routing and rebinding
    - malformed request coverage for `client.hello` and `threads.create`
  - still missing explicit mapped proof for process/subagent/benchmark/history/edit-history/provider/config cross-sections

---

## Brutal Quality Gates

These are non-negotiable.

### Architecture Gates

- No client may instantiate or embed the daemon in-process.
- No new workflow-specific English status text may be generated in daemon C++.
- If a status line or blocking reason belongs to workflow meaning, it must come from workflow-owned stored state or script output.
- If an RPC method is declared, it must be:
  - implemented
  - serialized
  - dispatched
  - tested
  - or removed from the public protocol
- No new cutover-critical daemon mutation may depend silently on global current-thread/current-agent state. If bridging remains, it must be explicit and documented.

### Protocol Gates

- Use one wire naming convention: `snake_case`
- No mixed field naming across related RPCs
- No ad hoc raw JSON blobs presented as the main API when a typed subset can be defined
- Auxiliary raw JSON is acceptable only when:
  - clearly marked auxiliary/debug
  - not the primary semantic contract

### Backend Truth Gates

- Reconnect-safe truth beats event-tail truth
- Snapshot truth beats client-side guesswork
- No client should need to reconstruct runtime truth from persisted history if the daemon can do it once centrally
- If a surface is still reconstructive or partial, the daemon must say so honestly

### Verification Gates

- “Builds” is not proof
- Seeded fixture tests are not enough for live-runtime claims
- New daemon surfaces require:
  - malformed-param coverage
  - real RPC boundary coverage
  - reconnect/readback coverage where applicable
- For cutover readiness, at least one live daemon-boundary integration test must validate runtime truth rather than only seeded reconstruction

---

## Backend Gaps Still Blocking TUI Migration

These are the highest-value remaining gaps before the TUI should be switched over in earnest.

### 1. Multi-client mutation isolation

Why it matters:
- the whole daemon rationale includes multiple clients/windows
- if client A can reroute client B’s work through focusful core APIs, the architecture is still unsafe

What closes it:
- explicit target-scoped core APIs
- concurrent daemon-boundary tests proving no focus stomping

### 2. Hook/pact generalized runtime truth

Why it matters:
- `/promise`-style and workflow-heavy status surfaces must be reconnect-safe
- the daemon must not depend forever on one narrow persisted shape
- workflow/Luau-owned semantics must remain workflow-owned instead of creeping into daemon C++

What closes it:
- generalized persisted hook activity/state
- stronger canonical pact model in core or daemon-fed projections
- better live transition events

### 3. Benchmark backend

Why it matters:
- benchmarks are a real product surface
- they now have initial backend parity, but not enough runtime proof or lifecycle completeness for cutover confidence

What closes it:
- stronger reconnect/runtime proof for benchmark state/log surfaces
- benchmark lifecycle/event completeness
- benchmark concurrency and long-run daemon verification

### 4. Event contract hardening

Why it matters:
- a client cutover lives or dies on snapshot + event coherence

What closes it:
- typed event subset for cutover-critical domains
- ordering/replay/dedupe rules
- reconnect behavior tests

### 5. Live-runtime verification

Why it matters:
- today’s daemon proof is still too reconstructive in places

What closes it:
- live daemon tests for transcript/tool/process/subagent/hook/pact runtime changes
- reconnect parity assertions

---

## Required Automated Testing Before TUI Cutover

Every cutover-critical daemon surface must have daemon-boundary tests.

### Minimum automated suite

- daemon startup/shutdown
- daemon reconnect/autostart
- multi-client concurrent connect
- multi-client concurrent mutation
- thread create/open/focus/send
- transcript snapshot during idle and active runtime
- tool-call snapshot during idle and active runtime
- process snapshot with live stdout/stderr and exit
- subagent activity during live wait/run/finish
- hook state snapshot
- pact state snapshot
- benchmark start/status/log over the daemon boundary
- history/edit-history mutation/readback
- provider/config mutation/readback
- Windows named-pipe transport tests in CI

### Every new RPC must include

- valid request success path
- malformed params failure path
- not-found or ineligible failure path where applicable
- serialization/deserialization coverage

---

## Required Manual Verification Before TUI Cutover

Manual verification is still required even if CI is green.

### Daemon lifecycle

- Start `firmiusd`
- Connect a client
- Disconnect and reconnect
- Confirm daemon remains healthy
- Confirm multiple clients can connect simultaneously

### Runtime truth

- Start a thread, send a prompt, confirm transcript snapshot matches live runtime
- Trigger tool calls, confirm tool-call snapshot matches visible reality
- Spawn a process, confirm stdout/stderr/exit state is visible after reconnect
- Spawn a subagent, reconnect mid-run, confirm subagent activity remains truthful

### Hook and pact truth

- Trigger a hook-driven workflow
- Confirm hook state can be queried after reconnect
- Trigger a pact/promise workflow
- Confirm pact state is queryable after reconnect
- Confirm any human-facing status text shown is workflow-owned, not daemon-invented

### Config/catalog truth

- Change provider/config/router/purpose/MCP state through daemon RPCs
- Restart daemon
- Confirm readback is correct and persisted

### Operational surfaces

- Run benchmark flows and confirm backend status/log surfaces
- Leave a benchmark run active long enough to verify reconnect and status/log readback
- Exercise transcript undo/redo and edit undo/redo through daemon APIs

---

## No-Go Conditions

Do **not** begin the TUI migration if any of these are still true:

- daemon mutations can still cross-client stomp through ambient focus
- benchmark backend is still too incomplete or under-verified for client truth
- hook/pact truth is still workflow-hardcoded in daemon C++
- cutover-critical event semantics are undefined
- live reconnect parity is not proven
- Windows transport behavior is still unverified in CI
- the TUI would still need direct `Harness`/`AgentRegistry`/`ProviderRegistry` reads for cutover-critical surfaces

---

## Green-Light Criteria

The TUI cutover can begin only when all of the following are true:

- daemon boundary is stable
- multi-client mutation isolation is proven
- transcript/tool/process/subagent/hook/pact truth is reconnect-safe
- benchmark backend is reconnect-safe and lifecycle-complete enough for client truth
- event contract is typed enough for client consumption
- cutover-critical surfaces are tested over the daemon boundary
- cross-platform transport is verified
- the team can point to this document and mark every `FAIL` item above as resolved

Until then, daemon work remains backend-first.
