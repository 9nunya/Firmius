//! Live rendering of a structured run.
//!
//! The compact checklist ([`super::work::WorkView`]) answers "what is next on
//! the list". A run needs a different question answered: what is executing
//! right now, what is it waiting on, and is it moving. Ten workers in flight
//! collapse into an overflow count in the checklist view, which is precisely
//! when the user most wants to see the shape of the work.
//!
//! Two ideas make this feel live rather than like a status table that
//! refreshes twice a minute:
//!
//! - **Elapsed time per running node**, ticking on the existing frame clock.
//!   Node settlements are minutes apart, so without a continuously moving
//!   signal a run looks frozen even while it is healthy.
//! - **Per-node activity**, correlated from the agent event stream through
//!   the assignment that binds an agent to a node. A row reads
//!   `auditing slice 3 · reading src/routes.rs` rather than merely
//!   `running`, so the view answers "is this stuck?" without drilling in.
//!
//! Progress itself is always derived from the canonical graph, never
//! accumulated here, so the view cannot drift from durable state; the only
//! things this module owns are presentation-local (when a node was first
//! seen running, and the last activity line observed for an agent).

use std::collections::HashMap;
use std::time::Instant;

use firmius_core::GraphId;
use firmius_core::work::{LiveGraph, LiveNode, LiveState};

/// Presentation-local liveness that has no place in durable state.
#[derive(Debug, Default)]
pub struct RunLiveness {
    /// When each node was first observed running, for elapsed time.
    ///
    /// Attempt-scoped: a retried node restarts its clock, because the
    /// interesting number is how long the current attempt has been going,
    /// not the total across attempts.
    started: HashMap<(String, u32), Instant>,
    /// Last observed activity per agent, e.g. the tool it just invoked.
    activity: HashMap<String, String>,
    /// The run currently in flight, if any.
    active_run: Option<(String, GraphId)>,
}

impl RunLiveness {
    /// Note that a run began, so the view can expand while it is live.
    pub fn run_started(&mut self, run_id: String, graph_id: GraphId) {
        self.active_run = Some((run_id, graph_id));
    }

    pub fn run_concluded(&mut self, run_id: &str) {
        if self
            .active_run
            .as_ref()
            .is_some_and(|(active, _)| active == run_id)
        {
            self.active_run = None;
        }
        // Elapsed timers are meaningless once nothing is running, and
        // holding them would leak a little memory per node forever.
        self.started.clear();
        self.activity.clear();
    }

    pub fn is_running(&self) -> bool {
        self.active_run.is_some()
    }

    pub fn graph_id(&self) -> Option<GraphId> {
        self.active_run.as_ref().map(|(_, graph_id)| *graph_id)
    }

    /// Record what an agent is doing right now. Called from the agent event
    /// stream; the agent is mapped to a node by its assignment at render
    /// time, so this stays a simple per-agent map.
    pub fn note_activity(&mut self, agent_id: &str, activity: impl Into<String>) {
        self.activity.insert(agent_id.to_string(), activity.into());
    }

    pub fn clear_activity(&mut self, agent_id: &str) {
        self.activity.remove(agent_id);
    }

    /// Start (or continue) the clock for a running node, and drop clocks for
    /// nodes that are no longer running.
    pub fn sync(&mut self, live: &LiveGraph) {
        let now = Instant::now();
        let mut seen = Vec::new();
        for node in live.running() {
            let key = (node.key.clone(), node.attempt);
            self.started.entry(key.clone()).or_insert(now);
            seen.push(key);
        }
        self.started.retain(|key, _| seen.contains(key));
    }

    fn elapsed(&self, node: &LiveNode) -> Option<std::time::Duration> {
        self.started
            .get(&(node.key.clone(), node.attempt))
            .map(|start| start.elapsed())
    }

    fn activity_for(&self, node: &LiveNode) -> Option<&str> {
        node.agent_id
            .as_deref()
            .and_then(|agent| self.activity.get(agent))
            .map(|s| s.as_str())
    }
}

/// Format a duration the way a person reads a stopwatch.
pub fn format_elapsed(elapsed: std::time::Duration) -> String {
    let secs = elapsed.as_secs();
    if secs < 60 {
        format!("{secs}s")
    } else {
        format!("{}m{:02}s", secs / 60, secs % 60)
    }
}

/// One rendered row.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RunRow {
    pub indent: usize,
    pub glyph: &'static str,
    pub state: LiveState,
    pub text: String,
    /// Dim trailing detail: elapsed, activity, or why a node is waiting.
    pub detail: Option<String>,
}

