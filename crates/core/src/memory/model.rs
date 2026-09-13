use std::collections::BTreeMap;

use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use uuid::Uuid;

macro_rules! string_id {
    ($name:ident) => {
        #[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
        #[serde(transparent)]
        pub struct $name(pub String);

        impl $name {
            pub fn new() -> Self {
                Self(Uuid::now_v7().to_string())
            }
        }

        impl Default for $name {
            fn default() -> Self {
                Self::new()
            }
        }

        impl From<String> for $name {
            fn from(value: String) -> Self {
                Self(value)
            }
        }

        impl From<&str> for $name {
            fn from(value: &str) -> Self {
                Self(value.to_owned())
            }
        }

        impl std::fmt::Display for $name {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                self.0.fmt(f)
            }
        }
    };
}

impl Default for MemoryView {
    fn default() -> Self {
        Self {
            include_user: true,
            project_id: None,
            session_id: None,
        }
    }
}

string_id!(MemoryId);
string_id!(EvidenceId);
string_id!(OperationId);
string_id!(SummaryId);

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum MemoryScope {
    User,
    Project { project_id: String },
    Session { session_id: String },
}

impl MemoryScope {
    pub fn is_visible_in(&self, view: &MemoryView) -> bool {
        match self {
            Self::User => view.include_user,
            Self::Project { project_id } => view.project_id.as_ref() == Some(project_id),
            Self::Session { session_id } => view.session_id.as_ref() == Some(session_id),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemoryView {
    #[serde(default = "default_true")]
    pub include_user: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub project_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
}

fn default_true() -> bool {
    true
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MemoryKind {
    Fact,
    Preference,
    Decision,
    Constraint,
    Procedure,
    #[default]
    Note,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemoryContent {
    pub title: String,
    pub body: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum EvidenceKind {
    UserStatement,
    File,
    Command,
    ToolResult,
    Session,
    Inference,
    #[default]
    Other,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemoryEvidence {
    #[serde(default)]
    pub id: EvidenceId,
    #[serde(default)]
    pub kind: EvidenceKind,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub locator: Option<String>,
    pub excerpt: String,
    #[serde(default = "Utc::now")]
    pub observed_at: DateTime<Utc>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RelationKind {
    Supports,
    Contradicts,
    DerivedFrom,
    RelatedTo,
    Supersedes,
    Corrects,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemoryRelation {
    pub kind: RelationKind,
    pub target: MemoryId,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum MemoryState {
    /// Evidence has been extracted but has not passed curation/promotion.
    /// Candidates are never returned by ordinary retrieval or context packs.
    Candidate,
    Active,
    /// A conflict was recorded but current evidence cannot yet resolve it.
    /// Disputed records stay inspectable/auditable but are not injected.
    Disputed,
    Superseded {
        by: MemoryId,
    },
    /// A time-bounded assertion has passed its validity window.
    Expired,
    Forgotten,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MemoryRecord {
    pub id: MemoryId,
    pub scope: MemoryScope,
    pub kind: MemoryKind,
    pub content: MemoryContent,
    #[serde(default)]
    pub tags: Vec<String>,
    #[serde(default)]
    pub evidence: Vec<MemoryEvidence>,
    #[serde(default)]
    pub relations: Vec<MemoryRelation>,
    #[serde(default = "default_confidence")]
    pub confidence: f32,
    pub state: MemoryState,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
    #[serde(default = "default_record_version")]
    pub version: u32,
}

fn default_confidence() -> f32 {
    1.0
}

fn default_record_version() -> u32 {
    1
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct NewMemory {
    pub scope: MemoryScope,
    #[serde(default)]
    pub kind: MemoryKind,
    pub content: MemoryContent,
    #[serde(default)]
    pub tags: Vec<String>,
    #[serde(default)]
    pub evidence: Vec<MemoryEvidence>,
    #[serde(default)]
    pub relations: Vec<MemoryRelation>,
    #[serde(default = "default_confidence")]
    pub confidence: f32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Tombstone {
    pub target: MemoryId,
    pub forgotten_at: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum OperationKind {
    Propose {
        memory_id: MemoryId,
    },
    Promote {
        memory_id: MemoryId,
    },
    Dispute {
        memory_id: MemoryId,
    },
    Expire {
        memory_id: MemoryId,
    },
    Remember {
        memory_id: MemoryId,
    },
    Correct {
        target: MemoryId,
        replacement: MemoryId,
    },
    Supersede {
        target: MemoryId,
        replacement: MemoryId,
    },
    Forget {
        target: MemoryId,
    },
    Tombstone {
        target: MemoryId,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemoryOperation {
    pub id: OperationId,
    pub operation: OperationKind,
    pub actor: String,
    pub at: DateTime<Utc>,
    pub revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Citation {
    pub memory_id: MemoryId,
    #[serde(default)]
    pub evidence_ids: Vec<EvidenceId>,
    pub quote: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemorySummary {
    pub id: SummaryId,
    pub query: String,
    pub text: String,
    pub citations: Vec<Citation>,
    pub source_revision: u64,
    pub generated_at: DateTime<Utc>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SearchHit {
    pub record: MemoryRecord,
    pub score: f32,
    pub matched_terms: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SearchResults {
    pub revision: u64,
    pub hits: Vec<SearchHit>,
    /// Unsafe legacy entries are excluded at read time as a second line of defence.
    pub suppressed_unsafe: usize,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ContextMemory {
    pub id: MemoryId,
    pub scope: MemoryScope,
    pub title: String,
    pub body: String,
    pub citation_index: usize,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ContextPacket {
    pub query: String,
    pub source_revision: u64,
    pub generated_at: DateTime<Utc>,
    pub summary: String,
    pub memories: Vec<ContextMemory>,
    pub citations: Vec<Citation>,
    pub omitted_for_budget: usize,
    pub suppressed_unsafe: usize,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CanonicalMemory {
    pub schema_version: u32,
    pub revision: u64,
    pub records: BTreeMap<MemoryId, MemoryRecord>,
    #[serde(default)]
    pub tombstones: BTreeMap<MemoryId, Tombstone>,
    /// One-way fingerprints of explicitly forgotten content.  These are kept
    /// after the source record is erased so an extraction job cannot quietly
    /// resurrect the same forgotten assertion on a later pass.
    #[serde(default)]
    pub suppressed_fingerprints: BTreeMap<String, DateTime<Utc>>,
    #[serde(default)]
    pub operations: Vec<MemoryOperation>,
}

impl Default for CanonicalMemory {
    fn default() -> Self {
        Self {
            schema_version: 1,
            revision: 0,
            records: BTreeMap::new(),
            tombstones: BTreeMap::new(),
            suppressed_fingerprints: BTreeMap::new(),
            operations: Vec::new(),
        }
    }
}
