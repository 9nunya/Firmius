//! Coordination for native filesystem mutations issued by agent tools.
//!
//! An authority is process-shared (and may additionally use an OS writer
//! lease) while artifact mutations deliberately remain session-local.

use crate::workspace::{
    WorkspaceError, WorkspaceIdentity, WorkspaceLeaseError, WorkspaceWriterLease,
    normalize_write_path,
};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fmt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

/// The durable identity of one mutation attempt.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct EditAttemptId {
    pub session_id: String,
    pub agent_id: String,
    pub attempt_id: String,
}

/// A fully normalized, atomic set of native paths to mutate.
#[derive(Debug, Clone)]
pub struct EditAttempt {
    pub id: EditAttemptId,
    pub workspace: WorkspaceIdentity,
    pub paths: Vec<PathBuf>,
}

impl EditAttempt {
    /// Build an attempt from tool-facing relative paths. All aliases are
    /// canonicalized before any reservation is visible to another caller.
    pub fn native(
        workspace: WorkspaceIdentity,
        workdir: &Path,
        session_id: impl Into<String>,
        agent_id: impl Into<String>,
        attempt_id: impl Into<String>,
        paths: impl IntoIterator<Item = impl AsRef<str>>,
    ) -> Result<Self, EditCoordinationError> {
        workspace.revalidate()?;
        let mut paths = paths
            .into_iter()
            .map(|path| normalize_write_path(workdir, path.as_ref()))
            .collect::<Result<Vec<_>, _>>()?;
        paths.sort();
        paths.dedup();
        if paths.is_empty() {
            return Err(EditCoordinationError::EmptyAttempt);
        }
        Ok(Self {
            id: EditAttemptId {
                session_id: session_id.into(),
                agent_id: agent_id.into(),
                attempt_id: attempt_id.into(),
            },
            workspace,
            paths,
        })
    }

    /// Build from resource paths already normalized by a `Workspace`
    /// implementation. This is used for remote paths and session-qualified
    /// artifact resources as well as mixed atomic patches.
    pub fn normalized(
        workspace: WorkspaceIdentity,
        session_id: impl Into<String>,
        agent_id: impl Into<String>,
        attempt_id: impl Into<String>,
        paths: impl IntoIterator<Item = PathBuf>,
    ) -> Result<Self, EditCoordinationError> {
        workspace.revalidate()?;
        let mut paths = paths.into_iter().collect::<Vec<_>>();
        paths.sort();
        paths.dedup();
        if paths.is_empty() {
            return Err(EditCoordinationError::EmptyAttempt);
        }
        Ok(Self {
            id: EditAttemptId {
                session_id: session_id.into(),
                agent_id: agent_id.into(),
                attempt_id: attempt_id.into(),
            },
            workspace,
            paths,
        })
    }
}

/// Qualified owner information returned for an active overlap.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EditConflict {
    pub path: PathBuf,
    pub holder: EditAttemptId,
}

