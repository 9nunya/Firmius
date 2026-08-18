//! Pure policy for choosing historical segments to compact.
//!
//! This module deliberately does not alter a timeline or invoke a provider.
//! The result is a prefix of complete segments, so it can be used with the
//! existing generation-checked compaction plan without changing that API.

use crate::compaction::{Timeline, TimelineSegment};
use crate::context_budget::{TokenEstimate, estimate_messages};

/// Optional caller-supplied value attached to a segment.  Missing metadata is
/// treated as ordinary (unprotected) history.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct SegmentMetadata {
    /// Values above the policy's `protect_importance` are retained.
    pub importance: u8,
    /// Pinned history is never selected.
    pub pinned: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SelectionConfig {
    /// Maximum estimated input tokens to send to compaction.
    pub max_tokens: u32,
    /// Number of newest segments that must remain active.
    pub tail_segments: usize,
    /// Metadata at or above this value is retained rather than compacted.
    pub protect_importance: u8,
}

impl Default for SelectionConfig {
    fn default() -> Self {
        Self {
            max_tokens: u32::MAX,
            tail_segments: 1,
            protect_importance: 200,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScoredSegment {
    pub index: usize,
    pub id: String,
    pub estimate: TokenEstimate,
    /// Larger values are preferred for compaction.  Age is useful when
    /// callers inspect diagnostics, while importance reduces the priority.
    pub score: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SegmentSelection {
    /// Indices are in timeline order and form `[0, end)`.
    pub segment_indices: Vec<usize>,
    pub source_range: (usize, usize),
    pub estimated_tokens: u32,
    pub scored_segments: Vec<ScoredSegment>,
}

/// Select a complete historical prefix.  The latest `tail_segments` are
/// always excluded.  A pinned/high-importance segment and everything after it
/// is retained, since selecting around it would not be representable by the
/// existing contiguous compaction plan.
pub fn select_segments(
    segments: &[TimelineSegment],
    metadata: &[SegmentMetadata],
    config: SelectionConfig,
) -> SegmentSelection {
    let tail_start = segments.len().saturating_sub(config.tail_segments.max(1));
    // Do not select a segment containing an unfinished tool exchange.  This
    // mirrors the existing timeline boundary policy while retaining segment
    // atomicity (the selector never truncates a segment).
    let safe_entries = Timeline::new(segments.to_vec()).safe_boundary();
    let mut entries = 0;
    let mut safe_end = 0;
    for segment in segments.iter().take(tail_start) {
        entries += segment.entries.len();
        if entries > safe_entries {
            break;
        }
        safe_end += 1;
    }
    let historical_end = safe_end;
    let mut scored = Vec::with_capacity(historical_end);
    let mut selected = Vec::new();
    let mut total = 0u32;

    for (index, segment) in segments.iter().take(historical_end).enumerate() {
        let estimate = estimate_messages(
            &segment
                .entries
                .iter()
                .map(|entry| entry.message.clone())
                .collect::<Vec<_>>(),
        );
        let meta = metadata.get(index).cloned().unwrap_or_default();
        // Age and size make old, useful-to-remove history win ties. Saturating
        // arithmetic keeps this a total, deterministic policy.
        let score = ((historical_end - index) as u64 * 1_000 + estimate.total() as u64)
            .saturating_sub(meta.importance as u64 * 10_000);
        scored.push(ScoredSegment {
            index,
            id: segment.id.clone(),
            estimate,
            score,
        });

        if meta.pinned || meta.importance >= config.protect_importance {
            break;
        }
        let next = total.saturating_add(estimate.total());
        if next > config.max_tokens {
            break;
        }
        total = next;
        selected.push(index);
    }

    let end = selected.len();
    SegmentSelection {
        segment_indices: selected,
        source_range: (0, end),
        estimated_tokens: total,
        scored_segments: scored,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::compaction::{TimelineEntry, TimelineSegment};
    use crate::types::{Message, MessageRole};

    fn segment(id: &str, text: &str) -> TimelineSegment {
        TimelineSegment::new(
            id,
            [TimelineEntry::new(
                1,
                Message::text(MessageRole::User, text),
            )],
        )
    }

    #[test]
    fn keeps_latest_tail_and_selects_whole_prefix_under_budget() {
        let segments = vec![
            segment("old", "1234"),
            segment("middle", "5678"),
            segment("live", "tail"),
        ];
        let result = select_segments(
            &segments,
            &[],
            SelectionConfig {
                max_tokens: 6,
                ..Default::default()
            },
        );
        assert_eq!(result.segment_indices, vec![0]);
        assert_eq!(result.source_range, (0, 1));
        assert_eq!(result.scored_segments.len(), 2);
    }

    #[test]
    fn high_value_metadata_stops_prefix_before_it() {
        let segments = vec![
            segment("old", "old"),
            segment("important", "keep"),
            segment("tail", "live"),
        ];
        let metadata = vec![
            SegmentMetadata::default(),
            SegmentMetadata {
                importance: 255,
                pinned: false,
            },
        ];
        let result = select_segments(&segments, &metadata, SelectionConfig::default());
        assert_eq!(result.segment_indices, vec![0]);
        assert_eq!(result.scored_segments[1].id, "important");
    }

    #[test]
    fn selection_is_deterministic_and_single_segment_atomic() {
        let segments = vec![
            segment("a", "one"),
            segment("b", "two"),
            segment("tail", "three"),
        ];
        let config = SelectionConfig {
            max_tokens: 100,
            ..Default::default()
        };
        assert_eq!(
            select_segments(&segments, &[], config),
            select_segments(&segments, &[], config)
        );
        assert!(
            select_segments(&segments, &[], config)
                .segment_indices
                .iter()
                .all(|i| *i < 2)
        );
    }
}
