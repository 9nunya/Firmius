//! Typed, renderer-independent presentation data.
//!
//! This module deliberately sits beside (rather than inside) the current
//! renderer.  It gives the next renderer a stable contract while the existing
//! `Item` folding and drawing code remains unchanged.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet};
use std::ops::Range;

use super::model::{Item, SearchState, ToolState};

/// Stable identity for a thing which can be presented or hit-tested.
///
/// IDs are semantic, never line numbers or vector indexes.  The string
/// payloads are expected to come from durable/provider IDs.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum SemanticId {
    Transcript { ordinal: u64 },
    Tool { id: String },
    Process { id: String },
    Delegate { id: String },
    Workgraph { id: String },
    Goal { id: String },
    ModalRow { modal: String, row: u64 },
}

/// Preferred name used by hit-test and renderer code; retained as an alias so
/// IDs can be passed around without coupling callers to the enum's storage
/// name.
pub type TranscriptEventId = SemanticId;

impl SemanticId {
    pub fn transcript(ordinal: u64) -> Self {
        Self::Transcript { ordinal }
    }
    pub fn tool(id: impl Into<String>) -> Self {
        Self::Tool { id: id.into() }
    }
    pub fn process(id: impl Into<String>) -> Self {
        Self::Process { id: id.into() }
    }
    pub fn delegate(id: impl Into<String>) -> Self {
        Self::Delegate { id: id.into() }
    }
    pub fn workgraph(id: impl Into<String>) -> Self {
        Self::Workgraph { id: id.into() }
    }
    pub fn goal(id: impl Into<String>) -> Self {
        Self::Goal { id: id.into() }
    }
    pub fn modal_row(modal: impl Into<String>, row: u64) -> Self {
        Self::ModalRow {
            modal: modal.into(),
            row,
        }
    }
}

/// The only disclosure surfaces supported by the presentation contract.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DisclosurePolicy {
    None,
    Thinking,
    LiveOutput,
    Inspector,
}

impl DisclosurePolicy {
    pub fn is_interactive(self) -> bool {
        !matches!(self, Self::None)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DisclosureMode {
    Manual,
    AutoCurrent,
    AutoAll,
}

/// Data-only result of applying a disclosure setting to an event.  The
/// presenter may use this to choose the initial state; toggling remains the
/// renderer's existing mouse/model concern (there is no keyboard trigger).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DisclosureDecision {
    None,
    Current,
    All,
}

/// Decide whether an event should start disclosed. `AutoCurrent` only affects
/// the current eligible event, while `AutoAll` also affects future eligible
/// events. Completed ordinary tools are intentionally never eligible.
pub fn disclosure_decision(
    mode: DisclosureMode,
    event: &TranscriptEvent,
    is_current: bool,
) -> DisclosureDecision {
    if !event.is_disclosable() {
        return DisclosureDecision::None;
    }
    match mode {
        DisclosureMode::Manual => DisclosureDecision::None,
        DisclosureMode::AutoCurrent if is_current => DisclosureDecision::Current,
        DisclosureMode::AutoCurrent => DisclosureDecision::None,
        DisclosureMode::AutoAll => DisclosureDecision::All,
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ThinkingMode {
    CollapseAfterActivity,
    KeepOpen,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TailLines {
    One,
    Three,
    Five,
}

impl TailLines {
    pub const fn count(self) -> usize {
        match self {
            Self::One => 1,
            Self::Three => 3,
            Self::Five => 5,
        }
    }
}

/// User-facing disclosure settings.  These values are intentionally data;
/// no key handling or renderer policy is hidden in this type.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PresentationSettings {
    pub disclosure: DisclosureMode,
    pub thinking: ThinkingMode,
    pub tail_lines: TailLines,
}

impl Default for PresentationSettings {
    fn default() -> Self {
        Self {
            disclosure: DisclosureMode::Manual,
            thinking: ThinkingMode::CollapseAfterActivity,
            tail_lines: TailLines::Three,
        }
    }
}

/// Semantic event kinds corresponding to every current [`Item`] variant.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TranscriptEventKind {
    User,
    Text,
    AgentMessage,
    SystemMessage,
    AssignmentCompletion,
    Thinking,
    ToolCall,
    WebSearch,
    Compaction,
    Note,
}

/// A semantic transcript event. `primary` and `detail` are content, not
/// pre-framed terminal lines, so a future renderer can reflow them safely.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TranscriptEvent {
    pub id: SemanticId,
    pub kind: TranscriptEventKind,
    pub primary: String,
    pub detail: Vec<String>,
    pub disclosure: DisclosurePolicy,
    pub children: Vec<TranscriptEvent>,
    pub revision: u64,
}

impl TranscriptEvent {
    pub fn new(id: SemanticId, kind: TranscriptEventKind, primary: impl Into<String>) -> Self {
        Self {
            id,
            kind,
            primary: primary.into(),
            detail: Vec::new(),
            disclosure: DisclosurePolicy::None,
            children: Vec::new(),
            revision: 0,
        }
    }