#[derive(Debug, thiserror::Error)]
pub enum EditCoordinationError {
    #[error("edit attempt contains no native paths")]
    EmptyAttempt,
    #[error("edit attempt cancelled while waiting for its commit lease")]
    Cancelled,
    #[error("edit attempt is already active: {0:?}")]
    AlreadyActive(EditAttemptId),
    #[error("edit attempt was already committed: {0:?}")]
    AlreadyCommitted(EditAttemptId),
    #[error(
        "native edit conflict at {path}: held by session {holder_session}, agent {holder_agent}, attempt {holder_attempt}"
    )]
    Conflict {
        path: PathBuf,
        holder_session: String,
        holder_agent: String,
        holder_attempt: String,
    },
    #[error("workspace changed while the edit lease was held at {path}")]
    StaleGeneration { path: PathBuf },
    #[error("edit authority journal error: {0}")]
    Journal(String),
    #[error(transparent)]
    Workspace(#[from] WorkspaceError),
    #[error(transparent)]
    WriterLease(#[from] WorkspaceLeaseError),
}

impl From<EditConflict> for EditCoordinationError {
    fn from(value: EditConflict) -> Self {
        Self::Conflict {
            path: value.path,
            holder_session: value.holder.session_id,
            holder_agent: value.holder.agent_id,
            holder_attempt: value.holder.attempt_id,
        }
    }
}

/// Injectable authority used by `ToolContext` and direct Agent edit history.
pub trait EditAuthority: Send + Sync {
    /// Register before reading/preflighting content. Registration does not
    /// reserve paths, so peers remain eligible until one successfully commits.
    fn prepare(&self, attempt: EditAttempt) -> Result<EditAttemptGuard, EditCoordinationError>;
}

#[derive(Default)]
struct AuthorityState {
    sequence: u64,
    active: HashMap<ResourceKey, AttemptKey>,
    active_attempts: HashSet<AttemptKey>,
    prepared_attempts: HashSet<AttemptKey>,
    committed_attempts: HashSet<AttemptKey>,
    generations: HashMap<ResourceKey, u64>,
}

#[derive(Clone, PartialEq, Eq, Hash)]
struct AttemptKey {
    daemon_epoch: String,
    id: EditAttemptId,
}

#[derive(Clone, PartialEq, Eq, Hash)]
struct ResourceKey {
    workspace: String,
    path: PathBuf,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppliedEdit {
    pub daemon_epoch: String,
    /// Global authority sequence assigned to this successful commit.
    pub sequence: u64,
    pub id: EditAttemptId,
    pub workspace: String,
    pub paths: Vec<PathBuf>,
    pub generations: Vec<u64>,
}

struct AuthorityInner {
    state: Mutex<AuthorityState>,
    changed: Arc<tokio::sync::Notify>,
    journal: Option<PathBuf>,
    lease_dir: Option<PathBuf>,
    daemon_epoch: String,
}

/// Safe default authority for embedded and test runtimes.
#[derive(Clone)]
pub struct InProcessEditAuthority {
    inner: Arc<AuthorityInner>,
}

impl fmt::Debug for InProcessEditAuthority {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("InProcessEditAuthority")
            .field("journal", &self.inner.journal)
            .field("daemon_epoch", &self.inner.daemon_epoch)
            .finish_non_exhaustive()
    }
}

impl Default for InProcessEditAuthority {
    fn default() -> Self {
        Self::new()
    }
}

impl InProcessEditAuthority {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(AuthorityInner {
                state: Mutex::new(AuthorityState::default()),
                changed: Arc::new(tokio::sync::Notify::new()),
                journal: None,
                lease_dir: None,
                daemon_epoch: uuid::Uuid::new_v4().to_string(),
            }),
        }
    }

    /// Open an append-only applied-generation journal. Its parent also owns
    /// the protected OS writer leases used to fence multiple daemon processes.
    pub fn with_journal(path: impl Into<PathBuf>) -> Result<Self, EditCoordinationError> {
        Self::with_journal_and_epoch(path, uuid::Uuid::new_v4().to_string())
    }

    pub fn with_journal_and_epoch(
        path: impl Into<PathBuf>,
        daemon_epoch: impl Into<String>,
    ) -> Result<Self, EditCoordinationError> {
        let path = path.into();
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|error| EditCoordinationError::Journal(error.to_string()))?;
        }
        let mut state = AuthorityState::default();
        if let Ok(contents) = std::fs::read_to_string(&path) {
            for line in contents.lines().filter(|line| !line.trim().is_empty()) {
                let record: AppliedEdit = serde_json::from_str(line)
                    .map_err(|error| EditCoordinationError::Journal(error.to_string()))?;
                state.sequence = state.sequence.max(record.sequence);
                state.committed_attempts.insert(AttemptKey {
                    daemon_epoch: record.daemon_epoch,
                    id: record.id,
                });
                for (path, generation) in record.paths.into_iter().zip(record.generations) {
                    state.generations.insert(
                        ResourceKey {
                            workspace: record.workspace.clone(),
                            path,
                        },
                        generation,
                    );
                }
            }
        }
        let lease_dir = path
            .parent()
            .unwrap_or_else(|| Path::new("."))
            .join("edit-writer-leases");
        Ok(Self {
            inner: Arc::new(AuthorityInner {
                state: Mutex::new(state),
                changed: Arc::new(tokio::sync::Notify::new()),
                journal: Some(path),
                lease_dir: Some(lease_dir),
                daemon_epoch: daemon_epoch.into(),
            }),
        })
    }

    pub fn prepare(&self, attempt: EditAttempt) -> Result<EditAttemptGuard, EditCoordinationError> {
        <Self as EditAuthority>::prepare(self, attempt)
    }

    pub fn held_count(&self) -> usize {
        self.inner.state.lock().unwrap().active_attempts.len()
    }
}

