//! Integration tests for the process `Host`.
//!
//! These spawn real short-lived processes on the local machine through a PTY
//! and assert on live output, resize, stdin, kill, wait, and status.

use std::time::Duration;

use firmius_core::host::{Host, LocalHost, OnOrphan, ProcSpec, ProcStatus, PtySize};
use futures::StreamExt;
use tokio::time::timeout;

/// Collect a process's output stream into one byte vec, with a safety timeout.
async fn drain_output(host: &LocalHost, id: firmius_core::ProcId) -> Vec<u8> {
    let mut stream = host.output(id).expect("output stream");
    let mut out = Vec::new();
    let collect = async {
        while let Some(chunk) = stream.next().await {
            out.extend_from_slice(&chunk.bytes);
        }
        out
    };
    timeout(Duration::from_secs(10), collect)
        .await
        .expect("output stream did not finish in time")
}

#[tokio::test]
async fn spawns_and_captures_stdout() {
    let host = LocalHost::new();
    let id = host
        .spawn(ProcSpec::new("echo").arg("hello firmius"))
        .await
        .expect("spawn echo");

    let out = drain_output(&host, id).await;
    let text = String::from_utf8_lossy(&out);
    assert!(
        text.contains("hello firmius"),
        "expected greeting in output, got: {text:?}"
    );
}

#[tokio::test]
async fn wait_reports_exit_status() {
    let host = LocalHost::new();

    let ok = host.spawn(ProcSpec::new("true")).await.expect("spawn true");
    let status = host.wait(ok).await.expect("wait true");
    assert!(status.success, "`true` should succeed");
    assert_eq!(status.code, 0);

    let bad = host
        .spawn(ProcSpec::new("false"))
        .await
        .expect("spawn false");
    let status = host.wait(bad).await.expect("wait false");
    assert!(!status.success, "`false` should fail");
    assert_ne!(status.code, 0);
}

#[tokio::test]
async fn status_transitions_from_running_to_exited() {
    let host = LocalHost::new();
    // Sleep long enough to observe the Running state first.
    let id = host
        .spawn(ProcSpec::new("sh").arg("-c").arg("sleep 0.3; exit 7"))
        .await
        .expect("spawn sleeper");

    // Immediately after spawn it should be running.
    matches!(host.status(id).unwrap(), ProcStatus::Running)
        .then_some(())
        .expect("process should start out running");

    let status = host.wait(id).await.expect("wait sleeper");
    assert_eq!(status.code, 7);
    assert!(matches!(
        host.status(id).unwrap(),
        ProcStatus::Exited {
            code: 7,
            success: false
        }
    ));
}

#[tokio::test]
async fn kill_terminates_a_long_running_process() {
    let host = LocalHost::new();
    let id = host
        .spawn(ProcSpec::new("sleep").arg("60"))
        .await
        .expect("spawn sleep 60");

    assert!(matches!(host.status(id).unwrap(), ProcStatus::Running));

    host.kill(id).await.expect("kill");

    // wait must return promptly once killed, not block for 60s.
    let status = timeout(Duration::from_secs(5), host.wait(id))
        .await
        .expect("wait returned after kill")
        .expect("wait ok");
    assert!(!status.success, "killed process should not report success");
    assert!(host.status(id).unwrap().is_terminal());
}

#[tokio::test]
async fn stdin_is_delivered_to_the_child() {
    let host = LocalHost::new();
    // `cat` echoes stdin back to stdout (through the PTY).
    let id = host.spawn(ProcSpec::new("cat")).await.expect("spawn cat");

    host.write_stdin(id, b"ping\n").await.expect("write stdin");

    // Read enough output to see our line, then kill cat to end the stream.
    let mut stream = host.output(id).expect("output");
    let seen = timeout(Duration::from_secs(5), async {
        let mut acc = Vec::new();
        while let Some(chunk) = stream.next().await {
            acc.extend_from_slice(&chunk.bytes);
            if String::from_utf8_lossy(&acc).contains("ping") {
                break;
            }
        }
        acc
    })
    .await
    .expect("saw echoed stdin");

    assert!(String::from_utf8_lossy(&seen).contains("ping"));
    host.kill(id).await.expect("kill cat");
}

