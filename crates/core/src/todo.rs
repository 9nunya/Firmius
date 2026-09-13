//! Native, per-agent todo state.
//!
//! Todos are deliberately smaller than work graphs: they are an agent's
//! private execution loop, not a delegation or scheduling primitive.  The
//! owner is embedded in the ledger and every mutation is compare-and-swap
//! fenced.  Ordered maps and explicit cycle item order make projections and
//! completion assessment deterministic.

use chrono::{DateTime, Utc};
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

pub const TODO_SCHEMA_VERSION: u32 = 1;

macro_rules! string_id {
    ($name:ident) => {
        #[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
        #[serde(transparent)]
        pub struct $name(pub String);

        impl $name {
            pub fn new() -> Self {
                Self(uuid::Uuid::new_v4().to_string())
            }
        }

        impl std::fmt::Display for $name {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.write_str(&self.0)
            }
        }
    };
}

string_id!(TodoCycleId);
string_id!(TodoItemId);
string_id!(EvidenceReceiptId);

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoIntent {
    pub summary: String,
    #[serde(default)]
    pub completion_criteria: Vec<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TodoCycleStatus {
    Active,
    Completed,
    Cancelled,
    Archived,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TodoItemStatus {
    Pending,
    InProgress,
    Completed,
    Cancelled,
    Blocked,
}

impl TodoItemStatus {
    pub fn is_terminal(self) -> bool {
        matches!(self, Self::Completed | Self::Cancelled)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TodoOutcomeKind {
    Completed,
    Cancelled,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoOutcome {
    pub kind: TodoOutcomeKind,
    pub completed_items: usize,
    pub cancelled_items: usize,
    pub evidence_receipts: usize,
    pub assessed_at: DateTime<Utc>,
    pub ledger_revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoCompletionReceipt {
    pub cycle_id: TodoCycleId,
    pub outcome: TodoOutcome,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoItem {
    pub id: TodoItemId,
    pub cycle_id: TodoCycleId,
    pub title: String,
    #[serde(default)]
    pub description: Option<String>,
    pub status: TodoItemStatus,
    #[serde(default)]
    pub evidence_required: bool,
    #[serde(default)]
    pub evidence_receipt_ids: Vec<EvidenceReceiptId>,
    #[serde(default)]
    pub outcome: Option<String>,
    #[serde(default)]
    pub blocking_reason: Option<String>,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
    pub revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoCycle {
    pub id: TodoCycleId,
    pub intent: TodoIntent,
    pub status: TodoCycleStatus,
    #[serde(default)]
    pub item_ids: Vec<TodoItemId>,
    #[serde(default)]
    pub outcome: Option<TodoOutcome>,
    pub created_at: DateTime<Utc>,
    #[serde(default)]
    pub finished_at: Option<DateTime<Utc>>,
    pub revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EvidenceReceipt {
    pub id: EvidenceReceiptId,
    pub cycle_id: TodoCycleId,
    #[serde(default)]
    pub item_id: Option<TodoItemId>,
    pub kind: String,
    pub reference: String,
    #[serde(default)]
    pub summary: Option<String>,
    pub attached_at: DateTime<Utc>,
    pub ledger_revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct EvidenceJournal {
    #[serde(default)]
    pub receipts: Vec<EvidenceReceipt>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NewTodoItem {
    pub title: String,
    pub description: Option<String>,
    pub evidence_required: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct TodoItemPatch {
    pub title: Option<String>,
    pub description: Option<String>,
    pub evidence_required: Option<bool>,
    /// Update may move pending/in-progress/blocked items back into an
    /// actionable state. Complete and cancel retain their dedicated actions.
    pub status: Option<TodoItemStatus>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EvidenceAttachment {
    pub item_id: Option<TodoItemId>,
    pub kind: String,
    pub reference: String,
    pub summary: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum CompletionAction {
    AddItem,
    FinishItem { item_id: TodoItemId },
    ResolveBlock { item_id: TodoItemId, reason: String },
    AttachEvidence { item_id: TodoItemId },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EvidenceDeficit {
    pub item_id: TodoItemId,
    pub requirement: String,
}

/// A pure assessment of the active/latest cycle. `Ready` is deliberately not
/// final: only the CAS-fenced `assess` mutation records the terminal outcome.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum CompletionEvaluation {
    NoActiveCycle,
    Waiting {
        cycle_id: TodoCycleId,
        unfinished_items: Vec<TodoItemId>,
        blocked_items: Vec<TodoItemId>,
        actionable: Vec<CompletionAction>,
        evidence_deficits: Vec<EvidenceDeficit>,
    },
    Ready {
        cycle_id: TodoCycleId,
        outcome_kind: TodoOutcomeKind,
        completed_items: usize,
        cancelled_items: usize,
        evidence_receipts: usize,
    },
    Final {
        cycle_id: TodoCycleId,
        outcome: TodoOutcome,
    },
}

impl CompletionEvaluation {
    pub fn is_final(&self) -> bool {
        matches!(self, Self::Final { .. })
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoLedger {
    pub owner_agent_id: String,
    revision: u64,
    #[serde(default)]
    pub active_cycle_id: Option<TodoCycleId>,
    #[serde(default)]
    pub cycles: BTreeMap<TodoCycleId, TodoCycle>,
    #[serde(default)]
    pub items: BTreeMap<TodoItemId, TodoItem>,
    #[serde(default)]
    pub evidence_journal: EvidenceJournal,
    /// Compact terminal receipt retained after assessment clears working data.
    /// It is observational state only and never contains items or evidence.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_outcome: Option<TodoCompletionReceipt>,
}

impl TodoLedger {
    pub fn new(agent_id: impl Into<String>) -> Self {
        Self {
            owner_agent_id: agent_id.into(),
            revision: 0,
            active_cycle_id: None,
            cycles: BTreeMap::new(),
            items: BTreeMap::new(),
            evidence_journal: EvidenceJournal::default(),
            last_outcome: None,
        }
    }

    pub fn revision(&self) -> u64 {
        self.revision
    }

    /// Authenticated owner embedded in this ledger. Callers restoring a
    /// persisted envelope must compare this with the agent being restored.
    pub fn agent_id(&self) -> &str {
        &self.owner_agent_id
    }

    pub fn begin(
        &mut self,
        expected_revision: u64,
        intent: TodoIntent,
    ) -> Result<TodoCycleId, TodoError> {
        self.cas(expected_revision)?;
        validate_text("intent.summary", &intent.summary)?;
        validate_texts("intent.completion_criteria", &intent.completion_criteria)?;
        if self.active_cycle_id.is_some() {
            return Err(TodoError::ActiveCycleExists);
        }
        let id = TodoCycleId::new();
        let now = Utc::now();
        let revision = self.bump()?;
        self.last_outcome = None;
        self.cycles.insert(
            id.clone(),
            TodoCycle {
                id: id.clone(),
                intent,
                status: TodoCycleStatus::Active,
                item_ids: Vec::new(),
                outcome: None,
                created_at: now,
                finished_at: None,
                revision,
            },
        );
        self.active_cycle_id = Some(id.clone());
        Ok(id)
    }

    pub fn add(&mut self, expected_revision: u64, new: NewTodoItem) -> Result<TodoItem, TodoError> {
        self.cas(expected_revision)?;
        validate_text("title", &new.title)?;
        if let Some(value) = &new.description {
            validate_text("description", value)?;
        }
        let cycle_id = self.active_id()?;
        let id = TodoItemId::new();
        let now = Utc::now();
        let revision = self.bump()?;
        let item = TodoItem {
            id: id.clone(),
            cycle_id: cycle_id.clone(),
            title: new.title,
            description: new.description,
            status: TodoItemStatus::Pending,
            evidence_required: new.evidence_required,
            evidence_receipt_ids: Vec::new(),
            outcome: None,
            blocking_reason: None,
            created_at: now,
            updated_at: now,
            revision,
        };
        self.items.insert(id.clone(), item.clone());
        let cycle = self
            .cycles
            .get_mut(&cycle_id)
            .ok_or_else(|| TodoError::Corrupt("active cycle is missing".into()))?;
        cycle.item_ids.push(id);
        cycle.revision = revision;
        Ok(item)
    }

    pub fn update(
        &mut self,
        expected_revision: u64,
        item_id: &TodoItemId,
        patch: TodoItemPatch,
    ) -> Result<TodoItem, TodoError> {
        self.cas(expected_revision)?;
        self.ensure_active_item(item_id)?;
        if patch.title.is_none()
            && patch.description.is_none()
            && patch.evidence_required.is_none()
            && patch.status.is_none()
        {
            return Err(TodoError::Validation("update has no fields".into()));
        }
        if let Some(value) = &patch.title {
            validate_text("title", value)?;
        }
        if let Some(value) = &patch.description {
            validate_text("description", value)?;
        }
        if matches!(
            patch.status,
            Some(TodoItemStatus::Completed | TodoItemStatus::Cancelled)
        ) {
            return Err(TodoError::Validation(
                "use complete or cancel for terminal status".into(),
            ));
        }
        if self.items[item_id].status.is_terminal() {
            return Err(TodoError::ItemTerminal(item_id.clone()));
        }
        let revision = self.bump()?;
        let item = self.items.get_mut(item_id).expect("validated item");
        if let Some(value) = patch.title {
            item.title = value;
        }
        if let Some(value) = patch.description {
            item.description = Some(value);
        }
        if let Some(value) = patch.evidence_required {
            item.evidence_required = value;
        }
        if let Some(value) = patch.status {
            item.status = value;
            if value != TodoItemStatus::Blocked {
                item.blocking_reason = None;
            }
        }
        item.updated_at = Utc::now();
        item.revision = revision;
        Ok(item.clone())
    }

    pub fn complete(
        &mut self,
        expected_revision: u64,
        item_id: &TodoItemId,
        outcome: Option<String>,
    ) -> Result<TodoItem, TodoError> {
        self.finish_item(
            expected_revision,
            item_id,
            TodoItemStatus::Completed,
            outcome,
        )
    }

    /// Complete several active items in one compare-and-swap transaction.
    /// Validation is performed for the full batch before any item is changed.
    pub fn complete_many(
        &mut self,
        expected_revision: u64,
        item_ids: &[TodoItemId],
        outcome: Option<String>,
    ) -> Result<Vec<TodoItem>, TodoError> {
        self.cas(expected_revision)?;
        if item_ids.is_empty() {
            return Err(TodoError::Validation(
                "complete_many requires at least one item_id".into(),
            ));
        }
        if let Some(value) = &outcome {
            validate_text("outcome", value)?;
        }
        let mut unique = BTreeSet::new();
        for item_id in item_ids {
            if !unique.insert(item_id.clone()) {
                return Err(TodoError::Validation(format!(
                    "duplicate item in complete_many: {item_id}"
                )));
            }
            self.ensure_active_item(item_id)?;
            if self.items[item_id].status.is_terminal() {
                return Err(TodoError::ItemTerminal(item_id.clone()));
            }
        }

        let revision = self.bump()?;
        let now = Utc::now();
        let mut completed = Vec::with_capacity(item_ids.len());
        for item_id in item_ids {
            let item = self.items.get_mut(item_id).expect("validated item");
            item.status = TodoItemStatus::Completed;
            item.outcome = outcome.clone();
            item.blocking_reason = None;
            item.updated_at = now;
            item.revision = revision;
            completed.push(item.clone());
        }
        Ok(completed)
    }

    pub fn cancel(
        &mut self,
        expected_revision: u64,
        item_id: &TodoItemId,
        reason: String,
    ) -> Result<TodoItem, TodoError> {
        validate_text("reason", &reason)?;
        self.finish_item(
            expected_revision,
            item_id,
            TodoItemStatus::Cancelled,
            Some(reason),
        )
    }

    pub fn block(
        &mut self,
        expected_revision: u64,
        item_id: &TodoItemId,
        reason: String,
    ) -> Result<TodoItem, TodoError> {
        self.cas(expected_revision)?;
        validate_text("reason", &reason)?;
        self.ensure_active_item(item_id)?;
        if self.items[item_id].status.is_terminal() {
            return Err(TodoError::ItemTerminal(item_id.clone()));
        }
        let revision = self.bump()?;
        let item = self.items.get_mut(item_id).expect("validated item");
        item.status = TodoItemStatus::Blocked;
        item.blocking_reason = Some(reason);
        item.updated_at = Utc::now();
        item.revision = revision;
        Ok(item.clone())
    }

    pub fn attach_evidence(
        &mut self,
        expected_revision: u64,
        attachment: EvidenceAttachment,
    ) -> Result<EvidenceReceipt, TodoError> {
        self.cas(expected_revision)?;
        validate_text("evidence.kind", &attachment.kind)?;
        validate_text("evidence.reference", &attachment.reference)?;
        if let Some(value) = &attachment.summary {
            validate_text("evidence.summary", value)?;
        }
        let cycle_id = self.active_id()?;
        if let Some(item_id) = &attachment.item_id {
            self.ensure_active_item(item_id)?;
        }
        let revision = self.bump()?;
        let receipt = EvidenceReceipt {
            id: EvidenceReceiptId::new(),
            cycle_id: cycle_id.clone(),
            item_id: attachment.item_id.clone(),
            kind: attachment.kind,
            reference: attachment.reference,
            summary: attachment.summary,
            attached_at: Utc::now(),
            ledger_revision: revision,
        };
        if let Some(item_id) = &attachment.item_id {
            let item = self.items.get_mut(item_id).expect("validated item");
            item.evidence_receipt_ids.push(receipt.id.clone());
            item.updated_at = receipt.attached_at;
            item.revision = revision;
        }
        self.cycles
            .get_mut(&cycle_id)
            .expect("active cycle")
            .revision = revision;
        self.evidence_journal.receipts.push(receipt.clone());
        Ok(receipt)
    }

    pub fn evaluate_completion(&self) -> CompletionEvaluation {
        let cycle = self
            .active_cycle_id
            .as_ref()
            .and_then(|id| self.cycles.get(id));
        let Some(cycle) = cycle else {
            return self
                .last_outcome
                .as_ref()
                .map(|receipt| CompletionEvaluation::Final {
                    cycle_id: receipt.cycle_id.clone(),
                    outcome: receipt.outcome.clone(),
                })
                .unwrap_or(CompletionEvaluation::NoActiveCycle);
        };
        if let Some(outcome) = &cycle.outcome {
            return CompletionEvaluation::Final {
                cycle_id: cycle.id.clone(),
                outcome: outcome.clone(),
            };
        }
        let mut unfinished = Vec::new();
        let mut blocked = Vec::new();
        let mut actionable = Vec::new();
        let mut deficits = Vec::new();
        for id in &cycle.item_ids {
            let Some(item) = self.items.get(id) else {
                continue;
            };
            if !item.status.is_terminal() {
                unfinished.push(id.clone());
                if item.status == TodoItemStatus::Blocked {
                    blocked.push(id.clone());
                    actionable.push(CompletionAction::ResolveBlock {
                        item_id: id.clone(),
                        reason: item
                            .blocking_reason
                            .clone()
                            .unwrap_or_else(|| "blocked".into()),
                    });
                } else {
                    actionable.push(CompletionAction::FinishItem {
                        item_id: id.clone(),
                    });
                }
            }
            if item.status == TodoItemStatus::Completed
                && item.evidence_required
                && item.evidence_receipt_ids.is_empty()
            {
                deficits.push(EvidenceDeficit {
                    item_id: id.clone(),
                    requirement: "at least one evidence receipt".into(),
                });
                actionable.push(CompletionAction::AttachEvidence {
                    item_id: id.clone(),
                });
            }
        }
        if cycle.item_ids.is_empty() {
            actionable.push(CompletionAction::AddItem);
        }
        if !unfinished.is_empty() || !deficits.is_empty() || cycle.item_ids.is_empty() {
            return CompletionEvaluation::Waiting {
                cycle_id: cycle.id.clone(),
                unfinished_items: unfinished,
                blocked_items: blocked,
                actionable,
                evidence_deficits: deficits,
            };
        }
        let completed_items = cycle
            .item_ids
            .iter()
            .filter(|id| {
                self.items
                    .get(*id)
                    .is_some_and(|i| i.status == TodoItemStatus::Completed)
            })
            .count();
        let cancelled_items = cycle.item_ids.len().saturating_sub(completed_items);
        CompletionEvaluation::Ready {
            cycle_id: cycle.id.clone(),
            outcome_kind: if completed_items == 0 {
                TodoOutcomeKind::Cancelled
            } else {
                TodoOutcomeKind::Completed
            },
            completed_items,
            cancelled_items,
            evidence_receipts: self
                .evidence_journal
                .receipts
                .iter()
                .filter(|r| r.cycle_id == cycle.id)
                .count(),
        }
    }

    pub fn assess(&mut self, expected_revision: u64) -> Result<CompletionEvaluation, TodoError> {
        self.cas(expected_revision)?;
        let evaluation = self.evaluate_completion();
        let CompletionEvaluation::Ready {
            cycle_id,
            outcome_kind,
            completed_items,
            cancelled_items,
            evidence_receipts,
        } = evaluation
        else {
            return Ok(evaluation);
        };
        let revision = self.bump()?;
        let now = Utc::now();
        let outcome = TodoOutcome {
            kind: outcome_kind,
            completed_items,
            cancelled_items,
            evidence_receipts,
            assessed_at: now,
            ledger_revision: revision,
        };
        let item_ids = self
            .cycles
            .get(&cycle_id)
            .expect("evaluated cycle")
            .item_ids
            .clone();
        self.cycles.remove(&cycle_id);
        for item_id in item_ids {
            self.items.remove(&item_id);
        }
        self.evidence_journal
            .receipts
            .retain(|receipt| receipt.cycle_id != cycle_id);
        self.active_cycle_id = None;
        self.last_outcome = Some(TodoCompletionReceipt {
            cycle_id: cycle_id.clone(),
            outcome: outcome.clone(),
        });
        Ok(CompletionEvaluation::Final { cycle_id, outcome })
    }

    pub fn archive(
        &mut self,
        expected_revision: u64,
        cycle_id: &TodoCycleId,
    ) -> Result<(), TodoError> {
        self.cas(expected_revision)?;
        let cycle = self
            .cycles
            .get(cycle_id)
            .ok_or_else(|| TodoError::CycleNotFound(cycle_id.clone()))?;
        if cycle.status == TodoCycleStatus::Active {
            return Err(TodoError::CycleActive(cycle_id.clone()));
        }
        let revision = self.bump()?;
        let cycle = self.cycles.get_mut(cycle_id).expect("validated cycle");
        cycle.status = TodoCycleStatus::Archived;
        cycle.revision = revision;
        Ok(())
    }

    pub fn project(&self, compact: bool) -> TodoProjection {
        let cycles = self
            .cycles
            .values()
            .filter(|cycle| !compact || cycle.status != TodoCycleStatus::Archived)
            .map(|cycle| {
                let items = cycle
                    .item_ids
                    .iter()
                    .filter_map(|id| self.items.get(id))
                    .map(|item| TodoItemProjection {
                        id: item.id.clone(),
                        title: item.title.clone(),
                        status: item.status,
                        evidence_required: item.evidence_required,
                        evidence_count: item.evidence_receipt_ids.len(),
                        blocking_reason: item.blocking_reason.clone(),
                    })
                    .collect();
                TodoCycleProjection {
                    id: cycle.id.clone(),
                    intent: cycle.intent.clone(),
                    status: cycle.status,
                    items,
                    outcome: cycle.outcome.clone(),
                    created_at: cycle.created_at,
                }
            })
            .collect();
        TodoProjection {
            owner_agent_id: self.owner_agent_id.clone(),
            revision: self.revision,
            active_cycle_id: self.active_cycle_id.clone(),
            cycles,
            evidence_receipts: self.evidence_journal.receipts.len(),
        }
    }

    pub fn validate(&self) -> Result<(), TodoError> {
        validate_text("owner_agent_id", &self.owner_agent_id)?;
        if let Some(id) = &self.active_cycle_id {
            let cycle = self
                .cycles
                .get(id)
                .ok_or_else(|| TodoError::Corrupt("active cycle missing".into()))?;
            if cycle.status != TodoCycleStatus::Active {
                return Err(TodoError::Corrupt("active cycle is terminal".into()));
            }
        }
        let mut referenced = BTreeSet::new();
        for (id, cycle) in &self.cycles {
            if id != &cycle.id {
                return Err(TodoError::Corrupt("cycle map key mismatch".into()));
            }
            for item_id in &cycle.item_ids {
                if !referenced.insert(item_id.clone()) {
                    return Err(TodoError::Corrupt("item is referenced twice".into()));
                }
                let item = self
                    .items
                    .get(item_id)
                    .ok_or_else(|| TodoError::Corrupt("cycle item missing".into()))?;
                if item.cycle_id != cycle.id {
                    return Err(TodoError::Corrupt("item cycle mismatch".into()));
                }
            }
        }
        if referenced.len() != self.items.len() {
            return Err(TodoError::Corrupt("unreferenced item".into()));
        }
        let receipt_ids = self
            .evidence_journal
            .receipts
            .iter()
            .map(|r| r.id.clone())
            .collect::<BTreeSet<_>>();
        if receipt_ids.len() != self.evidence_journal.receipts.len() {
            return Err(TodoError::Corrupt("duplicate evidence receipt".into()));
        }
        for item in self.items.values() {
            if item
                .evidence_receipt_ids
                .iter()
                .any(|id| !receipt_ids.contains(id))
            {
                return Err(TodoError::Corrupt(
                    "item receipt missing from journal".into(),
                ));
            }
        }
        Ok(())
    }

    fn finish_item(
        &mut self,
        expected: u64,
        item_id: &TodoItemId,
        status: TodoItemStatus,
        outcome: Option<String>,
    ) -> Result<TodoItem, TodoError> {
        self.cas(expected)?;
        self.ensure_active_item(item_id)?;
        if self.items[item_id].status.is_terminal() {
            return Err(TodoError::ItemTerminal(item_id.clone()));
        }
        if let Some(value) = &outcome {
            validate_text("outcome", value)?;
        }
        let revision = self.bump()?;
        let item = self.items.get_mut(item_id).expect("validated item");
        item.status = status;
        item.outcome = outcome;
        item.blocking_reason = None;
        item.updated_at = Utc::now();
        item.revision = revision;
        Ok(item.clone())
    }
    fn cas(&self, expected: u64) -> Result<(), TodoError> {
        if expected == self.revision {
            Ok(())
        } else {
            Err(TodoError::StaleRevision {
                expected,
                actual: self.revision,
            })
        }
    }
    fn bump(&mut self) -> Result<u64, TodoError> {
        self.revision = self
            .revision
            .checked_add(1)
            .ok_or(TodoError::RevisionOverflow)?;
        Ok(self.revision)
    }
    fn active_id(&self) -> Result<TodoCycleId, TodoError> {
        self.active_cycle_id.clone().ok_or(TodoError::NoActiveCycle)
    }
    fn ensure_active_item(&self, id: &TodoItemId) -> Result<(), TodoError> {
        let active = self
            .active_cycle_id
            .as_ref()
            .ok_or(TodoError::NoActiveCycle)?;
        let item = self
            .items
            .get(id)
            .ok_or_else(|| TodoError::ItemNotFound(id.clone()))?;
        if &item.cycle_id != active {
            return Err(TodoError::ItemOutsideActiveCycle(id.clone()));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoItemProjection {
    pub id: TodoItemId,
    pub title: String,
    pub status: TodoItemStatus,
    pub evidence_required: bool,
    pub evidence_count: usize,
    pub blocking_reason: Option<String>,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoCycleProjection {
    pub id: TodoCycleId,
    pub intent: TodoIntent,
    pub status: TodoCycleStatus,
    pub items: Vec<TodoItemProjection>,
    pub outcome: Option<TodoOutcome>,
    /// Authored order is by creation time; consumers use this to select the
    /// most recent cycle deterministically even though the ledger stores
    /// cycles in an id-keyed map.
    pub created_at: DateTime<Utc>,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoProjection {
    pub owner_agent_id: String,
    pub revision: u64,
    pub active_cycle_id: Option<TodoCycleId>,
    pub cycles: Vec<TodoCycleProjection>,
    pub evidence_receipts: usize,
}

impl TodoProjection {
    /// Stable, bounded line-oriented rendering suitable for model context.
    pub fn render_compact(&self) -> String {
        let mut out = format!(
            "todos owner={} revision={} receipts={}\n",
            self.owner_agent_id, self.revision, self.evidence_receipts
        );
        if self.cycles.is_empty() {
            out.push_str("(no cycles)\n");
            return out;
        }
        for cycle in &self.cycles {
            out.push_str(&format!(
                "cycle {} [{:?}] {}\n",
                cycle.id, cycle.status, cycle.intent.summary
            ));
            for item in &cycle.items {
                let evidence = if item.evidence_required {
                    format!(" evidence={}", item.evidence_count)
                } else {
                    String::new()
                };
                let blocked = item
                    .blocking_reason
                    .as_ref()
                    .map(|r| format!(" blocked={r}"))
                    .unwrap_or_default();
                out.push_str(&format!(
                    "- {} [{:?}] {}{}{}\n",
                    item.id, item.status, item.title, evidence, blocked
                ));
            }
        }
        out
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TodoEnvelope {
    pub schema_version: u32,
    pub ledger: TodoLedger,
}
impl TodoEnvelope {
    pub fn new(ledger: TodoLedger) -> Self {
        Self {
            schema_version: TODO_SCHEMA_VERSION,
            ledger,
        }
    }
    pub fn validate(&self) -> Result<(), TodoError> {
        if self.schema_version != TODO_SCHEMA_VERSION {
            return Err(TodoError::UnsupportedSchema(self.schema_version));
        }
        self.ledger.validate()
    }

    /// Validate structure and bind the restored state to its authenticated
    /// owner. This prevents a valid envelope copied from another agent from
    /// being accepted as this agent's private todo state.
    pub fn into_ledger_for(self, agent_id: &str) -> Result<TodoLedger, TodoError> {
        self.validate()?;
        if self.ledger.agent_id() != agent_id {
            return Err(TodoError::OwnerMismatch {
                expected: agent_id.to_string(),
                actual: self.ledger.agent_id().to_string(),
            });
        }
        Ok(self.ledger)
    }
}

/// Lossless compatibility state for lifecycle persistence. A missing legacy
/// field uses `Default`; a malformed present field retains its original JSON.
#[derive(Debug, Clone, PartialEq)]
pub enum PersistedTodoState {
    MissingLegacy,
    Valid(TodoEnvelope),
    Quarantined { raw: Value, reason: String },
}
impl Default for PersistedTodoState {
    fn default() -> Self {
        Self::MissingLegacy
    }
}
impl Serialize for PersistedTodoState {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Self::MissingLegacy => serializer.serialize_none(),
            Self::Valid(value) => value.serialize(serializer),
            Self::Quarantined { raw, .. } => raw.serialize(serializer),
        }
    }
}
impl<'de> Deserialize<'de> for PersistedTodoState {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let raw = Value::deserialize(deserializer)?;
        match serde_json::from_value::<TodoEnvelope>(raw.clone()) {
            Ok(value) => match value.validate() {
                Ok(()) => Ok(Self::Valid(value)),
                Err(error) => Ok(Self::Quarantined {
                    raw,
                    reason: error.to_string(),
                }),
            },
            Err(error) => Ok(Self::Quarantined {
                raw,
                reason: error.to_string(),
            }),
        }
    }
}

#[derive(Debug, thiserror::Error, Clone, PartialEq, Eq)]
pub enum TodoError {
    #[error("stale todo revision: expected {expected}, actual {actual}")]
    StaleRevision { expected: u64, actual: u64 },
    #[error("todo revision overflow")]
    RevisionOverflow,
    #[error("an active todo cycle already exists")]
    ActiveCycleExists,
    #[error("there is no active todo cycle")]
    NoActiveCycle,
    #[error("todo cycle not found: {0}")]
    CycleNotFound(TodoCycleId),
    #[error("todo cycle is still active: {0}")]
    CycleActive(TodoCycleId),
    #[error("todo item not found: {0}")]
    ItemNotFound(TodoItemId),
    #[error("todo item is outside the active cycle: {0}")]
    ItemOutsideActiveCycle(TodoItemId),
    #[error("todo item is terminal: {0}")]
    ItemTerminal(TodoItemId),
    #[error("invalid todo: {0}")]
    Validation(String),
    #[error("corrupt todo ledger: {0}")]
    Corrupt(String),
    #[error("unsupported todo schema version {0}")]
    UnsupportedSchema(u32),
    #[error("todo owner mismatch: expected {expected}, actual {actual}")]
    OwnerMismatch { expected: String, actual: String },
    #[error("todo storage failed: {0}")]
    Storage(String),
}

fn validate_text(field: &str, value: &str) -> Result<(), TodoError> {
    if value.trim().is_empty() {
        Err(TodoError::Validation(format!("{field} must not be empty")))
    } else if value.len() > 16 * 1024 {
        Err(TodoError::Validation(format!("{field} is too long")))
    } else {
        Ok(())
    }
}
fn validate_texts(field: &str, values: &[String]) -> Result<(), TodoError> {
    for value in values {
        validate_text(field, value)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn intent() -> TodoIntent {
        TodoIntent {
            summary: "ship it".into(),
            completion_criteria: vec!["tests pass".into()],
        }
    }
    #[test]
    fn cas_and_evidence_gate_completion_deterministically() {
        let mut ledger = TodoLedger::new("agent-a");
        ledger.begin(0, intent()).unwrap();
        let item = ledger
            .add(
                1,
                NewTodoItem {
                    title: "test".into(),
                    description: None,
                    evidence_required: true,
                },
            )
            .unwrap();
        assert!(matches!(
            ledger.complete(1, &item.id, None),
            Err(TodoError::StaleRevision { actual: 2, .. })
        ));
        ledger.complete(2, &item.id, Some("passed".into())).unwrap();
        match ledger.evaluate_completion() {
            CompletionEvaluation::Waiting {
                evidence_deficits,
                actionable,
                ..
            } => {
                assert_eq!(evidence_deficits.len(), 1);
                assert!(matches!(
                    actionable[0],
                    CompletionAction::AttachEvidence { .. }
                ));
            }
            other => panic!("unexpected {other:?}"),
        }
        ledger
            .attach_evidence(
                3,
                EvidenceAttachment {
                    item_id: Some(item.id),
                    kind: "command".into(),
                    reference: "cargo test".into(),
                    summary: None,
                },
            )
            .unwrap();
        assert!(matches!(
            ledger.evaluate_completion(),
            CompletionEvaluation::Ready { .. }
        ));
        assert!(matches!(
            ledger.assess(4).unwrap(),
            CompletionEvaluation::Final { .. }
        ));
        assert_eq!(ledger.revision(), 5);
        assert!(ledger.active_cycle_id.is_none());
        assert!(ledger.cycles.is_empty());
        assert!(ledger.items.is_empty());
        assert!(ledger.evidence_journal.receipts.is_empty());
        assert!(matches!(
            ledger.evaluate_completion(),
            CompletionEvaluation::Final { outcome, .. }
                if outcome.kind == TodoOutcomeKind::Completed
                    && outcome.completed_items == 1
                    && outcome.cancelled_items == 0
        ));
        let serialized = serde_json::to_value(&TodoEnvelope::new(ledger.clone())).unwrap();
        assert!(serialized["ledger"]["last_outcome"].is_object());
        let round_trip: TodoEnvelope = serde_json::from_value(serialized).unwrap();
        assert_eq!(round_trip.ledger.last_outcome, ledger.last_outcome);
        ledger.begin(5, intent()).unwrap();
        assert!(ledger.last_outcome.is_none());
    }

    #[test]
    fn cancelled_assessment_keeps_a_cancelled_receipt_and_new_begin_supersedes_it() {
        let mut ledger = TodoLedger::new("agent-a");
        let cycle_id = ledger.begin(0, intent()).unwrap();
        let item = ledger
            .add(
                1,
                NewTodoItem {
                    title: "drop".into(),
                    description: None,
                    evidence_required: false,
                },
            )
            .unwrap();
        ledger.cancel(2, &item.id, "obsolete".into()).unwrap();
        let final_state = ledger.assess(3).unwrap();
        assert!(matches!(
            &final_state,
            CompletionEvaluation::Final { outcome, .. }
                if outcome.kind == TodoOutcomeKind::Cancelled
                    && outcome.completed_items == 0
                    && outcome.cancelled_items == 1
        ));
        assert_eq!(ledger.last_outcome.as_ref().unwrap().cycle_id, cycle_id);
        assert!(ledger.cycles.is_empty());
        assert!(ledger.items.is_empty());
        assert!(ledger.evidence_journal.receipts.is_empty());

        ledger.begin(4, intent()).unwrap();
        assert!(ledger.last_outcome.is_none());
        assert!(matches!(
            ledger.evaluate_completion(),
            CompletionEvaluation::Waiting { .. }
        ));
    }

    #[test]
    fn an_old_envelope_without_a_completion_receipt_still_loads() {
        let old = serde_json::json!({
            "schema_version": TODO_SCHEMA_VERSION,
            "ledger": {
                "owner_agent_id": "agent-a",
                "revision": 0,
                "active_cycle_id": null,
                "cycles": {},
                "items": {},
                "evidence_journal": {"receipts": []}
            }
        });
        let envelope: TodoEnvelope = serde_json::from_value(old).unwrap();
        assert!(envelope.ledger.last_outcome.is_none());
        envelope.validate().unwrap();
    }

    #[test]
    fn complete_many_is_atomic_and_uses_one_revision() {
        let mut ledger = TodoLedger::new("agent-a");
        ledger.begin(0, intent()).unwrap();
        let first = ledger
            .add(
                1,
                NewTodoItem {
                    title: "first".into(),
                    description: None,
                    evidence_required: false,
                },
            )
            .unwrap();
        let second = ledger
            .add(
                2,
                NewTodoItem {
                    title: "second".into(),
                    description: None,
                    evidence_required: false,
                },
            )
            .unwrap();
        let missing = TodoItemId("missing".into());
        assert!(matches!(
            ledger.complete_many(3, &[first.id.clone(), missing], None),
            Err(TodoError::ItemNotFound(_))
        ));
        assert_eq!(ledger.revision(), 3);
        assert_eq!(ledger.items[&first.id].status, TodoItemStatus::Pending);

        let completed = ledger
            .complete_many(
                3,
                &[first.id.clone(), second.id.clone()],
                Some("done together".into()),
            )
            .unwrap();
        assert_eq!(ledger.revision(), 4);
        assert_eq!(completed.len(), 2);
        assert!(completed.iter().all(|item| item.revision == 4));
        assert!(
            completed
                .iter()
                .all(|item| item.status == TodoItemStatus::Completed)
        );
    }

    #[test]
    fn malformed_present_state_is_quarantined_losslessly() {
        let raw =
            serde_json::json!({"schema_version":99,"ledger":{"owner_agent_id":"a","revision":0}});
        let state: PersistedTodoState = serde_json::from_value(raw.clone()).unwrap();
        assert!(
            matches!(&state, PersistedTodoState::Quarantined { raw: kept, .. } if kept == &raw)
        );
        assert_eq!(serde_json::to_value(state).unwrap(), raw);
        assert!(matches!(
            PersistedTodoState::default(),
            PersistedTodoState::MissingLegacy
        ));
    }
}