pub fn state_glyph(state: LiveState) -> &'static str {
    match state {
        LiveState::Waiting => "○",
        LiveState::Running => "◐",
        LiveState::Succeeded => "●",
        LiveState::Failed => "✗",
        LiveState::Stuck => "⊘",
    }
}

/// Render a stage's members as a dot cluster: `●●●◐○○`.
///
/// This is what keeps a wide fan-out legible. Ten workers as ten rows is
/// noise that pushes the transcript off screen; ten dots is one glance.
/// Wide stages are summarized numerically instead, since a hundred dots
/// conveys no more than a count.
fn dots(nodes: &[LiveNode]) -> String {
    const MAX_DOTS: usize = 24;
    if nodes.len() > MAX_DOTS {
        return String::new();
    }
    nodes.iter().map(|n| state_glyph(n.state)).collect()
}

/// Build the rows for a live run.
///
/// `budget` is the number of lines available. Rows are chosen by attention
/// rather than by authored order: running work first, then whatever explains
/// why the rest is not running. A view that truncates to "the first N nodes"
/// would reliably hide the only rows that matter in a wide run.
pub fn rows(live: &LiveGraph, liveness: &RunLiveness, budget: usize) -> Vec<RunRow> {
    if budget == 0 {
        return Vec::new();
    }
    let mut rows = Vec::new();
    let counts = live.counts();

    // Header: overall progress, so the run's shape is legible in one line
    // even when every other row is cut for space.
    let total = counts.total();
    let done = counts.settled();
    let mut header = format!("{} · {done}/{total}", live.title);
    if counts.running > 0 {
        header.push_str(&format!(" · {} running", counts.running));
    }
    if counts.failed > 0 {
        header.push_str(&format!(" · {} failed", counts.failed));
    }
    rows.push(RunRow {
        indent: 0,
        glyph: if counts.running > 0 { "▸" } else { "▪" },
        state: if counts.failed > 0 {
            LiveState::Failed
        } else if counts.running > 0 {
            LiveState::Running
        } else if counts.waiting > 0 {
            LiveState::Waiting
        } else {
            LiveState::Succeeded
        },
        text: header,
        detail: None,
    });

    // Running nodes are the reason to be watching, so they are never cut.
    let running: Vec<&LiveNode> = live.running().collect();
    for node in &running {
        if rows.len() >= budget {
            return rows;
        }
        let mut detail = Vec::new();
        if let Some(elapsed) = liveness.elapsed(node) {
            detail.push(format_elapsed(elapsed));
        }
        if let Some(activity) = liveness.activity_for(node) {
            detail.push(activity.to_string());
        }
        if node.attempt > 1 {
            detail.push(match node.max_attempts {
                Some(max) => format!("attempt {}/{max}", node.attempt),
                None => format!("attempt {}", node.attempt),
            });
        }
        rows.push(RunRow {
            indent: 1,
            glyph: state_glyph(LiveState::Running),
            state: LiveState::Running,
            text: node.title.clone(),
            detail: (!detail.is_empty()).then(|| detail.join(" · ")),
        });
    }

    // Then the stages that are not fully done, so the user can see what is
    // still ahead and what is holding it up.
    for stage in &live.stages {
        if rows.len() >= budget {
            break;
        }
        let stage_counts = stage.counts();
        // A stage whose work is entirely finished, or entirely shown above
        // as running rows, adds nothing.
        if stage_counts.settled() == stage_counts.total() && stage_counts.total() > 0 {
            continue;
        }
        if stage_counts.running == stage_counts.total() && stage_counts.total() > 0 {
            continue;
        }
        let cluster = dots(&stage.nodes);
        let mut text = stage.label();
        if !cluster.is_empty() && stage.nodes.len() > 1 {
            text = format!("{cluster}  {text}");
        }
        // Explain the wait. With one node, quote its own reason; with many,
        // summarize, since ten copies of "waiting on w3" is not information.
        let detail = if stage.nodes.len() == 1 {
            stage.nodes[0].detail.clone()
        } else if stage_counts.waiting > 0 {
            Some(format!(
                "{}/{} done",
                stage_counts.settled(),
                stage_counts.total()
            ))
        } else {
            None
        };
        rows.push(RunRow {
            indent: 1,
            glyph: if stage_counts.stuck > 0 {
                state_glyph(LiveState::Stuck)
            } else {
                state_glyph(LiveState::Waiting)
            },
            state: if stage_counts.stuck > 0 {
                LiveState::Stuck
            } else {
                LiveState::Waiting
            },
            text,
            detail,
        });
    }
    rows.truncate(budget);
    rows
}

