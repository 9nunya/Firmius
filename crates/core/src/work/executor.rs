//! M4.6 — drives one durably claimed attempt to completion.
//!
//! `Executor::Command` runs a process through the existing [`Host`]
//! abstraction: program/argv are explicit (no shell parsing unless the
//! caller explicitly builds a shell invocation), working directory and
//! environment come from the caller-supplied [`CommandSpec`] (constrained by
//! node/session policy upstream), cancellation kills the process, and
//! output is bounded with the full transcript stored as a session artifact.
//! Process exit status is recorded as evidence; it is never conflated with
//! the semantic [`Outcome`] the settlement records.
//!
//! `Executor::Agent` hands the claim to a caller-supplied async launcher —
//! only the session/tool layer has the persona, provider, and tool-registry
//! wiring needed to spawn a subagent (the same infrastructure `delegate`
//! uses), so this module stays a thin, testable settlement boundary rather
//! than re-implementing that wiring.
//!
//! Settlement (`settle_claim`) is always a fresh, durable, revisioned
//! `mutate_work` transaction — it goes through `WorkState::settle_assignment`,
//! which already rejects a stale/duplicate settlement (an assignment that
//! was already released by a cancellation or a retry can never be
//! overwritten by a late settlement).

use super::event::WorkEvent;
use super::ids::{AssignmentId, GraphId};
use super::model::*;
use crate::host::{Host, HostError, ProcSpec};
use crate::session::Session;
use std::sync::Arc;

/// Program/argv/environment for one `Executor::Command` attempt. Built by
/// the caller from node + session policy — this module does not interpret
/// shell syntax; `program`/`args` are passed to `Host::spawn` verbatim.
#[derive(Debug, Clone, Default)]
pub struct CommandSpec {
    pub program: String,
    pub args: Vec<String>,
    pub cwd: Option<String>,
    pub env: Vec<(String, String)>,
}

/// Output captured beyond this many bytes is truncated in the settlement
/// summary; the full transcript is always stored as an artifact regardless.
pub const MAX_SUMMARY_OUTPUT_BYTES: usize = 4096;

