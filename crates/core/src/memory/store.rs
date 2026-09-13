use std::collections::{BTreeSet, HashMap};
use std::fs::{self, File, OpenOptions};
use std::io;
use std::path::{Path, PathBuf};

use chrono::Utc;
use fs2::FileExt;
use rusqlite::{Connection, OptionalExtension, params};
use sha2::{Digest, Sha256};

#[cfg(test)]
use uuid::Uuid;

use super::model::*;

const MAX_MEMORY_BYTES: usize = 256 * 1024;
const MAX_STORE_BYTES: u64 = 64 * 1024 * 1024;

#[derive(Debug, thiserror::Error)]
pub enum MemoryError {
    #[error("memory I/O at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("invalid memory store at {path}: {reason}")]
    Corrupt { path: PathBuf, reason: String },
    #[error("memory store schema {found} is newer than supported schema {supported}")]
    UnsupportedSchema { found: u32, supported: u32 },
    #[error("memory revision conflict (expected {expected}, actual {actual})")]
    RevisionConflict { expected: u64, actual: u64 },
    #[error("memory not found: {0}")]
    NotFound(MemoryId),
    #[error("memory was already forgotten: {0}")]
    Forgotten(MemoryId),
    #[error("memory content was explicitly forgotten and may not be reintroduced")]
    SuppressedContent,
    #[error("memory content is empty or invalid: {0}")]
    Invalid(String),
}

#[derive(Debug, Clone)]
pub struct MemoryStore {
    root: PathBuf,
    database_path: PathBuf,
    /// Retained solely for a one-time migration of pre-SQLite installations.
    canonical_path: PathBuf,
    lock_path: PathBuf,
}