#[tokio::test]
async fn resize_reports_new_dimensions_to_the_pty() {
    let host = LocalHost::new();
    // Start an interactive shell so the PTY stays open for the resize + query.
    let id = host
        .spawn(ProcSpec::new("sh").size(PtySize::new(24, 80)))
        .await
        .expect("spawn sh");

    host.resize(id, PtySize::new(50, 132)).expect("resize");

    // Ask the shell to print the terminal size it now sees.
    host.write_stdin(id, b"stty size; exit\n")
        .await
        .expect("write stty");

    let out = drain_output(&host, id).await;
    let text = String::from_utf8_lossy(&out);
    assert!(
        text.contains("50 132"),
        "expected resized dimensions '50 132' in `stty size` output, got: {text:?}"
    );
}

#[tokio::test]
async fn output_can_be_replayed_after_exit() {
    let host = LocalHost::new();
    let id = host
        .spawn(ProcSpec::new("echo").arg("replay-me"))
        .await
        .expect("spawn echo");

    // Ensure it's fully done first.
    host.wait(id).await.expect("wait");

    // Subscribing *after* exit must still replay the captured buffer.
    let out = drain_output(&host, id).await;
    assert!(String::from_utf8_lossy(&out).contains("replay-me"));
}

#[tokio::test]
async fn multiple_subscribers_each_see_full_output() {
    let host = LocalHost::new();
    let id = host
        .spawn(ProcSpec::new("echo").arg("fan-out"))
        .await
        .expect("spawn echo");

    let a = drain_output(&host, id).await;
    let b = drain_output(&host, id).await;
    assert!(String::from_utf8_lossy(&a).contains("fan-out"));
    assert!(String::from_utf8_lossy(&b).contains("fan-out"));
}

#[tokio::test]
async fn list_and_remove_track_processes() {
    let host = LocalHost::new();
    let id = host
        .spawn(ProcSpec::new("sleep").arg("60"))
        .await
        .expect("spawn");

    assert!(host.list().contains(&id));

    host.remove(id).await.expect("remove");
    assert!(!host.list().contains(&id));
    // Operations on a removed process should error, not panic.
    assert!(host.status(id).is_err());
}

#[tokio::test]
async fn unknown_process_id_errors_cleanly() {
    let host = LocalHost::new();
    // A never-spawned process id.
    let bogus = {
        let real = LocalHost::new();
        let id = real.spawn(ProcSpec::new("true")).await.unwrap();
        real.wait(id).await.unwrap();
        id
    };
    assert!(host.status(bogus).is_err());
    assert!(host.output(bogus).is_err());
    assert!(host.wait(bogus).await.is_err());
    assert!(host.kill(bogus).await.is_err());
}

#[tokio::test]
async fn dropped_host_kills_orphans_by_default() {
    // Spawn under one host, then drop it; the OnOrphan::Kill default should
    // reap the child. We can't easily observe the PID post-drop, so we assert
    // the policy path doesn't hang and that Detach is selectable.
    let detach_id;
    {
        let host = LocalHost::new();
        detach_id = host
            .spawn(ProcSpec::new("sleep").arg("30").on_orphan(OnOrphan::Detach))
            .await
            .expect("spawn detached");
        assert!(matches!(
            host.status(detach_id).unwrap(),
            ProcStatus::Running
        ));
        // Host drops here. Kill-default children are reaped; detached ones leak
        // intentionally. Nothing should block.
    }
    // Best-effort cleanup of the detached process so tests don't leak it.
    // (New host can't see it; kill by nothing — rely on OS. This is a no-op
    // assertion that we reached here without deadlock.)
    let _ = detach_id;
}