impl EditAuthority for InProcessEditAuthority {
    fn prepare(&self, attempt: EditAttempt) -> Result<EditAttemptGuard, EditCoordinationError> {
        attempt.workspace.revalidate()?;
        let key = AttemptKey {
            daemon_epoch: self.inner.daemon_epoch.clone(),
            id: attempt.id.clone(),
        };
        let mut state = self.inner.state.lock().unwrap();
        if state.committed_attempts.contains(&key) {
            return Err(EditCoordinationError::AlreadyCommitted(attempt.id));
        }
        if state.active_attempts.contains(&key) || state.prepared_attempts.contains(&key) {
            return Err(EditCoordinationError::AlreadyActive(attempt.id));
        }
        let begin_sequence = state.sequence;
        state.prepared_attempts.insert(key.clone());
        drop(state);
        Ok(EditAttemptGuard {
            authority: self.inner.clone(),
            attempt: Some(attempt),
            key,
            begin_sequence,
        })
    }
}

/// A registered attempt which has not yet reserved its paths. Drop removes
/// the registration; no generation or winner is produced.
pub struct EditAttemptGuard {
    authority: Arc<AuthorityInner>,
    attempt: Option<EditAttempt>,
    key: AttemptKey,
    begin_sequence: u64,
}

impl EditAttemptGuard {
    pub fn attempt(&self) -> &EditAttempt {
        self.attempt.as_ref().expect("live edit attempt")
    }

    pub fn begin_sequence(&self) -> u64 {
        self.begin_sequence
    }

    /// Wait for an active overlapping holder. If it drops, this registered
    /// attempt remains eligible; if it commits, generation CAS rejects this
    /// attempt as stale. Cancellation drops the registration cleanly.
    pub async fn acquire_wait(
        mut self,
        cancellation: &tokio_util::sync::CancellationToken,
    ) -> Result<EditLease, EditCoordinationError> {
        loop {
            let authority = self.authority.clone();
            let notified = authority.changed.notified();
            match self.try_acquire() {
                Ok(Some(lease)) => return Ok(lease),
                Ok(None) => {
                    tokio::select! {
                        _ = notified => {}
                        _ = cancellation.cancelled() => {
                            return Err(EditCoordinationError::Cancelled);
                        }
                    }
                }
                Err(EditCoordinationError::Conflict { .. }) => {
                    tokio::select! {
                        _ = notified => {}
                        _ = cancellation.cancelled() => {
                            return Err(EditCoordinationError::Cancelled);
                        }
                    }
                }
                Err(error) => return Err(error),
            }
        }
    }

    /// Atomically reserve every path after preparation.
    pub fn acquire(mut self) -> Result<EditLease, EditCoordinationError> {
        self.try_acquire()?
            .ok_or_else(|| unreachable!("acquire cannot defer"))
    }

    fn try_acquire(&mut self) -> Result<Option<EditLease>, EditCoordinationError> {
        let attempt = self.attempt.as_ref().expect("live edit attempt");
        attempt.workspace.revalidate()?;
        let workspace = attempt.workspace.lease_key();
        let resources = attempt
            .paths
            .iter()
            .cloned()
            .map(|path| ResourceKey {
                workspace: workspace.clone(),
                path,
            })
            .collect::<Vec<_>>();
        {
            let mut state = self.authority.state.lock().unwrap();
            for resource in &resources {
                if state.generations.get(resource).copied().unwrap_or(0) > self.begin_sequence {
                    return Err(EditCoordinationError::StaleGeneration {
                        path: resource.path.clone(),
                    });
                }
                if let Some(holder) = state.active.get(resource) {
                    if holder.id == self.key.id {
                        return Err(EditCoordinationError::AlreadyActive(self.key.id.clone()));
                    }
                    return Err(EditConflict {
                        path: resource.path.clone(),
                        holder: holder.id.clone(),
                    }
                    .into());
                }
            }
            for resource in &resources {
                state.active.insert(resource.clone(), self.key.clone());
            }
            state.active_attempts.insert(self.key.clone());
            state.prepared_attempts.remove(&self.key);
        }
        let writer_lease = match &self.authority.lease_dir {
            Some(directory) => match WorkspaceWriterLease::acquire(&attempt.workspace, directory) {
                Ok(lease) => Some(lease),
                Err(error) => {
                    release(&self.authority, &self.key, &resources);
                    return Err(error.into());
                }
            },
            None => None,
        };
        let attempt = self.attempt.take().expect("live edit attempt");
        Ok(Some(EditLease {
            authority: self.authority.clone(),
            attempt: Some(attempt),
            key: self.key.clone(),
            resources,
            begin_sequence: self.begin_sequence,
            writer_lease,
        }))
    }
}

