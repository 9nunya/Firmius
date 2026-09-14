# Firmius Desktop

The desktop client is a native Slint workspace over the Firmius daemon. The
daemon remains the authority for sessions, authentication, and state; the
desktop renders and controls that durable state directly.

The proposed next UI architecture, design language, base component library,
and implementation gates are specified in
[Desktop design](../../docs/desktop-design/README.md). That specification
distinguishes the planned redesign from the current implementation below.

Run it from the repository with:

```sh
cargo run -p firmius-desktop
```

The desktop provides durable session creation and discovery, multiple open
session tabs, a typed conversation/runtime/changes/work projection, streamed
tool and reasoning cards, agent status, turn submission/cancellation, save,
compaction, turn rewind, and per-agent edit undo/redo. The active session is
kept current from the daemon event bus and reconciled from authoritative
snapshots after live events or reconnects.

Messages sent while the focused agent is running are queued through the same
daemon path as the TUI. Session titles can be renamed from the header, and the
current attached session can be exported as Markdown without leaving the app.

The command center exposes live MCP inventory with add (stdio or HTTP),
start/stop/restart/remove controls, plus a goal creation sheet and durable
goal inspection/lifecycle controls.
Permission requests arrive from the daemon event stream as an interruptive
review card; the Allow/Deny decision carries the daemon-issued nonce, digest,
revision, session, agent, and tool identifiers back to the service.

Prompt workflow discovery searches the same project and global roots as the
TUI. The selected file can be inserted into the composer or run immediately;
the normal submit/queue path handles its execution. SSH host, saved remote
workspace, account, and settings inventories are available as inspectable
desktop panels; the SSH workspace sheet can save an alias or create a remote
session directly.

The first-run onboarding sheet explains the daemon-backed model, and theme
choices persist through `UserSettings`. Login accepts a configured provider
schema and stores the resulting API-key record through the shared account
store, so credentials do not enter session snapshots or transcript rendering.

The model control applies a provider/model/effort to the focused session
agent only after the daemon acknowledges it, then persists the confirmed
default locally. A new session accepts either a local workspace or an
`ssh://host/absolute/path` workspace. The app uses the same endpoint token
and typed protocol as the TUI, so it never reads or copies provider
credentials into the UI.

The model picker includes configured provider models, context windows, and
effort metadata; selecting a catalog row fills the focused-agent model form
and exposes that model's supported effort modes before applying the change.
The command center also exposes daemon-backed persona and effort controls for
the focused agent, including clearing a previously assigned persona and
choosing from the selected model's supported reasoning modes.

Hosted search has a dedicated mode sheet for off, cached, indexed, and live
configurations. Changes are sent through `UpdateConfig`, persisted locally,
and still pass through provider capability and permission checks at runtime.

Copy actions distinguish the last assistant reply from the complete visible
transcript. The custom Slint controls animate hover, press, and expandable
tool-card geometry, while saved SSH targets are selectable directly in the
remote workspace sheet.

Tool cards retain call and result IDs, show parsed JSON arguments when
available, distinguish running/completed/failed states, and keep live tool
events separate from durable assistant text.
Delegate, task, workflow, goal, MCP, bash, and edit calls receive operation-
specific summaries and metadata rather than a generic JSON-only presentation.

Assistant Markdown is normalized into visible heading, list, quote, link, and
code-block treatments before it reaches the transcript model, and edit cards
use the same expandable interaction for unified diff previews.

The Work inspector shows dependency edges, required versus optional routing,
conditional outcomes, acceptance criteria, tracked file scope, attempts,
result summaries, outcomes, and verification levels for each durable graph.

The workflow builder accepts a title, shared brief, and ordered step list. It
submits a generated workflow-tool prompt through the focused agent, preserving
the daemon's normal graph creation, permission, and queue behavior.

## Shell and model boundaries

The shell is defined in `src/shell.rs`, independently of Slint. A `Route`
identifies a session, optional agent, and view. A tab has its own ID, route,
draft, and expansion state. Each viewport owns its tab selection; the shell
owns focus. Opening an agent or a view creates/selects a route in the focused
viewport. Split duplicates that route into another viewport. A session can
therefore appear in several tabs or viewports without sharing local focus.
Navigation reads the route directly; there is no parallel active-session model.

`desktop/shell_ui.rs` projects routes into `ViewportRow`. The reusable
`ui/shell/viewport.slint` renders those rows. Settings is a viewport feature,
while dialogs live in `ui/views/`. Basic structural/control roles are defined
in `ui/shell/tokens.slint`. This is an intentionally plain functional shell.

To add a document/list feature, publish portable rows through `show_document`;
the shell supplies a tab, viewport, focus and local state. For an interactive
feature, add its route and a component accepting explicit inputs and emitting
callbacks, following `SettingsView`. Do not mutate the active transcript from
a feature or create transport connections in a presenter.

`daemon.rs` owns an application-lifetime Tokio runtime and a connection per
session. The catalog connection is separate. Session sockets never retarget
when focus changes. `desktop/connection.rs` subscribes before reconciliation,
consumes every delivered event, requests replay by sequence, and coalesces
visual updates. All open sessions are monitored, including background tabs.
Snapshot reconciliation refreshes durable history and external process state.
Daemon epoch changes invalidate sequence watermarks.

`session_model.rs` retains received events and folds tool identity from the
first preparing delta through invocation completion. IDs arriving later promote
the existing call; provider indices are scoped to generations. Tool runtime
resources are separate typed process/delegate records, so a completed spawn
call can still have running work. Their source events are emitted by the tools;
latest resource state survives daemon journal eviction. Presenters select by
actual tool name and never recover identity from display labels or output text.

The daemon allows concurrent viewers of a session. One connection at a time
owns interactive permission approval, and other viewers cannot steal that
ownership by attaching. Detaching/disconnecting a viewer does not interrupt
other clients or stop session execution.

The daemon replay journal is bounded and process-local. The desktop explicitly
reports an unavailable delta range and uses snapshots to restore current state;
it does not claim to recover evicted deltas. Runtime resources do not survive
a daemon restart. Rebuild both desktop and daemon for the added lifecycle and
replay protocol variants, then restart the daemon when existing work is idle.

Validation:

```sh
cargo test -p firmius-desktop -p firmius-client -p firmius-service --offline
cargo test -p firmius-core --test tools --offline
cargo run -p firmius-desktop -- --preview=/tmp/firmius-shell.ppm
```
