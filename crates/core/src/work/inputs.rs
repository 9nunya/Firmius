//! Render a node's frozen [`InputManifest`] into text a worker can read.
//!
//! `freeze_manifest` records exactly which predecessor results an attempt
//! is entitled to, keyed by the alias its incoming edge declared. Until
//! this module existed, nothing ever read that selection back: a
//! synthesizer could be assigned a node whose manifest named ten worker
//! results and still be told nothing about them, so authored data flow
//! terminated in a dead end.
//!
//! Rendering is deliberately pure and bounded:
//!
//! - Pure, so it is testable without a session and cannot mutate the graph
//!   while an attempt is being launched.
//! - Bounded, because a fan-in of ten workers with verbose results would
//!   otherwise blow out the successor's context in a single prompt. Large
//!   values are replaced by the `artifact://` reference they were already
//!   stored under (or by an explicit truncation notice), so the worker can
//!   `read` what it needs instead of receiving everything inline.
//!
//! Aliases are rendered in the manifest's own (sorted) key order so the
//! same graph state always produces the same prompt prefix. That keeps the
//! text stable across attempts and lets sibling agents share a prompt
//! cache prefix rather than each paying for a differently-ordered one.

use super::model::*;

/// Longest inline rendering of one bound result's summary. Beyond this the
/// summary is truncated and the reader is pointed at the artifact or told
/// how much was elided, rather than silently losing the tail.
pub const MAX_INLINE_SUMMARY_BYTES: usize = 4096;

/// Longest inline rendering of one bound result's structured output.
pub const MAX_INLINE_OUTPUT_BYTES: usize = 4096;

/// Truncate on a character boundary, appending a notice that says exactly
/// how much was elided. Never silently drops the tail: a worker that sees
/// a truncated input must be able to tell that it was truncated.
fn truncate(value: &str, limit: usize) -> String {
    if value.len() <= limit {
        return value.to_string();
    }
    let mut end = limit;
    while end > 0 && !value.is_char_boundary(end) {
        end -= 1;
    }
    format!(
        "{}\n[... truncated {} more bytes]",
        &value[..end],
        value.len() - end
    )
}

/// Render one bound result under `alias`.
fn render_result(alias: &str, result: &NodeResult, producer_key: Option<&str>) -> String {
    let mut out = String::new();
    let origin = producer_key
        .map(|key| format!(" (from node `{key}`)"))
        .unwrap_or_default();
    out.push_str(&format!("### {alias}{origin}\n"));
    out.push_str(&format!(
        "status: {:?}",
        result.execution_status
    ));
    if let Some(outcome) = &result.outcome {
        out.push_str(&format!(", outcome: {outcome:?}"));
    }
    if result.verification != VerificationLevel::None {
        out.push_str(&format!(", verification: {:?}", result.verification));
    }
    out.push('\n');

    if !result.summary.is_empty() {
        out.push_str(&truncate(&result.summary, MAX_INLINE_SUMMARY_BYTES));
        out.push('\n');
    }

    if let Some(value) = &result.structured_output {
        let encoded = serde_json::to_string_pretty(value).unwrap_or_default();
        out.push_str("output:\n");
        out.push_str(&truncate(&encoded, MAX_INLINE_OUTPUT_BYTES));
        out.push('\n');
    }

    // Artifacts and changed files are listed as references, never inlined:
    // they are already addressable, and a worker can `read` the ones it
    // actually needs.
    if !result.artifacts.is_empty() {
        out.push_str(&format!("artifacts: {}\n", result.artifacts.join(", ")));
    }
    if !result.changed_files.is_empty() {
        out.push_str(&format!(
            "changed files: {}\n",
            result.changed_files.join(", ")
        ));
    }
    if !result.evidence.is_empty() {
        let evidence: Vec<String> = result
            .evidence
            .iter()
            .map(|e| truncate(e, 240))
            .collect();
        out.push_str(&format!("evidence: {}\n", evidence.join("; ")));
    }
    out
}

