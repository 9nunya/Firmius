//! Deterministic concurrency checks for cross-session native edit authority.
//!
//! These tests coordinate with held leases and barriers rather than sleeps.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Barrier};

use firmius_core::edit_coordination::{EditAttempt, EditCoordinationError, InProcessEditAuthority};
use firmius_core::workspace::WorkspaceIdentity;

struct TestWorkspace(PathBuf);

impl TestWorkspace {
    fn new(label: &str) -> Self {
        let path = std::env::temp_dir().join(format!(
            "firmius-edit-concurrency-{label}-{}-{}",
            std::process::id(),
            uuid::Uuid::new_v4()
        ));
        std::fs::create_dir_all(&path).expect("create test workspace");
        Self(path)
    }

    fn path(&self) -> &Path {
        &self.0
    }

    fn identity(&self) -> WorkspaceIdentity {
        WorkspaceIdentity::local(&self.0).expect("identify test workspace")
    }
}

#[test]
fn remote_authority_coordinates_matching_target_and_root_but_not_distinct_targets() {
    let local_workdir = TestWorkspace::new("remote-path-normalization");
    let authority = InProcessEditAuthority::new();
    let root = WorkspaceIdentity::remote("build-host", "/srv/project/")
        .expect("normalize remote workspace");
    let held = authority
        .prepare(remote_attempt(
            &local_workdir,
            root,
            "session-a",
            "held",
            &["src/lib.rs"],
        ))
        .expect("prepare remote holder")
        .acquire()
        .expect("acquire remote holder");

    let alias = WorkspaceIdentity::remote("build-host", "/srv/./project")
        .expect("normalize equivalent remote root");
    let error = match authority
        .prepare(remote_attempt(
            &local_workdir,
            alias,
            "session-b",
            "overlap",
            &["src/lib.rs"],
        ))
        .expect("prepare matching remote contender")
        .acquire()
    {
        Ok(_) => panic!("matching remote target/root must conflict"),
        Err(error) => error,
    };
    assert!(matches!(error, EditCoordinationError::Conflict { .. }));

    let distinct_target = WorkspaceIdentity::remote("other-host", "/srv/project")
        .expect("identify distinct remote target");
    let independent = authority
        .prepare(remote_attempt(
            &local_workdir,
            distinct_target,
            "session-c",
            "independent",
            &["src/lib.rs"],
        ))
        .expect("prepare distinct remote target")
        .acquire()
        .expect("distinct SSH targets use separate authority namespaces");
    drop((held, independent));
}

impl Drop for TestWorkspace {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).ok();
    }
}

fn attempt(
    workspace: &TestWorkspace,
    session: &str,
    agent: &str,
    attempt_id: impl Into<String>,
    paths: &[&str],
) -> EditAttempt {
    EditAttempt::native(
        workspace.identity(),
        workspace.path(),
        session,
        agent,
        attempt_id,
        paths.iter().copied(),
    )
    .expect("valid native edit attempt")
}

fn remote_attempt(
    local_workdir: &TestWorkspace,
    workspace: WorkspaceIdentity,
    session: &str,
    attempt_id: &str,
    paths: &[&str],
) -> EditAttempt {
    // `EditAttempt::native` accepts normalized tool-facing paths. A remote
    // identity provides the shared coordination namespace while the absolute
    // workdir supplies deterministic path normalization without SSH I/O.
    EditAttempt::native(
        workspace,
        local_workdir.path(),
        session,
        "agent",
        attempt_id,
        paths.iter().copied(),
    )
    .expect("valid remote edit attempt")
}

#[test]
fn canonical_root_aliases_conflict_across_sessions_even_for_the_same_agent_id() {
    let workspace = TestWorkspace::new("aliases");
    let authority = InProcessEditAuthority::new();
    let held = authority
        .prepare(attempt(
            &workspace,
            "session-a",
            "shared-agent-name",
            "winner",
            &["nested/note.txt"],
        ))
        .expect("prepare first attempt")
        .acquire()
        .expect("first attempt wins the active lease");

    // `root/.` is a distinct spelling of the same canonical root. The
    // qualified attempt identity must still preserve the session boundary.
    let alias = workspace.path().join(".");
    let contender = EditAttempt::native(
        WorkspaceIdentity::local(&alias).expect("identify root alias"),
        &alias,
        "session-b",
        "shared-agent-name",
        "loser",
        ["nested/note.txt"],
    )
    .expect("normalize alias attempt");
    let error = match authority
        .prepare(contender)
        .expect("prepare alias contender")
        .acquire()
    {
        Ok(_) => panic!("canonical aliases must contend for the same path"),
        Err(error) => error,
    };
    assert!(matches!(
        error,
        EditCoordinationError::Conflict {
            holder_session,
            holder_agent,
            holder_attempt,
            ..
        } if holder_session == "session-a"
            && holder_agent == "shared-agent-name"
            && holder_attempt == "winner"
    ));
    assert_eq!(authority.held_count(), 1);
    drop(held);
    assert_eq!(authority.held_count(), 0);
}