    /// Adapt an existing runtime item without changing its folding or render
    /// behavior. The ordinal is the durable message/event ordinal supplied by
    /// the caller, not a rendered row number.
    pub fn from_item(item: &Item, ordinal: u64) -> Self {
        let (kind, primary, disclosure, id) = match item {
            Item::User(text) => (
                TranscriptEventKind::User,
                text.clone(),
                DisclosurePolicy::None,
                SemanticId::transcript(ordinal),
            ),
            Item::AgentMessage { sender_id, text } => (
                TranscriptEventKind::AgentMessage,
                format!("from {sender_id}: {text}"),
                DisclosurePolicy::None,
                SemanticId::transcript(ordinal),
            ),
            Item::SystemMessage { text } => (
                TranscriptEventKind::SystemMessage,
                text.clone(),
                DisclosurePolicy::None,
                SemanticId::transcript(ordinal),
            ),
            Item::AssignmentCompletion {
                assignment_id,
                text,
                ..
            } => (
                TranscriptEventKind::AssignmentCompletion,
                format!("assignment {assignment_id}: {text}"),
                DisclosurePolicy::None,
                SemanticId::transcript(ordinal),
            ),
            Item::Text(text) => (
                TranscriptEventKind::Text,
                text.clone(),
                DisclosurePolicy::None,
                SemanticId::transcript(ordinal),
            ),
            Item::Thinking { text: _, .. } => (
                TranscriptEventKind::Thinking,
                "Thinking".into(),
                DisclosurePolicy::Thinking,
                SemanticId::transcript(ordinal),
            ),
            Item::ToolCall {
                stream_id,
                stream_index,
                name,
                state,
                result,
                ..
            } => {
                let id = stream_id
                    .clone()
                    .map(SemanticId::tool)
                    .unwrap_or_else(|| SemanticId::tool(format!("{name}:{stream_index}")));
                // Output-capable tools keep a real retained-output surface
                // after settlement: a completed bash/delegate run remains
                // expandable while its retained output exists. Ordinary
                // completed tools (read/edit/grep/…) deliberately stay
                // concise; their result is a summary, not a transcript dump.
                let output_capable = matches!(
                    name.as_str(),
                    "bash" | "delegate" | "task" | "message" | "edit"
                );
                let has_retained_output = result.as_deref().is_some_and(|value| !value.is_empty());
                let disclosure = if output_capable
                    && (!matches!(state, ToolState::Done { .. } | ToolState::Interrupted)
                        || has_retained_output)
                {
                    DisclosurePolicy::LiveOutput
                } else {
                    DisclosurePolicy::None
                };
                (TranscriptEventKind::ToolCall, name.clone(), disclosure, id)
            }
            Item::WebSearch { id, state, .. } => (
                TranscriptEventKind::WebSearch,
                "web search".into(),
                // A hosted search that produced results stays inspectable;
                // interrupted/other searches collapse back to a quiet row.
                if matches!(state, SearchState::Preparing(_)) {
                    DisclosurePolicy::LiveOutput
                } else {
                    DisclosurePolicy::None
                },
                SemanticId::tool(id.clone()),
            ),
            Item::Compaction(item) => (
                TranscriptEventKind::Compaction,
                "compaction".into(),
                // Compaction summaries are part of the transcript itself:
                // they remain fully visible while streaming and after
                // settlement, rather than acquiring a collapsible output
                // surface like a tool call.
                DisclosurePolicy::None,
                SemanticId::transcript(item.generation),
            ),
            Item::Note(text) => (
                TranscriptEventKind::Note,
                text.clone(),
                DisclosurePolicy::None,
                SemanticId::transcript(ordinal),
            ),
        };
        let mut event = Self::new(id, kind, primary);
        event.revision = ordinal;
        if let Item::Thinking { text, .. } = item {
            event.detail = text.lines().map(str::to_owned).collect();
        }
        event.disclosure = disclosure;
        event
    }