/// Render every result bound to `manifest`, or `None` when the node has no
/// bound inputs (a source node) so callers can skip the section entirely
/// rather than emit an empty heading.
pub fn render_manifest(graph: &WorkGraph, manifest: &InputManifest) -> Option<String> {
    if manifest.results.is_empty() {
        return None;
    }
    let mut sections = Vec::new();
    for (alias, result_id) in &manifest.results {
        let Some(result) = graph.results.get(result_id) else {
            // A manifest names exact result ids; a missing one means the
            // graph was mutated in a way that dropped history. Say so
            // rather than silently omitting an input the node expected.
            sections.push(format!(
                "### {alias}\n[result {result_id} is no longer present in the graph]\n"
            ));
            continue;
        };
        let producer_key = graph.nodes.get(&result.node_id).map(|n| n.key.as_str());
        sections.push(render_result(alias, result, producer_key));
    }
    Some(sections.join("\n"))
}

/// Assemble the full context a worker receives for one node: the graph's
/// shared brief, the node's bound predecessor results, and the node's own
/// task sheet, in that order.
///
/// Order is deliberate. Shared framing first, then the data this attempt
/// was given, then the specific instruction, so the most specific thing the
/// worker must act on is closest to where it starts writing.
pub fn compose_node_context(
    graph: &WorkGraph,
    node: &WorkNode,
    manifest: Option<&InputManifest>,
    task_prompt: &str,
) -> String {
    let mut parts = Vec::new();
    if let Some(brief) = graph.brief.as_deref().filter(|b| !b.trim().is_empty()) {
        parts.push(format!("## Shared brief\n\n{brief}"));
    }
    if let Some(rendered) = manifest.and_then(|m| render_manifest(graph, m)) {
        parts.push(format!(
            "## Inputs\n\nResults produced by this node's predecessors, \
             named by the alias its edges declared.\n\n{rendered}"
        ));
    }
    if !node.acceptance_criteria.is_empty() {
        let criteria = node
            .acceptance_criteria
            .iter()
            .map(|c| format!("- ({}) {}", c.id, c.text))
            .collect::<Vec<_>>()
            .join("\n");
        parts.push(format!(
            "## Acceptance criteria\n\nLink evidence to these ids when you settle.\n\n{criteria}"
        ));
    }
    parts.push(format!("## Your task\n\n{task_prompt}"));
    parts.join("\n\n")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::work::ids::{AttemptId, NodeId, ResultId};
    use chrono::Utc;

    fn graph_with_result(summary: &str, structured: Option<serde_json::Value>) -> (WorkGraph, NodeId)
    {
        let mut graph = WorkGraph::new("g", Some("owner".into()), GraphMode::Managed);
        let producer = WorkNode::new("w1", "worker one");
        let consumer = WorkNode::new("syn", "synthesizer");
        let (producer_id, consumer_id) = (producer.id, consumer.id);
        graph.view_order.extend([producer_id, consumer_id]);
        graph.nodes.insert(producer_id, producer);
        graph.nodes.insert(consumer_id, consumer);

        let attempt_id = AttemptId::new();
        let result_id = ResultId::new();
        graph.attempts.insert(
            attempt_id,
            NodeAttempt {
                id: attempt_id,
                node_id: producer_id,
                number: 1,
                state: ExecutionStatus::Succeeded,
                started_at: Some(Utc::now()),
                finished_at: Some(Utc::now()),
                agent_id: Some("worker".into()),
                assignment_id: None,
                result_id: Some(result_id),
                input_manifest_id: None,
            },
        );
        graph.results.insert(
            result_id,
            NodeResult {
                id: result_id,
                node_id: producer_id,
                attempt_id,
                execution_status: ExecutionStatus::Succeeded,
                outcome: Some(Outcome::Success),
                verification: VerificationLevel::SelfVerified,
                summary: summary.into(),
                structured_output: structured,
                artifacts: vec!["artifact://finding-1.md".into()],
                evidence: vec!["cargo test passed".into()],
                evidence_links: Vec::new(),
                changed_files: vec!["src/routes.rs".into()],
                producer: Some("worker".into()),
                created_at: Utc::now(),
            },
        );
        graph
            .nodes
            .get_mut(&producer_id)
            .unwrap()
            .attempt_ids
            .push(attempt_id);
        graph.nodes.get_mut(&producer_id).unwrap().status = ExecutionStatus::Succeeded;

        let edge_id = crate::work::ids::EdgeId::new();
        graph.edges.insert(
            edge_id,
            WorkEdge {
                id: edge_id,
                from: producer_id,
                to: consumer_id,
                condition: EdgeCondition::Succeeded,
                required: true,
                binding: Some(InputBinding {
                    alias: "finding_1".into(),
                    selection: ResultSelection { field: None },
                }),
            },
        );
        (graph, consumer_id)
    }

    /// The point of the whole exercise: a successor actually receives the
    /// predecessor result its edge bound, under the declared alias.
    #[test]
    fn bound_predecessor_results_reach_the_successor() {
        let (graph, consumer_id) = graph_with_result("found three issues", None);
        let manifest = graph.freeze_manifest(consumer_id);
        let rendered = render_manifest(&graph, &manifest).expect("inputs are rendered");
        assert!(rendered.contains("finding_1"), "{rendered}");
        assert!(rendered.contains("from node `w1`"), "{rendered}");
        assert!(rendered.contains("found three issues"), "{rendered}");
        // References are listed, not inlined.
        assert!(rendered.contains("artifact://finding-1.md"), "{rendered}");
        assert!(rendered.contains("src/routes.rs"), "{rendered}");
    }

    /// A source node has no bound inputs and must not receive an empty
    /// "Inputs" section.
    #[test]
    fn a_node_with_no_bound_inputs_renders_nothing() {
        let (graph, _) = graph_with_result("x", None);
        let producer = graph.nodes.values().find(|n| n.key == "w1").unwrap();
        let manifest = graph.freeze_manifest(producer.id);
        assert!(render_manifest(&graph, &manifest).is_none());
    }

    /// Structured output is rendered so a successor can consume a typed
    /// handoff rather than re-parsing prose.
    #[test]
    fn structured_output_is_rendered() {
        let (graph, consumer_id) =
            graph_with_result("done", Some(serde_json::json!({"issues": 3})));
        let manifest = graph.freeze_manifest(consumer_id);
        let rendered = render_manifest(&graph, &manifest).unwrap();
        assert!(rendered.contains("\"issues\": 3"), "{rendered}");
    }

    /// A verbose predecessor must not blow out its successor's context.
    /// Truncation is explicit so the reader knows the tail exists.
    #[test]
    fn oversized_values_are_truncated_with_a_notice() {
        let huge = "x".repeat(MAX_INLINE_SUMMARY_BYTES * 3);
        let (graph, consumer_id) = graph_with_result(&huge, None);
        let manifest = graph.freeze_manifest(consumer_id);
        let rendered = render_manifest(&graph, &manifest).unwrap();
        assert!(rendered.len() < huge.len(), "oversized input was inlined whole");
        assert!(rendered.contains("truncated"), "{rendered}");
    }

    /// The composed context carries shared framing, the bound inputs, and
    /// the node's own task sheet, in that order.
    #[test]
    fn composed_context_orders_brief_inputs_then_task() {
        let (mut graph, consumer_id) = graph_with_result("found three issues", None);
        graph.brief = Some("Repo conventions apply.".into());
        let manifest = graph.freeze_manifest(consumer_id);
        let node = graph.nodes[&consumer_id].clone();
        let composed = compose_node_context(&graph, &node, Some(&manifest), "Merge the findings.");

        let brief_at = composed.find("Shared brief").expect("brief present");
        let inputs_at = composed.find("## Inputs").expect("inputs present");
        let task_at = composed.find("Your task").expect("task present");
        assert!(brief_at < inputs_at && inputs_at < task_at, "{composed}");
        assert!(composed.contains("Merge the findings."));
        assert!(composed.contains("found three issues"));
    }

    /// A node with no brief and no inputs still gets a clean prompt rather
    /// than stray empty headings.
    #[test]
    fn composed_context_omits_absent_sections() {
        let (graph, _) = graph_with_result("x", None);
        let producer = graph.nodes.values().find(|n| n.key == "w1").unwrap().clone();
        let composed = compose_node_context(&graph, &producer, None, "Do the thing.");
        assert!(!composed.contains("Shared brief"));
        assert!(!composed.contains("## Inputs"));
        assert!(composed.starts_with("## Your task"));
    }
}
