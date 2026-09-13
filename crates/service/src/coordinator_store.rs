//! Atomic persistence for the durable [`GoalCoordinator`] aggregate.

use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use firmius_core::{
    CoordinatorError, Goal, GoalActor, GoalCoordinator, GoalId, GoalStatus, GoalTransition,
    OutboxEntry, OutboxId, OutboxState,
};
use uuid::Uuid;

/// Persist a coordinator snapshot without deleting the last usable copy.
/// Unix can replace a file atomically with `rename`; Windows cannot rename
/// over an existing file, so move the old target aside and restore it if the
/// publish step fails.
#[cfg(not(windows))]
fn publish_replacement(tmp: &Path, path: &Path) -> Result<(), String> {
    fs::rename(tmp, path).map_err(|e| format!("replace coordinator: {e}"))
}

/// An error from snapshot publication. Once the replacement has been
/// renamed into place, the in-memory aggregate must not be rolled back: the
/// old bytes are no longer the snapshot on disk, even when the final fsync
/// reports an error.
#[derive(Debug)]
struct SaveError {
    message: String,
    published: bool,
}

#[cfg(windows)]
fn publish_replacement(tmp: &Path, path: &Path) -> Result<(), String> {
    if path.is_dir() {
        return Err(format!(
            "replace coordinator: target is a directory ({})",
            path.display()
        ));
    }
    let backup = path.with_extension(format!("json.bak.{}", Uuid::new_v4()));
    let had_target = match fs::rename(path, &backup) {
        Ok(()) => true,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => false,
        Err(error) => return Err(format!("backup coordinator: {error}")),
    };
    match fs::rename(tmp, path) {
        Ok(()) => {
            if had_target {
                let _ = fs::remove_file(&backup);
            }
            Ok(())
        }
        Err(publish_error) if had_target => match fs::rename(&backup, path) {
            Ok(()) => Err(format!(
                "replace coordinator: {publish_error} (previous file restored)"
            )),
            Err(restore_error) => Err(format!(
                "replace coordinator: {publish_error}; restore previous: {restore_error}"
            )),
        },
        Err(error) => Err(format!("replace coordinator: {error}")),
    }
}

fn fsync_file(path: &Path) -> Result<(), String> {
    let file = File::open(path).map_err(|e| format!("open coordinator for fsync: {e}"))?;
    file.sync_all()
        .map_err(|e| format!("fsync coordinator: {e}"))
}

pub struct CoordinatorStore {
    path: PathBuf,
    inner: Mutex<GoalCoordinator>,
}

impl CoordinatorStore {
    /// Load the durable coordinator and reconcile it for a new daemon
    /// lifetime.
    ///
    /// When an existing snapshot is found (a restart rather than a first
    /// launch), the persisted `daemon_epoch` is advanced by one and
    /// `GoalCoordinator::reconcile` fences every slot-holding run from any
    /// older epoch, releases their slots, requeues retry-safe work, and
    /// force-settles pending cancellations. Step, cost, budget, queue,
    /// idempotency, and outbox records are preserved byte-for-byte except for
    /// the deterministic fencing the aggregate applies.
    ///
    /// A corrupt or invalid snapshot fails closed: it is never overwritten
    /// with an empty coordinator, so the last usable bytes remain on disk for
    /// inspection or repair.
    pub fn load(root: PathBuf) -> Result<Self, String> {
        let path = root.join("coordinator.json");
        let coordinator = if path.exists() {
            let bytes = fs::read(&path).map_err(|e| format!("read coordinator: {e}"))?;
            let mut coordinator =
                GoalCoordinator::from_snapshot(&bytes).map_err(|e| e.to_string())?;
            Self::reconcile_on_restart(&mut coordinator)?;
            coordinator
        } else {
            let mut coordinator = GoalCoordinator::new();
            let legacy = root.join("goals.json");
            if legacy.exists() {
                let bytes = fs::read(&legacy).map_err(|e| format!("read goals: {e}"))?;
                let records: Vec<Goal> =
                    serde_json::from_slice(&bytes).map_err(|e| format!("decode goals: {e}"))?;
                for mut goal in records {
                    if coordinator.goals.contains_key(&goal.id) {
                        continue;
                    }
                    // The legacy goals file has no assignment, queue, run, or
                    // slot records. Execution-bound statuses therefore cannot
                    // be resumed safely (and cannot satisfy coordinator
                    // invariants). Retire those stale intents instead of
                    // treating a past-session label as live daemon work.
                    if matches!(
                        goal.status,
                        GoalStatus::Queued
                            | GoalStatus::Active
                            | GoalStatus::Waiting
                            | GoalStatus::Blocked
                            | GoalStatus::Cancelling
                    ) {
                        goal.transition(
                            GoalActor::System,
                            GoalTransition::Cancel {
                                reason:
                                    "retired while importing legacy goals without runtime state"
                                        .into(),
                            },
                        )
                        .map_err(|e| format!("retire imported goal {}: {e}", goal.id))?;
                    }
                    coordinator.goals.insert(goal.id, goal);
                }
                coordinator
                    .validate()
                    .map_err(|e| format!("validate imported goals: {e}"))?;
            }
            coordinator
        };
        let store = Self {
            path,
            inner: Mutex::new(coordinator),
        };
        store
            .save_locked(&store.inner.lock().unwrap())
            .map_err(|error| error.message)?;
        Ok(store)
    }