/// A short, human phrase for what an agent just did, derived from its event
/// stream. `None` for events that say nothing a viewer would want on a row.
///
/// Deliberately terse: this renders inline next to a node title, so it must
/// read as a status, not as a transcript.
pub fn activity_phrase(event: &firmius_core::AgentEvent) -> Option<String> {
    use firmius_core::AgentEvent;
    match event {
        AgentEvent::ToolCallStarted { name, args, .. } => Some(tool_phrase(name, args)),
        AgentEvent::Thinking(_) => Some("thinking".into()),
        // Text is the agent writing its answer: meaningful progress, but its
        // content belongs in the transcript, not on a one-line status row.
        AgentEvent::Text(_) => Some("writing".into()),
        AgentEvent::WebSearchStarted { .. } => Some("searching".into()),
        AgentEvent::WebSearchFinished { .. } => Some("searched".into()),
        _ => None,
    }
}

/// Name the target of a tool call when it is cheap and useful to do so.
///
/// Uses the same partial-JSON reader the transcript uses, so a row can fill
/// in while arguments are still streaming rather than waiting for the call
/// to be complete.
fn tool_phrase(name: &str, args: &str) -> String {
    let parsed = firmius_core::partial_json::PartialJson::parse(args);
    let target = parsed
        .complete_str("path")
        .or_else(|| parsed.complete_str("file_path"))
        .or_else(|| parsed.complete_str("pattern"))
        .or_else(|| parsed.complete_str("command"))
        .or_else(|| parsed.complete_str("intent"));
    match target {
        Some(value) => {
            let value = shorten(value, 42);
            format!("{name} {value}")
        }
        None => name.to_string(),
    }
}

/// Keep the tail of an over-long value: for a path, the end identifies the
/// file, while the leading directories are usually noise.
fn shorten(value: &str, max: usize) -> String {
    let value = value.trim();
    if value.chars().count() <= max {
        return value.to_string();
    }
    let tail: String = value
        .chars()
        .skip(value.chars().count().saturating_sub(max - 1))
        .collect();
    format!("…{tail}")
}

#[cfg(test)]
mod tests {
    use super::*;
    use firmius_core::work::{LiveStage, project_live};
    use firmius_core::{
        AuthorizationContext, GraphMode, PlannedEdge, PlannedNode, WorkGraph, WorkState,
    };

    fn auth() -> AuthorizationContext {
        AuthorizationContext {
            agent_id: "owner".into(),
            ..Default::default()
        }
    }

    fn fan_in(workers: usize) -> (WorkState, firmius_core::GraphId) {
        let mut state = WorkState::default();
        let graph = WorkGraph::new("audit routes", Some("owner".into()), GraphMode::Managed);
        let graph_id = graph.id;
        state.create_graph(graph, None).unwrap();
        let mut nodes: Vec<PlannedNode> = (1..=workers)
            .map(|i| PlannedNode {
                key: format!("w{i}"),
                title: format!("audit slice {i}"),
                ..Default::default()
            })
            .collect();
        nodes.push(PlannedNode {
            key: "syn".into(),
            title: "synthesize".into(),
            ..Default::default()
        });
        let edges: Vec<PlannedEdge> = (1..=workers)
            .map(|i| PlannedEdge {
                from: format!("w{i}"),
                to: "syn".into(),
                ..Default::default()
            })
            .collect();
        state
            .plan(graph_id, 0, &auth(), nodes, edges, None, None)
            .unwrap();
        (state, graph_id)
    }

    /// A wide fan-out must stay legible: the header carries overall
    /// progress and the stage collapses to a dot cluster rather than
    /// spending ten lines.
    #[test]
    fn a_wide_stage_renders_as_a_dot_cluster() {
        let (state, graph_id) = fan_in(10);
        let live = project_live(state.graph(graph_id).unwrap());
        let rows = rows(&live, &RunLiveness::default(), 10);
        assert!(
            rows[0].text.starts_with("audit routes · 0/11"),
            "{:?}",
            rows[0]
        );
        let cluster = rows
            .iter()
            .find(|row| row.text.contains("parallel tasks"))
            .expect("the wide stage is rendered");
        assert!(cluster.text.starts_with("○○○○○○○○○○"), "{cluster:?}");
    }

