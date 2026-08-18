//! Pure, generation-checked conversation compaction.
//!
//! Compaction operates on complete segments.  Segments are never split or
//! renamed, and a summary is projection metadata (not a message in the
//! provider context).

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashSet;

use crate::types::{Message, MessagePart, MessageRole, validate_context};

pub type Generation = u64;

/// The summary is projected into the provider's system context.  Keep an
/// explicit envelope around it: some providers flatten all system messages
/// into one instruction string, where an unlabelled summary can otherwise be
/// mistaken for (or run into) the configured system/persona prompt.
pub(crate) fn format_summary(summary: &str) -> String {
    format!("<compaction_summary>\n{summary}\n</compaction_summary>")
}

/// Parse the canonical persisted/provider summary envelope.
///
/// Summaries on disk must remain distinguishable from ordinary system text;
/// callers that accept provider output may normalize the returned contents
/// with [`format_summary`].
pub(crate) fn parse_summary(summary: &str) -> Option<&str> {
    summary
        .strip_prefix("<compaction_summary>\n")
        .and_then(|value| value.strip_suffix("\n</compaction_summary>"))
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TimelineEntry {
    pub turn: u64,
    pub message: Message,
}

/// Match the provider contract: tool results must be the immediately
/// following tool message for the preceding assistant call.  The generic
/// context validator supplies the adjacency rule; these checks close the
/// role loophole for tool-shaped parts in ordinary messages.
fn valid_provider_roles(timeline: &Timeline) -> bool {
    let messages: Vec<_> = timeline
        .entries()
        .map(|entry| entry.message.clone())
        .collect();
    if validate_context(&messages).is_err() {
        return false;
    }

    messages.iter().all(|message| {
        message.content.iter().all(|part| match part {
            MessagePart::ToolCall { .. } => message.role == MessageRole::Assistant,
            MessagePart::ToolResult { .. } => message.role == MessageRole::Tool,
            _ => true,
        })
    })
}

fn digest_segments(segments: &[TimelineSegment]) -> String {
    let bytes = serde_json::to_vec(segments).expect("timeline segments are serializable");
    format!("{:x}", Sha256::digest(bytes))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{MessagePart, MessageRole};

    fn text(role: MessageRole, value: &str) -> TimelineEntry {
        TimelineEntry::new(
            1,
            Message::with_parts(role, vec![MessagePart::Text(value.into())]),
        )
    }
    fn segment(id: &str, value: &str) -> TimelineSegment {
        TimelineSegment::new(id, [text(MessageRole::User, value)])
    }

    #[test]
    fn summary_parser_requires_the_canonical_envelope() {
        assert_eq!(
            parse_summary("<compaction_summary>\ntext\n</compaction_summary>"),
            Some("text")
        );
        assert_eq!(parse_summary("text"), None);
        assert_eq!(
            parse_summary("<compaction_summary>text</compaction_summary>"),
            None
        );
    }

    #[test]
    fn compaction_retains_stable_tail_ids_and_has_no_repeated_work() {
        let p = Projection::new(Timeline::new([segment("a", "old"), segment("b", "tail")]));
        let (next, record) = compact(&p, 0, "metadata").unwrap();
        assert_eq!(record.source_segment_ids, vec!["a"]);
        assert_eq!(next.timeline.segments[0].id, "b");
        assert!(matches!(
            plan(&next, 1),
            Err(CompactionError::NoSafeBoundary)
        ));
        assert_eq!(
            next.snapshot.as_ref().unwrap().summary,
            "<compaction_summary>\nmetadata\n</compaction_summary>"
        );
    }

    #[test]
    fn plan_is_atomic_to_segments() {
        let p = Projection::new(Timeline::new([
            TimelineSegment::new(
                "a",
                [text(MessageRole::User, "1"), text(MessageRole::User, "2")],
            ),
            segment("b", "tail"),
        ]));
        let plan = plan(&p, 0).unwrap();
        assert_eq!(plan.source_range, (0, 1));
        assert_eq!(plan.source_entries, 2);
    }

    #[test]
    fn malformed_tool_pairs_are_not_boundaries() {
        let bad = Timeline::new([
            TimelineSegment::new(
                "a",
                [TimelineEntry::new(
                    1,
                    Message::with_parts(
                        MessageRole::Tool,
                        vec![MessagePart::ToolResult {
                            id: "x".into(),
                            content: "bad".into(),
                            ok: false,
                        }],
                    ),
                )],
            ),
            segment("tail", "live"),
        ]);
        assert!(matches!(
            plan(&Projection::new(bad), 0),
            Err(CompactionError::MalformedToolPair)
        ));
    }

    #[test]
    fn stale_plan_and_generation_overflow_are_rejected() {
        let p = Projection::new(Timeline::new([segment("a", "old"), segment("b", "tail")]));
        let prepared = plan(&p, 0).unwrap();
        let mut changed = p.clone();
        changed.generation = 1;
        assert_eq!(
            apply(&changed, &prepared, "x"),
            Err(CompactionError::StalePlan)
        );
        let mut max = p;
        max.generation = Generation::MAX;
        let max_plan = plan(&max, Generation::MAX).unwrap();
        assert_eq!(
            apply(&max, &max_plan, "x"),
            Err(CompactionError::GenerationOverflow)
        );
    }

    #[test]
    fn summaries_are_replacements_across_generations() {
        let p = Projection::new(Timeline::new([
            segment("a", "first"),
            segment("b", "second"),
            segment("c", "third"),
            segment("tail", "live"),
        ]));
        let (mut p, _) = compact(&p, 0, "summary one").unwrap();
        p.timeline.segments.insert(0, segment("new", "new work"));
        let (p, _) = compact(&p, 1, "summary two").unwrap();
        assert_eq!(
            p.snapshot.as_ref().unwrap().summary,
            "<compaction_summary>\nsummary two\n</compaction_summary>"
        );
        assert_eq!(p.snapshots.len(), 2);
        assert_eq!(p.timeline.segments[0].id, "tail");
    }

    #[test]
    fn malformed_intervening_message_is_not_a_boundary() {
        let p = Projection::new(Timeline::new([
            TimelineSegment::new(
                "a",
                [
                    TimelineEntry::new(
                        1,
                        Message::with_parts(
                            MessageRole::Assistant,
                            vec![MessagePart::ToolCall {
                                id: "x".into(),
                                name: "bash".into(),
                                args: "{}".into(),
                            }],
                        ),
                    ),
                    text(MessageRole::User, "intervening"),
                    TimelineEntry::new(
                        1,
                        Message::with_parts(
                            MessageRole::Tool,
                            vec![MessagePart::ToolResult {
                                id: "x".into(),
                                content: "ok".into(),
                                ok: true,
                            }],
                        ),
                    ),
                ],
            ),
            segment("tail", "live"),
        ]));
        assert_eq!(plan(&p, 0), Err(CompactionError::MalformedToolPair));
    }

    #[test]
    fn source_digest_binds_apply_to_content() {
        let p = Projection::new(Timeline::new([
            segment("a", "old"),
            segment("tail", "live"),
        ]));
        let prepared = plan(&p, 0).unwrap();
        let mut changed = p.clone();
        changed.timeline.segments[0].entries[0].message =
            Message::text(MessageRole::User, "changed");
        assert_eq!(
            apply(&changed, &prepared, "x"),
            Err(CompactionError::StalePlan)
        );
    }
}

impl TimelineEntry {
    pub fn new(turn: u64, message: Message) -> Self {
        Self { turn, message }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TimelineSegment {
    pub id: String,
    pub entries: Vec<TimelineEntry>,
}

impl TimelineSegment {
    pub fn new(id: impl Into<String>, entries: impl IntoIterator<Item = TimelineEntry>) -> Self {
        Self {
            id: id.into(),
            entries: entries.into_iter().collect(),
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Timeline {
    pub segments: Vec<TimelineSegment>,
}

impl Timeline {
    pub fn new(segments: impl IntoIterator<Item = TimelineSegment>) -> Self {
        Self {
            segments: segments.into_iter().collect(),
        }
    }
    pub fn entries(&self) -> impl Iterator<Item = &TimelineEntry> {
        self.segments.iter().flat_map(|s| s.entries.iter())
    }
    /// The largest entry prefix with valid, completed tool pairing.
    pub fn safe_boundary(&self) -> usize {
        let mut calls = HashSet::new();
        let mut boundary = 0;
        for (index, entry) in self.entries().enumerate() {
            if !consume_tools(entry, &mut calls) {
                break;
            }
            if calls.is_empty() {
                boundary = index + 1;
            }
        }
        boundary
    }
}

fn consume_tools(entry: &TimelineEntry, calls: &mut HashSet<String>) -> bool {
    for part in &entry.message.content {
        match part {
            MessagePart::ToolCall { id, .. } if entry.message.role == MessageRole::Assistant => {
                if !calls.insert(id.clone()) {
                    return false;
                }
            }
            MessagePart::ToolResult { id, .. } if entry.message.role == MessageRole::Tool => {
                if !calls.remove(id) {
                    return false;
                }
            }
            MessagePart::ToolCall { .. } | MessagePart::ToolResult { .. } => return false,
            _ => {}
        }
    }
    true
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Snapshot {
    pub generation: Generation,
    pub source_entries: usize,
    /// Digest of the complete source segments, including IDs and entries.
    #[serde(default)]
    pub source_content_digest: String,
    pub source_segment_ids: Vec<String>,
    /// Half-open segment range in the source projection.
    pub source_range: (usize, usize),
    pub summary: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompactionPlan {
    pub generation: Generation,
    pub source_segment_ids: Vec<String>,
    pub source_range: (usize, usize),
    pub source_entries: usize,
    #[serde(default)]
    pub source_content_digest: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompactionRecord {
    pub from_generation: Generation,
    pub to_generation: Generation,
    pub compacted_entries: usize,
    pub source_segment_ids: Vec<String>,
    pub source_range: (usize, usize),
    pub snapshot: Snapshot,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Projection {
    pub generation: Generation,
    pub timeline: Timeline,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub snapshots: Vec<Snapshot>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub snapshot: Option<Snapshot>,
}

impl Projection {
    pub fn new(timeline: Timeline) -> Self {
        Self {
            generation: 0,
            timeline,
            snapshots: vec![],
            snapshot: None,
        }
    }
    pub fn staleness(&self, generation: Generation) -> Staleness {
        if self.generation == generation {
            Staleness::Fresh
        } else {
            Staleness::Stale
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Staleness {
    Fresh,
    Stale,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CompactionError {
    StaleGeneration {
        expected: Generation,
        actual: Generation,
    },
    NoSafeBoundary,
    MalformedToolPair,
    StalePlan,
    GenerationOverflow,
}

/// Plan compaction of complete, whole segments. The active (last) segment is
/// always retained, even when it is complete.
pub fn plan(
    projection: &Projection,
    expected_generation: Generation,
) -> Result<CompactionPlan, CompactionError> {
    if projection.generation != expected_generation {
        return Err(CompactionError::StaleGeneration {
            expected: expected_generation,
            actual: projection.generation,
        });
    }
    if !valid_provider_roles(&projection.timeline) {
        return Err(CompactionError::MalformedToolPair);
    }
    let safe = projection.timeline.safe_boundary();
    let mut end = 0;
    for segment in projection
        .timeline
        .segments
        .iter()
        .take(projection.timeline.segments.len().saturating_sub(1))
    {
        if end + segment.entries.len() > safe {
            break;
        }
        end += segment.entries.len();
    }
    if end == 0 {
        if projection.timeline.entries().any(|e| {
            e.message.content.iter().any(|p| {
                matches!(p, MessagePart::ToolResult { .. }) && e.message.role != MessageRole::Tool
            })
        }) || projection.timeline.entries().any(|e| {
            e.message.role == MessageRole::Tool
                && e.message
                    .content
                    .iter()
                    .any(|p| matches!(p, MessagePart::ToolResult { .. }))
        }) {
            // A result without a preceding call is malformed, rather than an
            // merely unfinished active turn.
            let mut calls = HashSet::new();
            for e in projection.timeline.entries() {
                if !consume_tools(e, &mut calls) {
                    return Err(CompactionError::MalformedToolPair);
                }
            }
        }
        return Err(CompactionError::NoSafeBoundary);
    }
    // A malformed pair is never silently turned into a boundary.
    if safe
        < projection.timeline.segments[..projection.timeline.segments.len().saturating_sub(1)]
            .iter()
            .map(|s| s.entries.len())
            .sum()
    {
        return Err(CompactionError::MalformedToolPair);
    }
    let mut source_end = 0;
    let mut source_count = 0;
    for segment in &projection.timeline.segments {
        if source_end + segment.entries.len() > end {
            break;
        }
        source_end += segment.entries.len();
        source_count += 1;
        if source_end == end {
            break;
        }
    }
    let source_range = (0, source_count);
    let ids = projection.timeline.segments[source_range.0..source_range.1]
        .iter()
        .map(|s| s.id.clone())
        .collect();
    let source_content_digest =
        digest_segments(&projection.timeline.segments[source_range.0..source_range.1]);
    Ok(CompactionPlan {
        generation: projection.generation,
        source_segment_ids: ids,
        source_range,
        source_entries: end,
        source_content_digest,
    })
}

/// Apply an unchanged plan and record its exact provenance.
pub fn apply(
    projection: &Projection,
    plan: &CompactionPlan,
    summary: impl Into<String>,
) -> Result<(Projection, CompactionRecord), CompactionError> {
    if projection.generation != plan.generation {
        return Err(CompactionError::StalePlan);
    }
    let (start, end) = plan.source_range;
    if start > end || end > projection.timeline.segments.len() {
        return Err(CompactionError::StalePlan);
    }
    let current: Vec<_> = projection.timeline.segments[start..end]
        .iter()
        .map(|s| s.id.clone())
        .collect();
    if current != plan.source_segment_ids {
        return Err(CompactionError::StalePlan);
    }
    if digest_segments(&projection.timeline.segments[start..end]) != plan.source_content_digest {
        return Err(CompactionError::StalePlan);
    }
    let next_generation = projection
        .generation
        .checked_add(1)
        .ok_or(CompactionError::GenerationOverflow)?;
    // The job receives the prior summary as metadata and returns a complete
    // replacement. Do not concatenate generations here: that duplicates
    // context and violates the replacement-summary contract.
    let returned_summary = summary.into();
    let summary = format_summary(parse_summary(&returned_summary).unwrap_or(&returned_summary));
    let snapshot = Snapshot {
        generation: next_generation,
        source_entries: plan.source_entries,
        source_content_digest: plan.source_content_digest.clone(),
        source_segment_ids: plan.source_segment_ids.clone(),
        source_range: plan.source_range,
        summary,
    };
    let mut timeline = projection.timeline.clone();
    timeline.segments.drain(start..end);
    let record = CompactionRecord {
        from_generation: projection.generation,
        to_generation: next_generation,
        compacted_entries: plan.source_entries,
        source_segment_ids: plan.source_segment_ids.clone(),
        source_range: plan.source_range,
        snapshot: snapshot.clone(),
    };
    let mut snapshots = projection.snapshots.clone();
    snapshots.push(snapshot.clone());
    Ok((
        Projection {
            generation: next_generation,
            timeline,
            snapshots,
            snapshot: Some(snapshot),
        },
        record,
    ))
}

/// Compatibility convenience for callers that do not need to retain a plan.
pub fn compact(
    projection: &Projection,
    expected_generation: Generation,
    summary: impl Into<String>,
) -> Result<(Projection, CompactionRecord), CompactionError> {
    let p = plan(projection, expected_generation)?;
    apply(projection, &p, summary)
}