impl MemoryStore {
    /// Open the canonical store rooted under `<data_dir>/memory`.
    pub fn open(data_dir: impl AsRef<Path>) -> Result<Self, MemoryError> {
        let root = data_dir.as_ref().join("memory");
        fs::create_dir_all(&root).map_err(|source| io_error(&root, source))?;
        harden_directory(&root)?;
        let store = Self {
            database_path: root.join("memory.sqlite3"),
            canonical_path: root.join("canonical.json"),
            lock_path: root.join("canonical.lock"),
            root,
        };
        store.initialize_database()?;
        Ok(store)
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn revision(&self) -> Result<u64, MemoryError> {
        self.with_read(|store| Ok(store.revision))
    }

    pub fn remember(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        input: NewMemory,
    ) -> Result<MemoryRecord, MemoryError> {
        self.insert_record(expected_revision, actor, input, MemoryState::Active, false)
    }

    /// Persist a curator proposal without making it eligible for retrieval.
    /// Promotion is intentionally a separate audited transition so candidate
    /// extraction cannot silently become durable agent context.
    pub fn propose(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        input: NewMemory,
    ) -> Result<MemoryRecord, MemoryError> {
        self.insert_record(
            expected_revision,
            actor,
            input,
            MemoryState::Candidate,
            true,
        )
    }

    fn insert_record(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        input: NewMemory,
        state: MemoryState,
        proposed: bool,
    ) -> Result<MemoryRecord, MemoryError> {
        validate_new_memory(&input)?;
        let actor = validate_actor(actor)?;
        self.with_write(|mut store| {
            check_revision(&store, expected_revision)?;
            if store
                .suppressed_fingerprints
                .contains_key(&content_fingerprint(&input.scope, &input.content.body))
            {
                return Err(MemoryError::SuppressedContent);
            }
            let now = Utc::now();
            let record = MemoryRecord {
                id: MemoryId::new(),
                scope: input.scope,
                kind: input.kind,
                content: input.content,
                tags: normalize_tags(input.tags),
                evidence: normalize_evidence(input.evidence),
                relations: input.relations,
                confidence: input.confidence,
                state,
                created_at: now,
                updated_at: now,
                version: 1,
            };
            store.revision += 1;
            store.operations.push(MemoryOperation {
                id: OperationId::new(),
                operation: if proposed {
                    OperationKind::Propose {
                        memory_id: record.id.clone(),
                    }
                } else {
                    OperationKind::Remember {
                        memory_id: record.id.clone(),
                    }
                },
                actor,
                at: now,
                revision: store.revision,
            });
            store.records.insert(record.id.clone(), record.clone());
            Ok((store, record))
        })
    }

    /// Make one candidate eligible for ordinary retrieval. The caller must
    /// have observed its exact revision; stale promotion fails closed.
    pub fn promote(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        target: &MemoryId,
    ) -> Result<MemoryRecord, MemoryError> {
        self.transition_state(
            expected_revision,
            actor,
            target,
            MemoryState::Candidate,
            MemoryState::Active,
            |memory_id| OperationKind::Promote { memory_id },
        )
    }

    pub fn dispute(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        target: &MemoryId,
    ) -> Result<MemoryRecord, MemoryError> {
        self.transition_state(
            expected_revision,
            actor,
            target,
            MemoryState::Active,
            MemoryState::Disputed,
            |memory_id| OperationKind::Dispute { memory_id },
        )
    }

    pub fn expire(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        target: &MemoryId,
    ) -> Result<MemoryRecord, MemoryError> {
        self.transition_state(
            expected_revision,
            actor,
            target,
            MemoryState::Active,
            MemoryState::Expired,
            |memory_id| OperationKind::Expire { memory_id },
        )
    }

    fn transition_state<F>(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        target: &MemoryId,
        required: MemoryState,
        next: MemoryState,
        operation: F,
    ) -> Result<MemoryRecord, MemoryError>
    where
        F: FnOnce(MemoryId) -> OperationKind,
    {
        let actor = validate_actor(actor)?;
        self.with_write(|mut store| {
            check_revision(&store, expected_revision)?;
            if store.tombstones.contains_key(target) {
                return Err(MemoryError::Forgotten(target.clone()));
            }
            let now = Utc::now();
            let record = store
                .records
                .get_mut(target)
                .ok_or_else(|| MemoryError::NotFound(target.clone()))?;
            if record.state != required {
                return Err(MemoryError::Invalid(format!(
                    "memory {target} is not in the required lifecycle state"
                )));
            }
            record.state = next;
            record.updated_at = now;
            record.version = record.version.saturating_add(1);
            let updated = record.clone();
            store.revision += 1;
            store.operations.push(MemoryOperation {
                id: OperationId::new(),
                operation: operation(target.clone()),
                actor,
                at: now,
                revision: store.revision,
            });
            Ok((store, updated))
        })
    }

    pub fn correct(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        target: &MemoryId,
        mut replacement: NewMemory,
    ) -> Result<MemoryRecord, MemoryError> {
        replacement.relations.push(MemoryRelation {
            kind: RelationKind::Corrects,
            target: target.clone(),
        });
        self.replace(expected_revision, actor, target, replacement, true)
    }

    pub fn supersede(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        target: &MemoryId,
        mut replacement: NewMemory,
    ) -> Result<MemoryRecord, MemoryError> {
        replacement.relations.push(MemoryRelation {
            kind: RelationKind::Supersedes,
            target: target.clone(),
        });
        self.replace(expected_revision, actor, target, replacement, false)
    }

    fn replace(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        target: &MemoryId,
        replacement: NewMemory,
        correction: bool,
    ) -> Result<MemoryRecord, MemoryError> {
        validate_new_memory(&replacement)?;
        let actor = validate_actor(actor)?;
        self.with_write(|mut store| {
            check_revision(&store, expected_revision)?;
            if store.tombstones.contains_key(target) {
                return Err(MemoryError::Forgotten(target.clone()));
            }
            let original = store
                .records
                .get(target)
                .ok_or_else(|| MemoryError::NotFound(target.clone()))?;
            if !matches!(original.state, MemoryState::Active) {
                return Err(MemoryError::Invalid(format!(
                    "only active memory can be replaced: {target}"
                )));
            }
            if original.scope != replacement.scope {
                return Err(MemoryError::Invalid(
                    "replacement must retain the original scope".into(),
                ));
            }
            let now = Utc::now();
            let record = MemoryRecord {
                id: MemoryId::new(),
                scope: replacement.scope,
                kind: replacement.kind,
                content: replacement.content,
                tags: normalize_tags(replacement.tags),
                evidence: normalize_evidence(replacement.evidence),
                relations: replacement.relations,
                confidence: replacement.confidence,
                state: MemoryState::Active,
                created_at: now,
                updated_at: now,
                version: 1,
            };
            let original = store.records.get_mut(target).expect("checked above");
            original.state = MemoryState::Superseded {
                by: record.id.clone(),
            };
            original.updated_at = now;
            original.version = original.version.saturating_add(1);
            store.revision += 1;
            store.operations.push(MemoryOperation {
                id: OperationId::new(),
                operation: if correction {
                    OperationKind::Correct {
                        target: target.clone(),
                        replacement: record.id.clone(),
                    }
                } else {
                    OperationKind::Supersede {
                        target: target.clone(),
                        replacement: record.id.clone(),
                    }
                },
                actor,
                at: now,
                revision: store.revision,
            });
            store.records.insert(record.id.clone(), record.clone());
            Ok((store, record))
        })
    }

    pub fn forget(
        &self,
        expected_revision: Option<u64>,
        actor: &str,
        target: &MemoryId,
        reason: Option<String>,
    ) -> Result<Tombstone, MemoryError> {
        let actor = validate_actor(actor)?;
        self.with_write(|mut store| {
            check_revision(&store, expected_revision)?;
            if let Some(existing) = store.tombstones.get(target) {
                return Ok((store.clone(), existing.clone()));
            }
            let removed = store
                .records
                .remove(target)
                .ok_or_else(|| MemoryError::NotFound(target.clone()))?;
            let now = Utc::now();
            store.suppressed_fingerprints.insert(
                content_fingerprint(&removed.scope, &removed.content.body),
                now,
            );
            let tombstone = Tombstone {
                target: target.clone(),
                forgotten_at: now,
                reason,
            };
            store.tombstones.insert(target.clone(), tombstone.clone());
            // Remove dangling relations and evidence-derived summaries cannot
            // resurrect the forgotten content because summaries are not stored.
            for record in store.records.values_mut() {
                record
                    .relations
                    .retain(|relation| relation.target != *target);
            }
            // If the record had superseded another record, that inactive
            // predecessor still points to the forgotten content. Remove it as
            // well: keeping its content would allow an old fact to survive a
            // forget operation, while promoting it back to Active would
            // incorrectly undo a correction.
            let mut suppressed_predecessors = vec![target.clone()];
            if let Some(predecessor) = removed.relations.iter().find_map(|relation| {
                matches!(
                    relation.kind,
                    RelationKind::Corrects | RelationKind::Supersedes
                )
                .then(|| relation.target.clone())
            }) {
                if matches!(
                    store.records.get(&predecessor).map(|record| &record.state),
                    Some(MemoryState::Superseded { by }) if by == target
                ) {
                    if let Some(predecessor_record) = store.records.remove(&predecessor) {
                        store.suppressed_fingerprints.insert(
                            content_fingerprint(
                                &predecessor_record.scope,
                                &predecessor_record.content.body,
                            ),
                            now,
                        );
                    }
                    store.tombstones.insert(
                        predecessor.clone(),
                        Tombstone {
                            target: predecessor.clone(),
                            forgotten_at: now,
                            reason: Some("replacement was forgotten".into()),
                        },
                    );
                    suppressed_predecessors.push(predecessor);
                }
            }
            store.revision += 1;
            store.operations.push(MemoryOperation {
                id: OperationId::new(),
                operation: OperationKind::Forget {
                    target: target.clone(),
                },
                actor,
                at: now,
                revision: store.revision,
            });
            store.operations.push(MemoryOperation {
                id: OperationId::new(),
                operation: OperationKind::Tombstone {
                    target: target.clone(),
                },
                actor: "memory-store".into(),
                at: now,
                revision: store.revision,
            });
            for predecessor in suppressed_predecessors.into_iter().skip(1) {
                store.operations.push(MemoryOperation {
                    id: OperationId::new(),
                    operation: OperationKind::Tombstone {
                        target: predecessor,
                    },
                    actor: "memory-store".into(),
                    at: now,
                    revision: store.revision,
                });
            }
            Ok((store, tombstone))
        })
    }

    pub fn get(&self, id: &MemoryId) -> Result<Option<MemoryRecord>, MemoryError> {
        self.with_read(|store| {
            if store.tombstones.contains_key(id) {
                return Ok(None);
            }
            let result = store.records.get(id).filter(|record| {
                matches!(record.state, MemoryState::Active) && record_is_safe(record)
            });
            Ok(result.cloned())
        })
    }

    /// Inspect a non-forgotten record regardless of retrieval eligibility.
    /// This is for curator/audit workflows; ordinary `get`, search, and
    /// context injection deliberately expose only active records.
    pub fn get_any(&self, id: &MemoryId) -> Result<Option<MemoryRecord>, MemoryError> {
        self.with_read(|store| {
            Ok((!store.tombstones.contains_key(id))
                .then(|| store.records.get(id).cloned())
                .flatten())
        })
    }

    pub fn search(
        &self,
        query: &str,
        view: &MemoryView,
        limit: usize,
    ) -> Result<SearchResults, MemoryError> {
        let terms = tokenize(query);
        if terms.is_empty() || limit == 0 {
            return self.with_read(|store| {
                Ok(SearchResults {
                    revision: store.revision,
                    hits: Vec::new(),
                    suppressed_unsafe: 0,
                })
            });
        }
        // FTS5 narrows candidates and provides a semantic-ish relevance
        // signal; the canonical-record scorer below remains the safe lexical
        // fallback and enforces scope/state policy.
        let fts_scores = self.fts_scores(&terms, limit)?;
        self.with_read(|store| {
            let active: Vec<_> = store
                .records
                .values()
                .filter(|record| {
                    matches!(record.state, MemoryState::Active)
                        && record.scope.is_visible_in(view)
                        && !store.tombstones.contains_key(&record.id)
                })
                .collect();
            let mut document_frequency: HashMap<&str, usize> = HashMap::new();
            for term in &terms {
                let count = active
                    .iter()
                    .filter(|record| document_tokens(record).contains_key(term.as_str()))
                    .count();
                document_frequency.insert(term, count);
            }
            let mut hits = Vec::new();
            let mut suppressed_unsafe = 0;
            let document_count = active.len().max(1) as f32;
            for record in active {
                if !record_is_safe(record) {
                    suppressed_unsafe += 1;
                    continue;
                }
                let tokens = document_tokens(record);
                let mut score = 0.0f32;
                let mut matched = Vec::new();
                for term in &terms {
                    let frequency = *tokens.get(term).unwrap_or(&0) as f32;
                    if frequency == 0.0 {
                        continue;
                    }
                    matched.push(term.clone());
                    let containing = *document_frequency.get(term.as_str()).unwrap_or(&0) as f32;
                    let inverse = ((document_count + 1.0) / (containing + 1.0)).ln() + 1.0;
                    score += (1.0 + frequency.ln()) * inverse;
                }
                if score > 0.0 {
                    let title = record.content.title.to_ascii_lowercase();
                    score += matched
                        .iter()
                        .filter(|term| title.contains(term.as_str()))
                        .count() as f32
                        * 1.5;
                    score *= record.confidence.clamp(0.05, 1.0);
                    score += fts_scores.get(&record.id.0).copied().unwrap_or_default();
                    hits.push(SearchHit {
                        record: record.clone(),
                        score,
                        matched_terms: matched,
                    });
                }
            }
            hits.sort_by(|left, right| {
                right
                    .score
                    .total_cmp(&left.score)
                    .then_with(|| right.record.updated_at.cmp(&left.record.updated_at))
                    .then_with(|| left.record.id.cmp(&right.record.id))
            });
            hits.truncate(limit.min(1000));
            Ok(SearchResults {
                revision: store.revision,
                hits,
                suppressed_unsafe,
            })
        })
    }

    pub fn summarize(
        &self,
        query: &str,
        view: &MemoryView,
        limit: usize,
    ) -> Result<MemorySummary, MemoryError> {
        let results = self.search(query, view, limit)?;
        let citations: Vec<_> = results
            .hits
            .iter()
            .map(|hit| citation_for(&hit.record))
            .collect();
        let text = if citations.is_empty() {
            "No relevant durable memories found.".to_owned()
        } else {
            results
                .hits
                .iter()
                .enumerate()
                .map(|(index, hit)| {
                    format!(
                        "[{}] {} — {}",
                        index + 1,
                        hit.record.content.title.trim(),
                        hit.record.content.body.trim()
                    )
                })
                .collect::<Vec<_>>()
                .join("\n")
        };
        Ok(MemorySummary {
            id: SummaryId::new(),
            query: query.to_owned(),
            text,
            citations,
            source_revision: results.revision,
            generated_at: Utc::now(),
        })
    }

    pub fn context_packet(
        &self,
        query: &str,
        view: &MemoryView,
        limit: usize,
        max_bytes: usize,
    ) -> Result<ContextPacket, MemoryError> {
        let results = self.search(query, view, limit)?;
        // A query is a poor gate for profile and project context: "What's my
        // name?" need not share words with a stored identity fact.  Inject a
        // small, deterministic baseline of active User and Project records
        // before the query-ranked records.  Session records are intentionally
        // excluded here: they are already in the live transcript and must not
        // silently become a cross-session profile.
        let baseline = self.with_read(|store| {
            let mut records = store
                .records
                .values()
                .filter(|record| {
                    matches!(record.state, MemoryState::Active)
                        && matches!(
                            record.scope,
                            MemoryScope::User | MemoryScope::Project { .. }
                        )
                        && record.scope.is_visible_in(view)
                        && !store.tombstones.contains_key(&record.id)
                        && record_is_safe(record)
                })
                .cloned()
                .collect::<Vec<_>>();
            records.sort_by(|left, right| {
                let priority = |scope: &MemoryScope| match scope {
                    MemoryScope::User => 0,
                    MemoryScope::Project { .. } => 1,
                    MemoryScope::Session { .. } => 2,
                };
                priority(&left.scope)
                    .cmp(&priority(&right.scope))
                    .then_with(|| right.updated_at.cmp(&left.updated_at))
                    .then_with(|| left.id.cmp(&right.id))
            });
            Ok(records)
        })?;
        let mut memories = Vec::new();
        let mut citations = Vec::new();
        let mut used = 0usize;
        let mut omitted = 0usize;
        let mut records = baseline;
        let mut included_ids = records
            .iter()
            .map(|record| record.id.clone())
            .collect::<std::collections::BTreeSet<_>>();
        records.extend(results.hits.into_iter().filter_map(|hit| {
            included_ids
                .insert(hit.record.id.clone())
                .then_some(hit.record)
        }));
        for record in records.into_iter().take(limit.min(32)) {
            let memory = ContextMemory {
                id: record.id.clone(),
                scope: record.scope.clone(),
                title: record.content.title.clone(),
                body: record.content.body.clone(),
                citation_index: citations.len() + 1,
            };
            let cost = memory.title.len() + memory.body.len() + 64;
            if used.saturating_add(cost) > max_bytes {
                omitted += 1;
                continue;
            }
            used += cost;
            citations.push(citation_for(&record));
            memories.push(memory);
        }
        let summary = memories
            .iter()
            .map(|memory| format!("[{}] {}", memory.citation_index, memory.title.trim()))
            .collect::<Vec<_>>()
            .join("; ");
        Ok(ContextPacket {
            query: query.to_owned(),
            source_revision: results.revision,
            generated_at: Utc::now(),
            summary,
            memories,
            citations,
            omitted_for_budget: omitted,
            suppressed_unsafe: results.suppressed_unsafe,
        })
    }

    pub fn audit(
        &self,
        since_revision: u64,
        limit: usize,
    ) -> Result<Vec<MemoryOperation>, MemoryError> {
        self.with_read(|store| {
            Ok(store
                .operations
                .iter()
                .filter(|operation| operation.revision > since_revision)
                .take(limit.min(10_000))
                .cloned()
                .collect())
        })
    }

    fn with_read<T, F>(&self, action: F) -> Result<T, MemoryError>
    where
        F: FnOnce(&CanonicalMemory) -> Result<T, MemoryError>,
    {
        let lock = OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .open(&self.lock_path)
            .map_err(|source| io_error(&self.lock_path, source))?;
        harden_file(&lock, &self.lock_path)?;
        FileExt::lock_shared(&lock).map_err(|source| io_error(&self.lock_path, source))?;
        let store = self.load()?;
        action(&store)
    }

    fn with_write<T, F>(&self, action: F) -> Result<T, MemoryError>
    where
        F: FnOnce(CanonicalMemory) -> Result<(CanonicalMemory, T), MemoryError>,
    {
        let lock = OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .open(&self.lock_path)
            .map_err(|source| io_error(&self.lock_path, source))?;
        harden_file(&lock, &self.lock_path)?;
        FileExt::lock_exclusive(&lock).map_err(|source| io_error(&self.lock_path, source))?;
        let store = self.load()?;
        let (store, value) = action(store)?;
        self.commit(&store)?;
        Ok(value)
    }

    fn load(&self) -> Result<CanonicalMemory, MemoryError> {
        let connection = self.connection()?;
        let bytes: Option<Vec<u8>> = connection
            .query_row(
                "SELECT payload FROM canonical_state WHERE id = 1",
                [],
                |row| row.get(0),
            )
            .optional()
            .map_err(|error| sqlite_error(&self.database_path, error))?;
        let Some(bytes) = bytes else {
            return Ok(CanonicalMemory::default());
        };
        if bytes.len() as u64 > MAX_STORE_BYTES {
            return Err(MemoryError::Corrupt {
                path: self.database_path.clone(),
                reason: format!("store exceeds {MAX_STORE_BYTES} byte safety limit"),
            });
        }
        let store: CanonicalMemory =
            serde_json::from_slice(&bytes).map_err(|error| MemoryError::Corrupt {
                path: self.database_path.clone(),
                reason: error.to_string(),
            })?;
        if store.schema_version != 1 {
            return Err(MemoryError::UnsupportedSchema {
                found: store.schema_version,
                supported: 1,
            });
        }
        validate_store(&store).map_err(|reason| MemoryError::Corrupt {
            path: self.canonical_path.clone(),
            reason,
        })?;
        Ok(store)
    }

    fn commit(&self, store: &CanonicalMemory) -> Result<(), MemoryError> {
        let bytes = serde_json::to_vec_pretty(store).map_err(|error| MemoryError::Corrupt {
            path: self.canonical_path.clone(),
            reason: format!("serialize: {error}"),
        })?;
        let mut connection = self.connection()?;
        let transaction = connection
            .transaction()
            .map_err(|error| sqlite_error(&self.database_path, error))?;
        transaction
            .execute(
                "INSERT INTO canonical_state (id, schema_version, revision, payload) VALUES (1, ?1, ?2, ?3)
                 ON CONFLICT(id) DO UPDATE SET schema_version = excluded.schema_version, revision = excluded.revision, payload = excluded.payload",
                params![store.schema_version, store.revision, bytes],
            )
            .map_err(|error| sqlite_error(&self.database_path, error))?;
        transaction
            .execute("DELETE FROM memory_fts", [])
            .map_err(|error| sqlite_error(&self.database_path, error))?;
        for record in store
            .records
            .values()
            .filter(|record| matches!(record.state, MemoryState::Active))
        {
            transaction
                .execute(
                    "INSERT INTO memory_fts (memory_id, title, body, tags) VALUES (?1, ?2, ?3, ?4)",
                    params![
                        &record.id.0,
                        &record.content.title,
                        &record.content.body,
                        record.tags.join(" ")
                    ],
                )
                .map_err(|error| sqlite_error(&self.database_path, error))?;
        }
        transaction
            .commit()
            .map_err(|error| sqlite_error(&self.database_path, error))
    }

    fn connection(&self) -> Result<Connection, MemoryError> {
        Connection::open(&self.database_path)
            .map_err(|error| sqlite_error(&self.database_path, error))
    }

    fn fts_scores(
        &self,
        terms: &[String],
        limit: usize,
    ) -> Result<HashMap<String, f32>, MemoryError> {
        let query = terms.join(" OR ");
        let connection = self.connection()?;
        let mut statement = connection
            .prepare(
                "SELECT memory_id, bm25(memory_fts) FROM memory_fts WHERE memory_fts MATCH ?1 LIMIT ?2",
            )
            .map_err(|error| sqlite_error(&self.database_path, error))?;
        let rows = statement
            .query_map(params![query, limit.min(1000) as i64], |row| {
                let id: String = row.get(0)?;
                let rank: f64 = row.get(1)?;
                Ok((id, (1.0 / (1.0 + rank.abs())) as f32))
            })
            .map_err(|error| sqlite_error(&self.database_path, error))?;
        let mut scores = HashMap::new();
        for row in rows {
            let (id, score) = row.map_err(|error| sqlite_error(&self.database_path, error))?;
            scores.insert(id, score);
        }
        Ok(scores)
    }

    fn initialize_database(&self) -> Result<(), MemoryError> {
        let connection = self.connection()?;
        connection
            .execute_batch(
                "PRAGMA journal_mode = WAL;
                 PRAGMA foreign_keys = ON;
                 CREATE TABLE IF NOT EXISTS memory_schema (version INTEGER NOT NULL);
                 CREATE TABLE IF NOT EXISTS canonical_state (
                     id INTEGER PRIMARY KEY CHECK (id = 1), schema_version INTEGER NOT NULL,
                     revision INTEGER NOT NULL, payload BLOB NOT NULL
                 );
                 CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5(memory_id UNINDEXED, title, body, tags);",
            )
            .map_err(|error| sqlite_error(&self.database_path, error))?;
        let version: Option<u32> = connection
            .query_row("SELECT version FROM memory_schema LIMIT 1", [], |row| {
                row.get(0)
            })
            .optional()
            .map_err(|error| sqlite_error(&self.database_path, error))?;
        match version {
            Some(1) => {}
            Some(found) => {
                return Err(MemoryError::UnsupportedSchema {
                    found,
                    supported: 1,
                });
            }
            None => {
                connection
                    .execute("INSERT INTO memory_schema (version) VALUES (1)", [])
                    .map_err(|error| sqlite_error(&self.database_path, error))?;
            }
        }
        let state_exists: bool = connection
            .query_row(
                "SELECT EXISTS(SELECT 1 FROM canonical_state WHERE id = 1)",
                [],
                |row| row.get(0),
            )
            .map_err(|error| sqlite_error(&self.database_path, error))?;
        if !state_exists && self.canonical_path.exists() {
            let bytes = fs::read(&self.canonical_path)
                .map_err(|source| io_error(&self.canonical_path, source))?;
            let legacy: CanonicalMemory =
                serde_json::from_slice(&bytes).map_err(|error| MemoryError::Corrupt {
                    path: self.canonical_path.clone(),
                    reason: error.to_string(),
                })?;
            validate_store(&legacy).map_err(|reason| MemoryError::Corrupt {
                path: self.canonical_path.clone(),
                reason,
            })?;
            self.commit(&legacy)?;
            let migrated = self.root.join("canonical.json.migrated");
            fs::rename(&self.canonical_path, &migrated)
                .map_err(|source| io_error(&self.canonical_path, source))?;
        }
        Ok(())
    }
}

fn sqlite_error(path: &Path, error: rusqlite::Error) -> MemoryError {
    MemoryError::Corrupt {
        path: path.to_path_buf(),
        reason: format!("sqlite: {error}"),
    }
}

fn check_revision(store: &CanonicalMemory, expected: Option<u64>) -> Result<(), MemoryError> {
    if let Some(expected) = expected {
        if expected != store.revision {
            return Err(MemoryError::RevisionConflict {
                expected,
                actual: store.revision,
            });
        }
    }
    Ok(())
}

/// Canonical, one-way identity for content that a user explicitly asked us to
/// forget.  We intentionally retain no recoverable assertion text: a scope
/// prevents an unrelated project from being suppressed, while normalized body
/// text catches trivial capitalization and whitespace changes from a future
/// extraction pass.
fn content_fingerprint(scope: &MemoryScope, body: &str) -> String {
    let scope = match scope {
        MemoryScope::User => "user".to_owned(),
        MemoryScope::Project { project_id } => format!("project:{project_id}"),
        MemoryScope::Session { session_id } => format!("session:{session_id}"),
    };
    let normalized = body
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .to_lowercase();
    let mut digest = Sha256::new();
    digest.update(b"firmius-memory-forget-v1\\0");
    digest.update(scope.as_bytes());
    digest.update(b"\\0");
    digest.update(normalized.as_bytes());
    format!("{:x}", digest.finalize())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temporary_directory(label: &str) -> PathBuf {
        std::env::temp_dir().join(format!("firmius-memory-{label}-{}", Uuid::new_v4()))
    }

    fn memory(scope: MemoryScope, title: &str, body: &str) -> NewMemory {
        NewMemory {
            scope,
            kind: MemoryKind::Fact,
            content: MemoryContent {
                title: title.into(),
                body: body.into(),
            },
            tags: vec!["Rust".into(), "rust".into()],
            evidence: vec![MemoryEvidence {
                id: EvidenceId::new(),
                kind: EvidenceKind::UserStatement,
                locator: Some("session:test".into()),
                excerpt: body.into(),
                observed_at: Utc::now(),
            }],
            relations: Vec::new(),
            confidence: 0.9,
        }
    }

    #[test]
    fn persists_searches_and_scopes_canonical_records() {
        let directory = temporary_directory("search");
        let store = MemoryStore::open(&directory).unwrap();
        store
            .remember(
                Some(0),
                "agent:a",
                memory(MemoryScope::User, "Formatting", "Prefer rustfmt defaults"),
            )
            .unwrap();
        store
            .remember(
                Some(1),
                "agent:a",
                memory(
                    MemoryScope::Project {
                        project_id: "project-a".into(),
                    },
                    "Database",
                    "The project database is PostgreSQL",
                ),
            )
            .unwrap();
        store
            .remember(
                Some(2),
                "agent:a",
                memory(
                    MemoryScope::Project {
                        project_id: "project-b".into(),
                    },
                    "Other database",
                    "The other database is SQLite",
                ),
            )
            .unwrap();

        let reopened = MemoryStore::open(&directory).unwrap();
        assert_eq!(reopened.revision().unwrap(), 3);
        let results = reopened
            .search(
                "project database",
                &MemoryView {
                    include_user: true,
                    project_id: Some("project-a".into()),
                    session_id: None,
                },
                10,
            )
            .unwrap();
        assert_eq!(results.hits.len(), 1);
        assert_eq!(results.hits[0].record.content.title, "Database");
        assert_eq!(results.hits[0].record.tags, vec!["rust"]);
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn correction_suppresses_old_record_and_forget_removes_content() {
        let directory = temporary_directory("lifecycle");
        let store = MemoryStore::open(&directory).unwrap();
        let original = store
            .remember(
                Some(0),
                "agent:a",
                memory(MemoryScope::User, "Editor", "The editor is Vim"),
            )
            .unwrap();
        let replacement = store
            .correct(
                Some(1),
                "agent:a",
                &original.id,
                memory(MemoryScope::User, "Editor", "The editor is Helix"),
            )
            .unwrap();

        assert!(store.get(&original.id).unwrap().is_none());
        assert_eq!(
            store
                .search("Vim", &MemoryView::default(), 10)
                .unwrap()
                .hits
                .len(),
            0
        );
        assert_eq!(
            store
                .search("Helix", &MemoryView::default(), 10)
                .unwrap()
                .hits
                .len(),
            1
        );
        store
            .forget(
                Some(2),
                "agent:a",
                &replacement.id,
                Some("user request".into()),
            )
            .unwrap();
        assert!(store.get(&replacement.id).unwrap().is_none());
        let connection = Connection::open(directory.join("memory/memory.sqlite3")).unwrap();
        let persisted: Vec<u8> = connection
            .query_row(
                "SELECT payload FROM canonical_state WHERE id = 1",
                [],
                |row| row.get(0),
            )
            .unwrap();
        let persisted = String::from_utf8(persisted).unwrap();
        assert!(!persisted.contains("The editor is Helix"));
        assert!(!persisted.contains("The editor is Vim"));
        assert!(persisted.contains(&replacement.id.0));
        // Forgetting keeps only a one-way suppression fingerprint. A later
        // candidate/extraction pass cannot reintroduce the same fact merely
        // by changing its title, capitalization, or whitespace.
        assert!(matches!(
            store.remember(
                Some(3),
                "agent:a",
                memory(
                    MemoryScope::User,
                    "Different title",
                    "  THE   editor is helix  ",
                ),
            ),
            Err(MemoryError::SuppressedContent)
        ));
        assert!(matches!(
            store.remember(
                Some(1),
                "agent:a",
                memory(MemoryScope::User, "stale", "stale revision write"),
            ),
            Err(MemoryError::RevisionConflict {
                expected: 1,
                actual: 3
            })
        ));
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn candidate_lifecycle_is_audited_and_never_leaks_into_retrieval() {
        let directory = temporary_directory("candidate");
        let store = MemoryStore::open(&directory).unwrap();
        let candidate = store
            .propose(
                Some(0),
                "memory-curator",
                memory(MemoryScope::User, "Candidate", "Prefer cargo nextest"),
            )
            .unwrap();
        assert!(matches!(candidate.state, MemoryState::Candidate));
        assert!(
            store
                .search("nextest", &MemoryView::default(), 10)
                .unwrap()
                .hits
                .is_empty()
        );
        let active = store.promote(Some(1), "lead", &candidate.id).unwrap();
        assert!(matches!(active.state, MemoryState::Active));
        assert_eq!(
            store
                .search("nextest", &MemoryView::default(), 10)
                .unwrap()
                .hits
                .len(),
            1
        );
        let disputed = store.dispute(Some(2), "lead", &candidate.id).unwrap();
        assert!(matches!(disputed.state, MemoryState::Disputed));
        assert!(
            store
                .search("nextest", &MemoryView::default(), 10)
                .unwrap()
                .hits
                .is_empty()
        );
        let operations = store.audit(0, 10).unwrap();
        assert!(matches!(
            operations[0].operation,
            OperationKind::Propose { .. }
        ));
        assert!(matches!(
            operations[1].operation,
            OperationKind::Promote { .. }
        ));
        assert!(matches!(
            operations[2].operation,
            OperationKind::Dispute { .. }
        ));
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn credentials_and_legacy_content_are_retrievable_memory() {
        let directory = temporary_directory("secrets");
        let store = MemoryStore::open(&directory).unwrap();
        let credential_memory = memory(
            MemoryScope::User,
            "credential",
            "api_key=ABCDEFGHIJKLMNOPQRSTUVWX123456",
        );
        let credential = store
            .remember(Some(0), "agent:a", credential_memory)
            .unwrap();
        assert_eq!(
            credential.content.body,
            "api_key=ABCDEFGHIJKLMNOPQRSTUVWX123456"
        );

        let mut canonical = CanonicalMemory::default();
        let now = Utc::now();
        let record = MemoryRecord {
            id: MemoryId::new(),
            scope: MemoryScope::User,
            kind: MemoryKind::Fact,
            content: MemoryContent {
                title: "legacy".into(),
                body: "password=hunterhunter123".into(),
            },
            tags: Vec::new(),
            evidence: Vec::new(),
            relations: Vec::new(),
            confidence: 1.0,
            state: MemoryState::Active,
            created_at: now,
            updated_at: now,
            version: 1,
        };
        canonical.records.insert(record.id.clone(), record);
        // Import is one-time and only occurs before a database exists.
        fs::remove_dir_all(directory.join("memory")).unwrap();
        fs::create_dir_all(directory.join("memory")).unwrap();
        fs::write(
            directory.join("memory/canonical.json"),
            serde_json::to_vec(&canonical).unwrap(),
        )
        .unwrap();
        let migrated = MemoryStore::open(&directory).unwrap();
        assert!(directory.join("memory/canonical.json.migrated").exists());
        let results = migrated
            .search("legacy", &MemoryView::default(), 10)
            .unwrap();
        assert_eq!(results.hits.len(), 1);
        assert_eq!(results.suppressed_unsafe, 0);
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn summary_and_context_packet_cite_sources_and_respect_budget() {
        let directory = temporary_directory("context");
        let store = MemoryStore::open(&directory).unwrap();
        let record = store
            .remember(
                Some(0),
                "agent:a",
                memory(
                    MemoryScope::Session {
                        session_id: "session-a".into(),
                    },
                    "Testing policy",
                    "Run focused tests after every implementation change",
                ),
            )
            .unwrap();
        let view = MemoryView {
            include_user: false,
            project_id: None,
            session_id: Some("session-a".into()),
        };
        let summary = store.summarize("focused tests", &view, 5).unwrap();
        assert!(summary.text.contains("[1]"));
        assert_eq!(summary.citations[0].memory_id, record.id);

        let packet = store
            .context_packet("focused tests", &view, 5, 4096)
            .unwrap();
        assert_eq!(packet.memories.len(), 1);
        assert_eq!(packet.citations[0].memory_id, record.id);
        let tiny = store.context_packet("focused tests", &view, 5, 1).unwrap();
        assert!(tiny.memories.is_empty());
        assert_eq!(tiny.omitted_for_budget, 1);
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn context_packet_injects_user_and_project_baseline_without_query_overlap() {
        let directory = temporary_directory("baseline-context");
        let store = MemoryStore::open(&directory).unwrap();
        let user = store
            .remember(
                Some(0),
                "agent:a",
                memory(MemoryScope::User, "User identity", "Isaac"),
            )
            .unwrap();
        let project = store
            .remember(
                Some(1),
                "agent:a",
                memory(
                    MemoryScope::Project {
                        project_id: "project-a".into(),
                    },
                    "Project convention",
                    "Use cargo fmt before tests",
                ),
            )
            .unwrap();
        let session = store
            .remember(
                Some(2),
                "agent:a",
                memory(
                    MemoryScope::Session {
                        session_id: "session-a".into(),
                    },
                    "Ephemeral note",
                    "Current branch is experimental",
                ),
            )
            .unwrap();
        let packet = store
            .context_packet(
                "unrelated wording with no matching token",
                &MemoryView {
                    include_user: true,
                    project_id: Some("project-a".into()),
                    session_id: Some("session-a".into()),
                },
                8,
                4096,
            )
            .unwrap();
        let ids = packet
            .memories
            .iter()
            .map(|memory| &memory.id)
            .collect::<Vec<_>>();
        assert_eq!(ids, vec![&user.id, &project.id]);
        assert!(!ids.contains(&&session.id));
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn concurrent_writers_are_serialized_without_lost_updates() {
        let directory = temporary_directory("concurrency");
        let store = MemoryStore::open(&directory).unwrap();
        let writers: Vec<_> = (0..8)
            .map(|index| {
                let store = store.clone();
                std::thread::spawn(move || {
                    store
                        .remember(
                            None,
                            &format!("agent:{index}"),
                            memory(
                                MemoryScope::User,
                                &format!("writer {index}"),
                                &format!("concurrent durable fact {index}"),
                            ),
                        )
                        .unwrap();
                })
            })
            .collect();
        for writer in writers {
            writer.join().unwrap();
        }
        assert_eq!(store.revision().unwrap(), 8);
        let operations = store.audit(0, 100).unwrap();
        assert_eq!(operations.len(), 8);
        assert_eq!(
            store
                .search("concurrent durable fact", &MemoryView::default(), 100)
                .unwrap()
                .hits
                .len(),
            8
        );
        fs::remove_dir_all(directory).unwrap();
    }
}

fn validate_actor(actor: &str) -> Result<String, MemoryError> {
    let actor = actor.trim();
    if actor.is_empty() || actor.len() > 256 {
        return Err(MemoryError::Invalid("actor must be 1..=256 bytes".into()));
    }
    // Actors are authenticated runtime routing identifiers, not remembered
    // user content.
    Ok(actor.to_owned())
}

fn validate_new_memory(input: &NewMemory) -> Result<(), MemoryError> {
    if input.content.title.trim().is_empty() || input.content.title.len() > 512 {
        return Err(MemoryError::Invalid("title must be 1..=512 bytes".into()));
    }
    if input.content.body.trim().is_empty() {
        return Err(MemoryError::Invalid("body must not be empty".into()));
    }
    let size = input.content.title.len()
        + input.content.body.len()
        + input.tags.iter().map(String::len).sum::<usize>()
        + input
            .evidence
            .iter()
            .map(|evidence| {
                evidence.excerpt.len() + evidence.locator.as_deref().unwrap_or("").len()
            })
            .sum::<usize>();
    if size > MAX_MEMORY_BYTES {
        return Err(MemoryError::Invalid(format!(
            "memory exceeds {MAX_MEMORY_BYTES} byte limit"
        )));
    }
    if !input.confidence.is_finite() || !(0.0..=1.0).contains(&input.confidence) {
        return Err(MemoryError::Invalid(
            "confidence must be between 0 and 1".into(),
        ));
    }
    Ok(())
}

fn record_is_safe(record: &MemoryRecord) -> bool {
    validate_new_memory(&NewMemory {
        scope: record.scope.clone(),
        kind: record.kind.clone(),
        content: record.content.clone(),
        tags: record.tags.clone(),
        evidence: record.evidence.clone(),
        relations: record.relations.clone(),
        confidence: record.confidence,
    })
    .is_ok()
}

fn normalize_tags(tags: Vec<String>) -> Vec<String> {
    let mut unique = BTreeSet::new();
    for tag in tags.into_iter().take(128) {
        let tag = tag.trim().to_ascii_lowercase();
        if !tag.is_empty() && tag.len() <= 128 {
            unique.insert(tag);
        }
    }
    unique.into_iter().collect()
}

fn normalize_evidence(mut evidence: Vec<MemoryEvidence>) -> Vec<MemoryEvidence> {
    evidence.truncate(128);
    for item in &mut evidence {
        if item.id.0.trim().is_empty() {
            item.id = EvidenceId::new();
        }
    }
    evidence
}

fn tokenize(text: &str) -> Vec<String> {
    let mut terms = Vec::new();
    let mut current = String::new();
    for character in text.chars().flat_map(char::to_lowercase) {
        if character.is_alphanumeric() || character == '_' {
            current.push(character);
        } else if !current.is_empty() {
            if current.len() >= 2 {
                terms.push(std::mem::take(&mut current));
            } else {
                current.clear();
            }
        }
    }
    if current.len() >= 2 {
        terms.push(current);
    }
    terms.sort();
    terms.dedup();
    terms
}

fn document_tokens(record: &MemoryRecord) -> HashMap<String, usize> {
    let mut frequencies = HashMap::new();
    for token in tokenize(&format!(
        "{} {} {} {}",
        record.content.title,
        record.content.body,
        record.tags.join(" "),
        record
            .evidence
            .iter()
            .map(|evidence| evidence.excerpt.as_str())
            .collect::<Vec<_>>()
            .join(" ")
    )) {
        *frequencies.entry(token).or_default() += 1;
    }
    frequencies
}

fn citation_for(record: &MemoryRecord) -> Citation {
    Citation {
        memory_id: record.id.clone(),
        evidence_ids: record
            .evidence
            .iter()
            .map(|evidence| evidence.id.clone())
            .collect(),
        quote: record.content.body.chars().take(280).collect(),
    }
}

fn validate_store(store: &CanonicalMemory) -> Result<(), String> {
    for (id, record) in &store.records {
        if id != &record.id {
            return Err(format!(
                "record key {id} differs from embedded id {}",
                record.id
            ));
        }
        if store.tombstones.contains_key(id) {
            return Err(format!("forgotten record {id} still has canonical content"));
        }
    }
    if store
        .operations
        .iter()
        .any(|operation| operation.revision > store.revision)
    {
        return Err("operation revision exceeds store revision".into());
    }
    Ok(())
}

fn io_error(path: &Path, source: io::Error) -> MemoryError {
    MemoryError::Io {
        path: path.to_owned(),
        source,
    }
}

fn harden_file(file: &File, path: &Path) -> Result<(), MemoryError> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|source| io_error(path, source))?;
    }
    #[cfg(not(unix))]
    {
        let _ = (file, path);
    }
    Ok(())
}

fn harden_directory(path: &Path) -> Result<(), MemoryError> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))
            .map_err(|source| io_error(path, source))?;
    }
    #[cfg(not(unix))]
    {
        let _ = path;
    }
    Ok(())
}