    /// Running work is never cut for space: it is the reason to be looking.
    #[test]
    fn running_nodes_survive_a_tight_budget() {
        let (mut state, graph_id) = fan_in(10);
        let ids: Vec<firmius_core::NodeId> = state.graph(graph_id).unwrap().view_order.clone();
        for id in ids.iter().take(3) {
            let revision = state.graph(graph_id).unwrap().revision;
            state
                .assign(graph_id, revision, &auth(), *id, "worker", None, None)
                .unwrap();
            // Each assign needs a distinct agent; release for the next one.
            let revision = state.graph(graph_id).unwrap().revision;
            let _ = revision;
            break;
        }
        let live = project_live(state.graph(graph_id).unwrap());
        let rows = rows(&live, &RunLiveness::default(), 2);
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[1].state, LiveState::Running);
    }

    /// The signals that make a run feel alive: elapsed time and what the
    /// node's agent is doing right now.
    #[test]
    fn a_running_row_shows_elapsed_and_activity() {
        let (mut state, graph_id) = fan_in(2);
        let id = state.graph(graph_id).unwrap().view_order[0];
        let revision = state.graph(graph_id).unwrap().revision;
        state
            .assign(graph_id, revision, &auth(), id, "worker-a", None, None)
            .unwrap();

        let live = project_live(state.graph(graph_id).unwrap());
        let mut liveness = RunLiveness::default();
        liveness.sync(&live);
        liveness.note_activity("worker-a", "read src/routes.rs");

        let rows = rows(&live, &liveness, 8);
        let running = rows
            .iter()
            .find(|row| row.state == LiveState::Running && row.indent == 1)
            .expect("the running node is rendered");
        let detail = running.detail.as_deref().unwrap_or_default();
        assert!(detail.contains('s'), "elapsed is shown: {detail}");
        assert!(detail.contains("read src/routes.rs"), "{detail}");
    }

    /// A waiting node must say what it is waiting for, otherwise the view
    /// cannot answer the only question worth asking about it.
    #[test]
    fn a_waiting_successor_explains_itself() {
        let (state, graph_id) = fan_in(3);
        let live = project_live(state.graph(graph_id).unwrap());
        let rows = rows(&live, &RunLiveness::default(), 8);
        let successor = rows
            .iter()
            .find(|row| row.text.contains("synthesize"))
            .expect("the successor is rendered");
        assert_eq!(
            successor.detail.as_deref(),
            Some("waiting on 3 tasks"),
            "{successor:?}"
        );
    }

    /// A retried node reads as progress, not as inexplicable repetition.
    #[test]
    fn a_retry_shows_its_attempt_number() {
        let mut live = LiveGraph {
            title: "loop".into(),
            structured: true,
            stages: vec![LiveStage {
                depth: 0,
                nodes: vec![LiveNode {
                    node_id: firmius_core::NodeId::new(),
                    key: "code".into(),
                    title: "implement".into(),
                    state: LiveState::Running,
                    attempt: 2,
                    max_attempts: Some(3),
                    agent_id: Some("worker".into()),
                    detail: None,
                    summary: None,
                }],
            }],
        };
        let mut liveness = RunLiveness::default();
        liveness.sync(&live);
        let rows = rows(&live, &liveness, 8);
        let running = rows.iter().find(|row| row.indent == 1).unwrap();
        assert!(
            running
                .detail
                .as_deref()
                .unwrap_or_default()
                .contains("attempt 2/3"),
            "{running:?}"
        );
        // And once it settles, the run reads as finished.
        live.stages[0].nodes[0].state = LiveState::Succeeded;
        assert!(live.is_finished());
    }

    /// Tool activity names its target so a row says what is happening, and
    /// fills in while arguments are still streaming.
    #[test]
    fn tool_activity_names_its_target() {
        assert_eq!(
            tool_phrase("read", r#"{"path": "src/main.rs"}"#),
            "read src/main.rs"
        );
        assert_eq!(tool_phrase("bash", "{\"comm"), "bash");
        let long = tool_phrase(
            "read",
            r#"{"path": "a/very/deeply/nested/directory/structure/that/keeps/going/file.rs"}"#,
        );
        assert!(long.starts_with("read …"), "{long}");
        assert!(long.ends_with("file.rs"), "{long}");
    }

    /// Liveness is presentation-local and must not outlive the run.
    #[test]
    fn concluding_a_run_clears_liveness() {
        let mut liveness = RunLiveness::default();
        liveness.run_started("run-1".into(), firmius_core::GraphId::new());
        liveness.note_activity("worker", "doing a thing");
        assert!(liveness.is_running());
        liveness.run_concluded("run-1");
        assert!(!liveness.is_running());
        assert!(liveness.activity.is_empty());
    }
}