    /// Advance the durable ledger epoch by exactly one and fence all work
    /// that still belongs to an older epoch. `GoalCoordinator::reconcile`
    /// rejects an epoch that does not strictly increase, so this is monotonic
    /// across restarts: every restart persists a higher `daemon_epoch`.
    fn reconcile_on_restart(coordinator: &mut GoalCoordinator) -> Result<(), String> {
        let new_epoch = coordinator
            .daemon_epoch
            .checked_add(1)
            .ok_or_else(|| "restart epoch overflow".to_string())?;
        coordinator
            .reconcile(coordinator.revision, new_epoch)
            .map(|_| ())
            .map_err(|error| format!("reconcile coordinator on restart: {error}"))
    }

    fn save_locked(&self, coordinator: &GoalCoordinator) -> Result<(), SaveError> {
        self.save_locked_with(coordinator, fsync_file)
    }

    fn save_locked_with(
        &self,
        coordinator: &GoalCoordinator,
        sync_published: impl FnOnce(&Path) -> Result<(), String>,
    ) -> Result<(), SaveError> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent).map_err(|e| SaveError {
                message: format!("create coordinator directory: {e}"),
                published: false,
            })?;
        }
        let bytes = coordinator.to_snapshot().map_err(|e| SaveError {
            message: format!("encode coordinator: {e}"),
            published: false,
        })?;
        let tmp = self
            .path
            .with_extension(format!("json.tmp.{}", Uuid::new_v4()));
        let result = (|| {
            {
                let mut file = OpenOptions::new()
                    .write(true)
                    .create(true)
                    .truncate(true)
                    .open(&tmp)
                    .map_err(|e| SaveError {
                        message: format!("write coordinator: {e}"),
                        published: false,
                    })?;
                file.write_all(&bytes).map_err(|e| SaveError {
                    message: format!("write coordinator: {e}"),
                    published: false,
                })?;
                file.sync_all().map_err(|e| SaveError {
                    message: format!("fsync coordinator tmp: {e}"),
                    published: false,
                })?;
            }
            publish_replacement(&tmp, &self.path).map_err(|message| SaveError {
                message,
                published: false,
            })?;
            sync_published(&self.path).map_err(|message| SaveError {
                message,
                published: true,
            })
        })();
        if result.is_err() {
            let _ = fs::remove_file(&tmp);
        }
        result
    }

    pub fn with<R>(&self, f: impl FnOnce(&GoalCoordinator) -> R) -> R {
        let guard = self.inner.lock().unwrap();
        f(&guard)
    }

    pub fn mutate<R, F>(&self, f: F) -> Result<R, String>
    where
        F: FnOnce(&mut GoalCoordinator) -> Result<R, CoordinatorError>,
    {
        let mut guard = self.inner.lock().unwrap();
        let previous = guard.clone();
        match f(&mut guard) {
            Ok(result) => {
                if let Err(error) = self.save_locked(&guard) {
                    if !error.published {
                        *guard = previous;
                    }
                    return Err(error.message);
                }
                Ok(result)
            }
            Err(error) => {
                *guard = previous;
                Err(error.to_string())
            }
        }
    }

    /// Mutation that surfaces the domain error type. Save failures are
    /// represented as [`CoordinatorError::Snapshot`] so callers keep one
    /// error axis and can map it to a wire code.
    pub fn try_mutate<R, F>(&self, f: F) -> Result<R, CoordinatorError>
    where
        F: FnOnce(&mut GoalCoordinator) -> Result<R, CoordinatorError>,
    {
        let mut guard = self.inner.lock().unwrap();
        let previous = guard.clone();
        match f(&mut guard) {
            Ok(result) => {
                if let Err(error) = self.save_locked(&guard) {
                    if !error.published {
                        *guard = previous;
                    }
                    return Err(CoordinatorError::Snapshot(error.message));
                }
                Ok(result)
            }
            Err(error) => {
                *guard = previous;
                Err(error)
            }
        }
    }

    pub fn goal(&self, id: GoalId) -> Option<Goal> {
        self.with(|c| c.goals.get(&id).cloned())
    }

    pub fn snapshot(&self) -> GoalCoordinator {
        self.with(|c| c.clone())
    }

    /// Pending outbox entries awaiting delivery, replayable by a dispatch
    /// worker after a restart.
    pub fn pending_outbox(&self) -> Vec<OutboxEntry> {
        self.with(|c| c.pending_outbox().into_iter().cloned().collect())
    }

    /// Acknowledge one outbox delivery idempotently.
    ///
    /// Re-acknowledging an already acknowledged entry returns that entry
    /// without further mutation; superseded or unknown ids fail closed
    /// through [`GoalCoordinator::acknowledge_outbox`].
    pub fn acknowledge_outbox(&self, id: OutboxId) -> Result<OutboxEntry, String> {
        if let Some(entry) = self.with(|c| c.outbox.get(&id).cloned())
            && entry.state == OutboxState::Acknowledged
        {
            return Ok(entry);
        }
        self.mutate(|c| c.acknowledge_outbox(c.revision, id))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use firmius_core::{GoalActor, GoalOwner, GoalProvenance, GoalSource, GoalStatus};

    fn temp_root() -> PathBuf {
        // Tests run in parallel; wall-clock nanoseconds are not a uniqueness
        // boundary on every filesystem/clock combination. A UUID prevents
        // one test's cleanup from deleting another test's active root.
        std::env::temp_dir().join(format!("firmius-coordinator-{}", Uuid::new_v4()))
    }

    fn sample_goal() -> Goal {
        Goal::new(
            "persist me",
            vec!["file exists".into()],
            GoalOwner::User {
                user_id: "u".into(),
            },
            GoalProvenance {
                actor: GoalActor::User {
                    user_id: "u".into(),
                },
                source: GoalSource::UserRequest,
                created_at: chrono::Utc::now(),
            },
        )
        .unwrap()
    }

    #[test]
    fn snapshot_survives_reload() {
        let root = temp_root();
        fs::create_dir_all(&root).unwrap();
        let store = CoordinatorStore::load(root.clone()).unwrap();
        let goal = sample_goal();
        let id = goal.id;
        store
            .mutate(|c| {
                let expected = c.revision;
                c.register(expected, goal)
            })
            .unwrap();
        drop(store);
        let loaded = CoordinatorStore::load(root.clone()).unwrap();
        let restored = loaded.goal(id).unwrap();
        assert_eq!(restored.status, GoalStatus::Proposed);
        assert_eq!(restored.description, "persist me");
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn legacy_execution_bound_goals_are_retired_without_being_run() {
        let root = temp_root();
        fs::create_dir_all(&root).unwrap();
        let legacy = [
            GoalStatus::Queued,
            GoalStatus::Active,
            GoalStatus::Waiting,
            GoalStatus::Blocked,
            GoalStatus::Cancelling,
        ]
        .into_iter()
        .map(|status| {
            let mut goal = sample_goal();
            goal.status = status;
            goal
        })
        .collect::<Vec<_>>();
        fs::write(
            root.join("goals.json"),
            serde_json::to_vec_pretty(&legacy).unwrap(),
        )
        .unwrap();

        let store = CoordinatorStore::load(root.clone()).unwrap();
        let snapshot = store.snapshot();
        assert_eq!(snapshot.goals.len(), 5);
        assert!(
            snapshot
                .goals
                .values()
                .all(|goal| goal.status == GoalStatus::Cancelled)
        );
        assert!(snapshot.runs.is_empty());
        assert!(snapshot.queue.is_empty());
        assert!(snapshot.slots.is_empty());
        assert!(root.join("coordinator.json").exists());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn failed_mutation_does_not_persist() {
        let root = temp_root();
        fs::create_dir_all(&root).unwrap();
        let store = CoordinatorStore::load(root.clone()).unwrap();
        let err = store
            .mutate(|c| {
                c.register(99, sample_goal())?;
                Ok(())
            })
            .unwrap_err();
        assert!(err.contains("stale") || err.contains("revision"));
        assert!(store.snapshot().goals.is_empty());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn published_snapshot_is_retained_when_final_sync_fails() {
        let root = temp_root();
        fs::create_dir_all(&root).unwrap();
        let store = CoordinatorStore::load(root.clone()).unwrap();
        let mut updated = store.snapshot();
        let goal = sample_goal();
        let expected = updated.revision;
        updated.register(expected, goal).unwrap();

        let error = store
            .save_locked_with(&updated, |_| Err("injected fsync failure".into()))
            .unwrap_err();
        assert!(error.published);
        let bytes = fs::read(root.join("coordinator.json")).unwrap();
        let persisted = GoalCoordinator::from_snapshot(&bytes).unwrap();
        assert_eq!(persisted.revision, updated.revision);
        assert_eq!(persisted.goals.len(), 1);
        let _ = fs::remove_dir_all(root);
    }
}