#[test]
fn disjoint_edits_coexist_and_multi_path_reservation_is_all_or_nothing() {
    let workspace = TestWorkspace::new("multi-path");
    let authority = InProcessEditAuthority::new();
    let left = authority
        .prepare(attempt(
            &workspace,
            "session-a",
            "agent-a",
            "left",
            &["a.txt", "b.txt"],
        ))
        .expect("prepare first atomic path set")
        .acquire()
        .expect("reserve first atomic path set");
    let right = authority
        .prepare(attempt(
            &workspace,
            "session-b",
            "agent-b",
            "right",
            &["c.txt"],
        ))
        .expect("prepare disjoint edit")
        .acquire()
        .expect("disjoint edit may coexist");
    assert_eq!(authority.held_count(), 2);

    let error = match authority
        .prepare(attempt(
            &workspace,
            "session-c",
            "agent-c",
            "overlap",
            &["b.txt", "d.txt"],
        ))
        .expect("prepare overlapping atomic path set")
        .acquire()
    {
        Ok(_) => panic!("one overlapping path must reject the entire reservation"),
        Err(error) => error,
    };
    assert!(matches!(error, EditCoordinationError::Conflict { .. }));

    // The failed multi-path begin must not retain its otherwise-free path.
    let d = authority
        .prepare(attempt(
            &workspace,
            "session-d",
            "agent-d",
            "prove-d-free",
            &["d.txt"],
        ))
        .expect("prepare d.txt proof")
        .acquire()
        .expect("failed atomic reservation did not retain d.txt");
    assert_eq!(authority.held_count(), 3);
    drop((left, right, d));
    assert_eq!(authority.held_count(), 0);
}

#[test]
fn failure_releases_without_winning_and_committed_attempts_are_idempotent() {
    let workspace = TestWorkspace::new("failure");
    let authority = InProcessEditAuthority::new();

    let failed: Result<(), &'static str> = (|| {
        let _lease = authority
            .prepare(attempt(
                &workspace,
                "session-a",
                "agent-a",
                "failed",
                &["note.txt"],
            ))
            .expect("prepare failed attempt")
            .acquire()
            .expect("reserve failed attempt");
        Err("injected mutation failure")
    })();
    assert_eq!(failed, Err("injected mutation failure"));
    assert_eq!(authority.held_count(), 0);

    let applied = authority
        .prepare(attempt(
            &workspace,
            "session-b",
            "agent-b",
            "winner",
            &["note.txt"],
        ))
        .expect("prepare winner after failure")
        .acquire()
        .expect("failure released the path")
        .commit()
        .expect("commit winner");
    assert_eq!(applied.generations, vec![1]);
    assert_eq!(authority.held_count(), 0);

    let duplicate = match authority.prepare(attempt(
        &workspace,
        "session-b",
        "agent-b",
        "winner",
        &["note.txt"],
    )) {
        Ok(_) => panic!("the same qualified attempt must never apply twice"),
        Err(error) => error,
    };
    assert!(matches!(
        duplicate,
        EditCoordinationError::AlreadyCommitted(id)
            if id.session_id == "session-b"
                && id.agent_id == "agent-b"
                && id.attempt_id == "winner"
    ));

    let later = authority
        .prepare(attempt(
            &workspace,
            "session-c",
            "agent-c",
            "later",
            &["note.txt"],
        ))
        .expect("prepare fresh attempt after winner")
        .acquire()
        .expect("a fresh attempt after the winner may edit sequentially")
        .commit()
        .expect("commit later edit");
    assert_eq!(later.generations, vec![2]);
}