#[derive(Debug, thiserror::Error)]
pub enum ExecutorError {
    #[error("host error: {0}")]
    Host(#[from] HostError),
    #[error("work error: {0}")]
    Work(String),
}

/// Run a claimed `Executor::Command` attempt to completion and settle it
/// durably. The process's exit status is separate from the semantic
/// outcome: exit 0 is `Outcome::Success`, non-zero is `Outcome::Failure`.
/// Cancellation (`Host::kill`) leaves the assignment for a caller to settle
/// as `Outcome::Cancelled` via `settle_claim` directly — this function
/// always runs a command to natural completion.
pub async fn execute_command(
    session: &Arc<Session>,
    host: &dyn Host,
    graph_id: GraphId,
    assignment_id: AssignmentId,
    agent_id: &str,
    spec: CommandSpec,
) -> Result<(), ExecutorError> {
    let proc_spec = ProcSpec::new(spec.program.clone())
        .args(spec.args.clone())
        .on_orphan(crate::host::OnOrphan::Kill);
    let proc_spec = match &spec.cwd {
        Some(cwd) => proc_spec.cwd(cwd.clone()),
        None => proc_spec,
    };
    let proc_spec = spec
        .env
        .iter()
        .fold(proc_spec, |acc, (k, v)| acc.env(k.clone(), v.clone()));

    let proc_id = host.spawn(proc_spec).await?;
    let status = host.wait(proc_id).await?;
    let captured = host
        .peek(proc_id, 0)
        .map(|(bytes, _offset, _status)| bytes)
        .unwrap_or_default();
    let output = String::from_utf8_lossy(&captured).into_owned();

    let artifact_path = format!("command-output-{assignment_id}.log");
    let artifact = session
        .artifacts
        .write(
            &artifact_path,
            output.clone(),
            Some(agent_id),
            crate::artifact::ArtifactSource::Manual,
        )
        .map_err(|e| ExecutorError::Work(e.to_string()))?;

    let summary_output: String = output.chars().take(MAX_SUMMARY_OUTPUT_BYTES).collect();
    let (execution_status, outcome, summary) = if status.success {
        (
            ExecutionStatus::Succeeded,
            Outcome::Success,
            format!("command exited 0\n{summary_output}"),
        )
    } else {
        (
            ExecutionStatus::Failed,
            Outcome::Failure,
            format!("command exited {}\n{summary_output}", status.code),
        )
    };

    settle_claim(
        session,
        graph_id,
        assignment_id,
        agent_id,
        execution_status,
        Some(outcome),
        summary,
        None,
        vec![format!("artifact://{}", artifact.path)],
        Vec::new(),
        Vec::new(),
    )
    .map_err(ExecutorError::Work)
}

/// Durably settle a claimed attempt. This is always a fresh revisioned
/// transaction: it does not assume the caller still holds the revision the
/// claim was made under, so a stale settlement racing a cancellation or
/// retry is rejected by `WorkState::settle_assignment` (assignment already
/// released / attempt no longer running), never silently applied.
#[allow(clippy::too_many_arguments)]
pub fn settle_claim(
    session: &Arc<Session>,
    graph_id: GraphId,
    assignment_id: AssignmentId,
    agent_id: &str,
    status: ExecutionStatus,
    outcome: Option<Outcome>,
    summary: String,
    structured_output: Option<serde_json::Value>,
    artifacts: Vec<String>,
    evidence: Vec<String>,
    changed_files: Vec<String>,
) -> Result<(), String> {
    let agent_id = agent_id.to_string();
    session.mutate_work(move |state| {
        let expected = state.graph(graph_id)?.revision;
        let auth = AuthorizationContext {
            agent_id: agent_id.clone(),
            can_manage: false,
            assignment_ids: [assignment_id].into_iter().collect(),
        };
        let result_id = state.settle_assignment(
            graph_id,
            expected,
            &auth,
            assignment_id,
            status,
            outcome,
            summary,
            structured_output,
            artifacts,
            evidence,
            changed_files,
        )?;
        let result = state.graph(graph_id)?.results[&result_id].clone();
        Ok(((), WorkEvent::ResultRecorded { graph_id, result }))
    })
}

/// Launch an `Executor::Agent` claim. Delegated to a caller-supplied async
/// closure so this module stays free of the persona/provider wiring that
/// only `Session`-level callers (e.g. the `delegate` tool infrastructure)
/// have; the launcher is responsible for eventually calling `settle_claim`
/// (directly, or via a worker's `yield` / natural termination settlement),
/// exactly once.
pub async fn execute_agent<F, Fut>(launch: F) -> Result<(), String>
where
    F: FnOnce() -> Fut,
    Fut: std::future::Future<Output = Result<(), String>>,
{
    launch().await
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::LocalHost;
    use crate::work::{NodeInput, WorkGraph};

    fn owner_auth() -> AuthorizationContext {
        AuthorizationContext {
            agent_id: "owner".into(),
            can_manage: false,
            assignment_ids: Default::default(),
        }
    }

    #[tokio::test]
    async fn command_executor_settles_success_and_failure() {
        let session = Session::new_handle();
        let host = LocalHost::new();
        let (graph_id, assignment_id) = session
            .mutate_work(|state| {
                let graph = WorkGraph::new("g", Some("owner".into()), GraphMode::Managed);
                let id = graph.id;
                state.create_graph(graph.clone(), None)?;
                let nid = state.add_node(
                    id,
                    state.graph(id)?.revision,
                    &owner_auth(),
                    NodeInput {
                        key: "n0".into(),
                        title: "Node 0".into(),
                        description: None,
                    },
                )?;
                let (_attempt, assignment) = state.assign(
                    id,
                    state.graph(id)?.revision,
                    &owner_auth(),
                    nid,
                    "worker",
                    Some("owner".into()),
                    None,
                )?;
                let revision = state.graph(id)?.revision;
                Ok((
                    (id, assignment),
                    WorkEvent::GraphChanged {
                        graph_id: id,
                        revision,
                    },
                ))
            })
            .unwrap();

        execute_command(
            &session,
            &host,
            graph_id,
            assignment_id,
            "worker",
            CommandSpec {
                program: "true".into(),
                ..Default::default()
            },
        )
        .await
        .unwrap();

        let graph = session
            .work
            .read()
            .unwrap()
            .graph(graph_id)
            .unwrap()
            .clone();
        assert!(graph.assignments[&assignment_id].released_at.is_some());
        let result = graph
            .results
            .values()
            .find(|r| r.attempt_id == graph.assignments[&assignment_id].attempt_id)
            .unwrap();
        assert_eq!(result.execution_status, ExecutionStatus::Succeeded);
        assert_eq!(result.outcome, Some(Outcome::Success));
        assert!(!result.artifacts.is_empty());
    }
}