impl Drop for EditAttemptGuard {
    fn drop(&mut self) {
        if self.attempt.is_some() {
            self.authority
                .state
                .lock()
                .unwrap()
                .prepared_attempts
                .remove(&self.key);
        }
    }
}

/// RAII reservation. Dropping without `commit` releases every path and does
/// not create a winner or journal record.
pub struct EditLease {
    authority: Arc<AuthorityInner>,
    attempt: Option<EditAttempt>,
    key: AttemptKey,
    resources: Vec<ResourceKey>,
    begin_sequence: u64,
    writer_lease: Option<WorkspaceWriterLease>,
}

impl EditLease {
    pub fn attempt(&self) -> &EditAttempt {
        self.attempt.as_ref().expect("live edit lease")
    }

    pub fn paths(&self) -> &[PathBuf] {
        &self.attempt().paths
    }

    /// CAS revalidation intended to run immediately before filesystem writes.
    pub fn revalidate(&self) -> Result<(), EditCoordinationError> {
        self.attempt().workspace.revalidate()?;
        let state = self.authority.state.lock().unwrap();
        for (resource, path) in self.resources.iter().zip(self.paths()) {
            if state.generations.get(resource).copied().unwrap_or(0) > self.begin_sequence
                || state.active.get(resource) != Some(&self.key)
            {
                return Err(EditCoordinationError::StaleGeneration { path: path.clone() });
            }
        }
        Ok(())
    }

    /// Mark a successful filesystem transaction as the winner. Journal
    /// persistence happens before the in-memory commit becomes visible.
    pub fn commit(mut self) -> Result<AppliedEdit, EditCoordinationError> {
        self.revalidate()?;
        let attempt = self.attempt.as_ref().expect("live edit lease");
        // This state mutex is also the journal append mutex. It makes the
        // global sequence, durable line order, and visible generations one
        // indivisible authority operation for concurrent commits.
        let mut state = self.authority.state.lock().unwrap();
        for resource in &self.resources {
            if state.generations.get(resource).copied().unwrap_or(0) > self.begin_sequence
                || state.active.get(resource) != Some(&self.key)
            {
                return Err(EditCoordinationError::StaleGeneration {
                    path: resource.path.clone(),
                });
            }
        }
        let sequence = state.sequence.saturating_add(1);
        let generations = vec![sequence; self.resources.len()];
        let applied = AppliedEdit {
            daemon_epoch: self.key.daemon_epoch.clone(),
            sequence,
            id: attempt.id.clone(),
            workspace: attempt.workspace.lease_key(),
            paths: attempt.paths.clone(),
            generations: generations.clone(),
        };
        if let Some(path) = &self.authority.journal {
            use std::io::Write;
            let mut file = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(path)
                .map_err(|error| EditCoordinationError::Journal(error.to_string()))?;
            serde_json::to_writer(&mut file, &applied)
                .map_err(|error| EditCoordinationError::Journal(error.to_string()))?;
            file.write_all(b"\n")
                .and_then(|_| file.sync_data())
                .map_err(|error| EditCoordinationError::Journal(error.to_string()))?;
        }
        state.sequence = sequence;
        for (resource, generation) in self.resources.iter().cloned().zip(generations) {
            state.generations.insert(resource.clone(), generation);
            state.active.remove(&resource);
        }
        state.active_attempts.remove(&self.key);
        state.committed_attempts.insert(self.key.clone());
        drop(state);
        self.authority.changed.notify_waiters();
        self.attempt = None;
        self.writer_lease.take();
        Ok(applied)
    }
}

impl Drop for EditLease {
    fn drop(&mut self) {
        if let Some(attempt) = &self.attempt {
            let _ = attempt;
            release(&self.authority, &self.key, &self.resources);
        }
    }
}

fn release(inner: &AuthorityInner, id: &AttemptKey, resources: &[ResourceKey]) {
    let mut state = inner.state.lock().unwrap();
    for resource in resources {
        if state.active.get(resource) == Some(id) {
            state.active.remove(resource);
        }
    }
    state.active_attempts.remove(id);
    drop(state);
    inner.changed.notify_waiters();
}