#[test]
fn earlier_registration_is_stale_after_a_peer_applies_but_survives_peer_failure() {
    let workspace = TestWorkspace::new("registration-order");
    let authority = InProcessEditAuthority::new();

    let early = authority
        .prepare(attempt(
            &workspace,
            "session-early",
            "agent-early",
            "early",
            &["note.txt"],
        ))
        .expect("register early attempt before preflight");
    let winner = authority
        .prepare(attempt(
            &workspace,
            "session-winner",
            "agent-winner",
            "winner",
            &["note.txt"],
        ))
        .expect("register peer attempt")
        .acquire()
        .expect("peer wins after both registered")
        .commit()
        .expect("apply peer winner");
    assert_eq!(winner.sequence, 1);
    assert!(matches!(
        early.acquire(),
        Err(EditCoordinationError::StaleGeneration { .. })
    ));

    let later = authority
        .prepare(attempt(
            &workspace,
            "session-later",
            "agent-later",
            "later",
            &["note.txt"],
        ))
        .expect("register after applied winner")
        .acquire()
        .expect("post-winner registration sees the current generation")
        .commit()
        .expect("apply sequential edit");
    assert_eq!(later.sequence, 2);

    let eligible = authority
        .prepare(attempt(
            &workspace,
            "session-eligible",
            "agent-eligible",
            "eligible",
            &["other.txt"],
        ))
        .expect("register attempt before failing peer");
    let failed_peer = authority
        .prepare(attempt(
            &workspace,
            "session-failed",
            "agent-failed",
            "failed-peer",
            &["other.txt"],
        ))
        .expect("register failing peer")
        .acquire()
        .expect("failing peer temporarily holds lease");
    drop(failed_peer);
    eligible
        .acquire()
        .expect("uncommitted peer does not stale earlier registration")
        .commit()
        .expect("eligible attempt applies after peer failure");
}

#[tokio::test]
async fn cancelling_a_task_drops_its_lease_and_allows_retry() {
    let workspace = TestWorkspace::new("cancellation");
    let authority = InProcessEditAuthority::new();
    let task_authority = authority.clone();
    let held_attempt = attempt(
        &workspace,
        "session-a",
        "agent-a",
        "cancelled",
        &["note.txt"],
    );
    let (started_tx, started_rx) = tokio::sync::oneshot::channel();
    let task = tokio::spawn(async move {
        let _lease = task_authority
            .prepare(held_attempt)
            .expect("prepare cancelled task")
            .acquire()
            .expect("cancelled task reserves lease");
        started_tx.send(()).expect("signal held lease");
        std::future::pending::<()>().await;
    });
    started_rx.await.expect("task reached held-lease barrier");
    assert_eq!(authority.held_count(), 1);

    task.abort();
    assert!(task.await.expect_err("task was cancelled").is_cancelled());
    assert_eq!(authority.held_count(), 0);
    authority
        .prepare(attempt(
            &workspace,
            "session-b",
            "agent-b",
            "retry",
            &["note.txt"],
        ))
        .expect("prepare cancellation retry")
        .acquire()
        .expect("cancelled lease was released");
}

#[test]
fn one_hundred_twenty_eight_barrier_started_overlaps_have_one_qualified_holder() {
    const ATTEMPTS: usize = 128;
    let workspace = TestWorkspace::new("stress");
    let authority = Arc::new(InProcessEditAuthority::new());
    let winner = authority
        .prepare(attempt(
            &workspace,
            "winner-session",
            "winner-agent",
            "winner-attempt",
            &["shared.txt"],
        ))
        .expect("prepare deterministic winner")
        .acquire()
        .expect("install deterministic winner before releasing barrier");
    let barrier = Arc::new(Barrier::new(ATTEMPTS + 1));

    let threads: Vec<_> = (0..ATTEMPTS)
        .map(|index| {
            let authority = authority.clone();
            let barrier = barrier.clone();
            let attempt = attempt(
                &workspace,
                &format!("session-{index}"),
                &format!("agent-{index}"),
                format!("attempt-{index}"),
                &["shared.txt"],
            );
            std::thread::spawn(move || {
                barrier.wait();
                authority
                    .prepare(attempt)
                    .expect("prepare stress contender")
                    .acquire()
            })
        })
        .collect();
    barrier.wait();

    for thread in threads {
        let error = match thread.join().expect("stress contender did not panic") {
            Ok(_) => panic!("held winner must reject every overlap"),
            Err(error) => error,
        };
        assert!(matches!(
            error,
            EditCoordinationError::Conflict {
                holder_session,
                holder_agent,
                holder_attempt,
                ..
            } if holder_session == "winner-session"
                && holder_agent == "winner-agent"
                && holder_attempt == "winner-attempt"
        ));
    }
    assert_eq!(authority.held_count(), 1);
    drop(winner);
    assert_eq!(authority.held_count(), 0);
}