    pub fn is_disclosable(&self) -> bool {
        self.disclosure.is_interactive()
    }
}

/// A semantic subtarget within an event. Ordinary text has no interactive
/// target, preserving mouse selection behavior.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HitSubtarget {
    Header,
    ThinkingHeader,
    LiveOutputHeader,
    Inspector,
    Detail,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HitTarget {
    pub event_id: SemanticId,
    pub subtarget: HitSubtarget,
    pub lines: Range<usize>,
    /// Column extent of the interactive affordance within its line (the
    /// glyph plus the visible `expand/collapse` text). Hit testing must
    /// match what is actually rendered — a wider-than-content or
    /// narrower-than-content rectangle either steals selection or makes
    /// clicks on the visible label do nothing.
    pub columns: Range<usize>,
}

/// Layout metadata intentionally contains stable IDs rather than item indexes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EventLayout {
    pub event_id: SemanticId,
    pub lines: Vec<String>,
    pub hit_targets: Vec<HitTarget>,
    pub height: usize,
    pub revision: u64,
}

impl EventLayout {
    pub fn from_event(event: &TranscriptEvent) -> Self {
        let mut lines = vec![event.primary.clone()];
        lines.extend(event.detail.iter().cloned());
        let height = lines.len();
        let mut hit_targets = Vec::new();
        if event.is_disclosable() {
            let subtarget = match event.disclosure {
                DisclosurePolicy::Thinking => HitSubtarget::ThinkingHeader,
                DisclosurePolicy::LiveOutput => HitSubtarget::LiveOutputHeader,
                DisclosurePolicy::Inspector => HitSubtarget::Inspector,
                DisclosurePolicy::None => HitSubtarget::Header,
            };
            let affordance_len = if event.disclosure == DisclosurePolicy::Thinking {
                "Thinking  ▸ expand".chars().count()
            } else {
                "▸ expand".chars().count()
            };
            hit_targets.push(HitTarget {
                event_id: event.id.clone(),
                subtarget,
                lines: 0..1,
                columns: 0..affordance_len,
            });
        }
        Self {
            event_id: event.id.clone(),
            lines,
            hit_targets,
            height,
            revision: event.revision,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CoverageClass {
    Transcript,
    TaskStatus,
    InspectorOnly,
    Suppressed,
}

pub type CoverageClassification = CoverageClass;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ItemVariant {
    User,
    Text,
    AgentMessage,
    SystemMessage,
    AssignmentCompletion,
    Thinking,
    ToolCall,
    WebSearch,
    Compaction,
    Note,
}

impl ItemVariant {
    pub const ALL: [Self; 10] = [
        Self::User,
        Self::Text,
        Self::AgentMessage,
        Self::SystemMessage,
        Self::AssignmentCompletion,
        Self::Thinking,
        Self::ToolCall,
        Self::WebSearch,
        Self::Compaction,
        Self::Note,
    ];
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CoverageAssertion {
    pub variant: ItemVariant,
    pub classification: CoverageClass,
    pub covered: bool,
}

impl CoverageAssertion {
    pub const fn covered(variant: ItemVariant, classification: CoverageClass) -> Self {
        Self {
            variant,
            classification,
            covered: true,
        }
    }
}

pub fn item_variant(item: &Item) -> ItemVariant {
    match item {
        Item::User(_) => ItemVariant::User,
        Item::Text(_) => ItemVariant::Text,
        Item::AgentMessage { .. } => ItemVariant::AgentMessage,
        Item::SystemMessage { .. } => ItemVariant::SystemMessage,
        Item::AssignmentCompletion { .. } => ItemVariant::AssignmentCompletion,
        Item::Thinking { .. } => ItemVariant::Thinking,
        Item::ToolCall { .. } => ItemVariant::ToolCall,
        Item::WebSearch { .. } => ItemVariant::WebSearch,
        Item::Compaction(_) => ItemVariant::Compaction,
        Item::Note(_) => ItemVariant::Note,
    }
}

pub fn classify_item(item: &Item) -> CoverageClass {
    match item {
        Item::User(_)
        | Item::Text(_)
        | Item::AgentMessage { .. }
        | Item::SystemMessage { .. }
        | Item::AssignmentCompletion { .. }
        | Item::Thinking { .. }
        | Item::ToolCall { .. }
        | Item::WebSearch { .. }
        | Item::Compaction(_)
        | Item::Note(_) => CoverageClass::Transcript,
    }
}

/// The exhaustive source inventory used by the Phase 0 coverage gate.
pub fn current_item_coverage() -> Vec<CoverageAssertion> {
    ItemVariant::ALL
        .into_iter()
        .map(|variant| {
            let classification = match variant {
                ItemVariant::Thinking => CoverageClass::Transcript,
                _ => CoverageClass::Transcript,
            };
            CoverageAssertion::covered(variant, classification)
        })
        .collect()
}

pub fn assert_item_coverage(assertions: &[CoverageAssertion]) -> Result<(), Vec<ItemVariant>> {
    let covered: BTreeMap<_, _> = assertions
        .iter()
        .filter(|a| a.covered)
        .map(|a| (a.variant, a.classification))
        .collect();
    let missing = ItemVariant::ALL
        .into_iter()
        .filter(|v| !covered.contains_key(v))
        .collect::<Vec<_>>();
    if missing.is_empty() {
        Ok(())
    } else {
        Err(missing)
    }
}

/// A one-shot, revision-driven arrival cue. There is intentionally no
/// `Instant` or tick field: wall-clock animation cannot create a cue.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArrivalCue {
    pub event_id: SemanticId,
    pub root_id: SemanticId,
    pub text_span: Range<usize>,
    pub source_revision: u64,
    pub unread: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ArrivalOutcome {
    Cue,
    Unread,
    IgnoredStale,
}

#[derive(Debug, Default, Clone)]
pub struct ArrivalCueTracker {
    cues: BTreeMap<SemanticId, ArrivalCue>,
    revisions: BTreeMap<SemanticId, u64>,
    rendered: RefCell<BTreeSet<SemanticId>>,
}

impl ArrivalCueTracker {
    pub fn record(
        &mut self,
        event_id: SemanticId,
        root_id: SemanticId,
        source_revision: u64,
        text_span: Range<usize>,
        visible: bool,
    ) -> ArrivalOutcome {
        let previous = self.revisions.get(&event_id).copied().unwrap_or(0);
        if source_revision <= previous {
            return ArrivalOutcome::IgnoredStale;
        }
        self.revisions.insert(event_id.clone(), source_revision);
        self.rendered.borrow_mut().remove(&event_id);
        // Only one active cue may exist for a visible root. Off-screen work is
        // retained as unread state and is revealed when the root returns.
        self.cues.retain(|_, cue| cue.root_id != root_id);
        let cue = ArrivalCue {
            event_id: event_id.clone(),
            root_id: root_id.clone(),
            text_span,
            source_revision,
            unread: !visible,
        };
        self.cues.insert(event_id, cue);
        if visible {
            ArrivalOutcome::Cue
        } else {
            ArrivalOutcome::Unread
        }
    }

    pub fn cue(&self, event_id: &SemanticId) -> Option<&ArrivalCue> {
        self.cues.get(event_id)
    }

    /// Remove and return a cue after the renderer has applied its one-shot
    /// highlight.  Keeping consumption explicit makes redraws idempotent:
    /// merely painting another frame cannot replay an already acknowledged
    /// revision.
    pub fn take(&mut self, event_id: &SemanticId) -> Option<ArrivalCue> {
        self.cues.remove(event_id)
    }

    /// Claim a visible cue for the current render pass.  This shared method is
    /// safe to call from the immutable renderer and guarantees one highlight
    /// per accepted revision without invalidating the transcript cache.
    pub fn claim_for_render(&self, event_id: &SemanticId) -> Option<ArrivalCue> {
        let cue = self.cues.get(event_id)?;
        if cue.unread || !self.rendered.borrow_mut().insert(event_id.clone()) {
            return None;
        }
        Some(cue.clone())
    }

    pub fn has_visible_pending(&self) -> bool {
        self.cues.values().any(|cue| !cue.unread)
    }

    /// The newest accepted revision for an event, including revisions whose
    /// cue was consumed or which arrived while the event was off-screen.
    pub fn revision(&self, event_id: &SemanticId) -> Option<u64> {
        self.revisions.get(event_id).copied()
    }
    pub fn unread_for(&self, root_id: &SemanticId) -> usize {
        self.cues
            .values()
            .filter(|cue| cue.root_id == *root_id && cue.unread)
            .count()
    }

    /// Mark off-screen activity as visible again.  This does not create a new
    /// revision; it only makes the already-recorded cue eligible for its
    /// one-shot render pass.
    pub fn reveal(&mut self, root_id: &SemanticId) {
        for cue in self.cues.values_mut().filter(|cue| &cue.root_id == root_id) {
            cue.unread = false;
        }
    }
    pub fn clear(&mut self, event_id: &SemanticId) {
        self.cues.remove(event_id);
    }
}

#[cfg(test)]
mod tests {
    use super::super::model::CompactionPhase;
    use super::*;
    use std::time::Instant;

    fn item_variants() -> Vec<Item> {
        vec![
            Item::User("hello".into()),
            Item::Text("answer".into()),
            Item::AgentMessage {
                sender_id: "worker".into(),
                text: "delegated update".into(),
            },
            Item::SystemMessage {
                text: "system update".into(),
            },
            Item::AssignmentCompletion {
                child_agent_id: "child".into(),
                assignment_id: "assignment-1".into(),
                text: "assignment complete".into(),
            },
            Item::Thinking {
                text: "line one\nline two".into(),
            },
            Item::ToolCall {
                stream_id: Some("tool-1".into()),
                stream_index: 0,
                name: "bash".into(),
                args: "{}".into(),
                result: None,
                state: ToolState::Running(Instant::now()),
            },
            Item::WebSearch {
                id: "search-1".into(),
                action: firmius_core::WebSearchAction::Search {
                    query: None,
                    queries: None,
                },
                state: SearchState::Preparing(Instant::now()),
            },
            Item::Compaction(super::super::model::CompactionItem {
                generation: 4,
                summary: "compact".into(),
                phase: CompactionPhase::Running(Instant::now()),
            }),
            Item::Note("note".into()),
        ]
    }

    #[test]
    fn item_variants_are_exhaustively_covered() {
        assert_item_coverage(&current_item_coverage()).unwrap();
        for (ordinal, item) in item_variants().iter().enumerate() {
            let event = TranscriptEvent::from_item(item, ordinal as u64 + 1);
            assert_eq!(item_variant(item), ItemVariant::ALL[ordinal]);
            assert_eq!(classify_item(item), CoverageClass::Transcript);
            assert_eq!(event.revision, ordinal as u64 + 1);
            assert_eq!(
                event.kind,
                match item_variant(item) {
                    ItemVariant::User => TranscriptEventKind::User,
                    ItemVariant::Text => TranscriptEventKind::Text,
                    ItemVariant::AgentMessage => TranscriptEventKind::AgentMessage,
                    ItemVariant::SystemMessage => TranscriptEventKind::SystemMessage,
                    ItemVariant::AssignmentCompletion => TranscriptEventKind::AssignmentCompletion,
                    ItemVariant::Thinking => TranscriptEventKind::Thinking,
                    ItemVariant::ToolCall => TranscriptEventKind::ToolCall,
                    ItemVariant::WebSearch => TranscriptEventKind::WebSearch,
                    ItemVariant::Compaction => TranscriptEventKind::Compaction,
                    ItemVariant::Note => TranscriptEventKind::Note,
                }
            );
        }
    }

    #[test]
    fn coverage_assertion_reports_missing_variants() {
        let assertions = [CoverageAssertion::covered(
            ItemVariant::User,
            CoverageClass::Transcript,
        )];
        let missing = assert_item_coverage(&assertions).unwrap_err();
        assert!(missing.contains(&ItemVariant::ToolCall));
        assert_eq!(missing.len(), ItemVariant::ALL.len() - 1);
    }

    #[test]
    fn tool_identity_and_disclosure_survive_layout_reflow() {
        let item = Item::ToolCall {
            stream_id: Some("provider-7".into()),
            stream_index: 4,
            name: "bash".into(),
            args: "{}".into(),
            result: None,
            state: ToolState::Running(std::time::Instant::now()),
        };
        let first = TranscriptEvent::from_item(&item, 9);
        let second = TranscriptEvent::from_item(&item, 9);
        assert_eq!(first.id, second.id);
        assert_eq!(first.disclosure, DisclosurePolicy::LiveOutput);
        assert_eq!(EventLayout::from_event(&first).event_id, first.id);
    }

    #[test]
    fn compaction_is_not_a_collapsible_disclosure_surface() {
        let item = Item::Compaction(super::super::model::CompactionItem {
            generation: 4,
            summary: "full summary".into(),
            phase: CompactionPhase::Running(std::time::Instant::now()),
        });
        let event = TranscriptEvent::from_item(&item, 9);
        assert_eq!(event.disclosure, DisclosurePolicy::None);
        assert!(!event.is_disclosable());
        assert!(EventLayout::from_event(&event).hit_targets.is_empty());
    }

    #[test]
    fn event_layout_keeps_disclosure_target_on_header_only_after_reflow() {
        let mut event = TranscriptEvent::new(
            SemanticId::transcript(1),
            TranscriptEventKind::Thinking,
            "Thinking header",
        );
        event.disclosure = DisclosurePolicy::Thinking;
        event.detail = vec!["wrapped detail".into(), "more detail".into()];
        let layout = EventLayout::from_event(&event);
        assert_eq!(layout.height, 3);
        assert_eq!(
            layout.hit_targets[0].subtarget,
            HitSubtarget::ThinkingHeader
        );
        assert_eq!(layout.hit_targets[0].lines, 0..1);
    }

    #[test]
    fn arrival_cues_are_revision_driven_and_one_per_root() {
        let root = SemanticId::delegate("child");
        let event = SemanticId::process("proc");
        let mut tracker = ArrivalCueTracker::default();
        assert_eq!(
            tracker.record(event.clone(), root.clone(), 1, 0..1, true),
            ArrivalOutcome::Cue
        );
        assert_eq!(
            tracker.record(event.clone(), root.clone(), 1, 0..1, true),
            ArrivalOutcome::IgnoredStale
        );
        assert_eq!(
            tracker.record(SemanticId::process("other"), root.clone(), 2, 1..2, false),
            ArrivalOutcome::Unread
        );
        assert_eq!(tracker.unread_for(&root), 1);
        // A newer visible arrival replaces the off-screen unread cue for the
        // same root; a stale revision can never resurrect it.
        assert_eq!(
            tracker.record(SemanticId::process("other"), root.clone(), 1, 1..2, true),
            ArrivalOutcome::IgnoredStale
        );
        assert_eq!(tracker.unread_for(&root), 1);
        assert_eq!(
            tracker.record(SemanticId::process("other"), root.clone(), 3, 1..2, true),
            ArrivalOutcome::Cue
        );
        assert_eq!(tracker.unread_for(&root), 0);
        assert!(tracker.cue(&SemanticId::process("other")).is_some());
        tracker.clear(&SemanticId::process("other"));
        assert!(tracker.cue(&SemanticId::process("other")).is_none());
    }

    #[test]
    fn arrival_cue_is_one_shot_and_preserves_only_the_changed_span() {
        let event = SemanticId::process("proc");
        let root = SemanticId::delegate("child");
        let mut tracker = ArrivalCueTracker::default();
        assert_eq!(
            tracker.record(event.clone(), root, 4, 3..7, true),
            ArrivalOutcome::Cue
        );
        let cue = tracker.take(&event).expect("new revision has one cue");
        assert_eq!(cue.text_span, 3..7);
        assert_eq!(cue.source_revision, 4);
        assert!(
            tracker.take(&event).is_none(),
            "consumed cues do not replay"
        );
        assert_eq!(tracker.revision(&event), Some(4));
        assert_eq!(
            tracker.record(event.clone(), SemanticId::delegate("child"), 4, 0..99, true),
            ArrivalOutcome::IgnoredStale
        );
        assert!(tracker.take(&event).is_none());
    }

    #[test]
    fn unrelated_event_revisions_do_not_trigger_or_replace_each_other() {
        let root = SemanticId::delegate("child");
        let first = SemanticId::process("first");
        let second = SemanticId::process("second");
        let mut tracker = ArrivalCueTracker::default();
        assert_eq!(
            tracker.record(first.clone(), root.clone(), 9, 1..2, true),
            ArrivalOutcome::Cue
        );
        assert_eq!(
            tracker.record(second.clone(), root, 1, 8..9, false),
            ArrivalOutcome::Unread
        );
        assert_eq!(tracker.revision(&first), Some(9));
        assert_eq!(tracker.revision(&second), Some(1));
        assert!(
            tracker.cue(&first).is_none(),
            "one cue per root is retained"
        );
        assert_eq!(tracker.cue(&second).unwrap().text_span, 8..9);
    }

    #[test]
    fn settings_default_to_calm_manual_three_line_tail() {
        let settings = PresentationSettings::default();
        assert_eq!(settings.disclosure, DisclosureMode::Manual);
        assert_eq!(settings.thinking, ThinkingMode::CollapseAfterActivity);
        assert_eq!(settings.tail_lines.count(), 3);
    }

    #[test]
    fn disclosure_decision_is_data_only_and_auto_modes_are_scoped() {
        let live = TranscriptEvent {
            disclosure: DisclosurePolicy::LiveOutput,
            ..TranscriptEvent::new(
                SemanticId::tool("bash-1"),
                TranscriptEventKind::ToolCall,
                "bash",
            )
        };
        assert_eq!(
            disclosure_decision(DisclosureMode::Manual, &live, true),
            DisclosureDecision::None
        );
        assert_eq!(
            disclosure_decision(DisclosureMode::AutoCurrent, &live, true),
            DisclosureDecision::Current
        );
        assert_eq!(
            disclosure_decision(DisclosureMode::AutoCurrent, &live, false),
            DisclosureDecision::None
        );
        assert_eq!(
            disclosure_decision(DisclosureMode::AutoAll, &live, false),
            DisclosureDecision::All
        );

        let ordinary_done = TranscriptEvent::new(
            SemanticId::tool("read-1"),
            TranscriptEventKind::ToolCall,
            "read",
        );
        assert_eq!(
            disclosure_decision(DisclosureMode::AutoAll, &ordinary_done, true),
            DisclosureDecision::None
        );
    }
}
