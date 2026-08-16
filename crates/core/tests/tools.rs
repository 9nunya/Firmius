//! Integration tests for the `bash`, `grep`, and `glob` tools.

use std::sync::Arc;

use firmius_core::{
    AgentState, LocalHost, ToolContext, ToolRegistry, register_bash_tool, register_glob_tool,
    register_grep_tool,
};
use tokio_util::sync::CancellationToken;

fn ctx(workdir: std::path::PathBuf) -> ToolContext {
    ToolContext {
        workdir,
        cancellation: CancellationToken::new(),
        tool_call_id: "test-call".into(),
        agent_id: "test-agent".into(),
        session_id: "test-session".into(),
        state: Arc::new(std::sync::RwLock::new(AgentState::default())),
        host: Arc::new(LocalHost::new()),
        session: None,
    }
}

fn tmp_workdir(name: &str) -> std::path::PathBuf {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let dir = std::env::temp_dir().join(format!("firmius-test-{name}-{nanos}"));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

// ---------------------------------------------------------------------------
// bash
// ---------------------------------------------------------------------------

#[tokio::test]
async fn bash_exec_runs_and_returns_output() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let c = ctx(std::env::temp_dir());

    let out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "exec", "command": "echo", "args": ["hi-from-bash"]}),
            c,
        )
        .await
        .expect("exec ok");

    assert!(out.contains("exit_code=0"));
    assert!(out.contains("success=true"));
    assert!(out.contains("hi-from-bash"));
}

#[tokio::test]
async fn bash_spawn_poll_wait_kill_roundtrip() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let c = ctx(std::env::temp_dir());

    // spawn a background sleeper
    let spawn_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "spawn", "command": "sleep", "args": ["30"]}),
            c.clone(),
        )
        .await
        .expect("spawn ok");
    let proc_id = spawn_out
        .strip_prefix("proc_id=")
        .expect("proc_id in output")
        .to_string();

    // poll should show it running, non-blocking
    let poll_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "poll", "proc_id": proc_id}),
            c.clone(),
        )
        .await
        .expect("poll ok");
    assert!(poll_out.contains("status=running"));

    // list should include it
    let list_out = tools
        .call("bash", serde_json::json!({"mode": "list"}), c.clone())
        .await
        .expect("list ok");
    assert!(list_out.contains(&proc_id));
    assert!(list_out.contains("sleep 30"));

    // kill it
    let kill_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "kill", "proc_id": proc_id}),
            c.clone(),
        )
        .await
        .expect("kill ok");
    assert!(kill_out.contains(&proc_id));

    // wait should now return promptly with a failure status
    let wait_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "wait", "proc_id": proc_id, "timeout_ms": 5000}),
            c,
        )
        .await
        .expect("wait ok");
    assert!(wait_out.contains("success=false"));
}

#[tokio::test]
async fn bash_input_feeds_stdin_to_process() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let c = ctx(std::env::temp_dir());

    let spawn_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "spawn", "command": "cat"}),
            c.clone(),
        )
        .await
        .expect("spawn cat");
    let proc_id = spawn_out.strip_prefix("proc_id=").unwrap().to_string();

    tools
        .call(
            "bash",
            serde_json::json!({"mode": "input", "proc_id": proc_id, "text": "echoback\n"}),
            c.clone(),
        )
        .await
        .expect("input ok");

    // Poll a few times until we see the echoed text (cat mirrors stdin to stdout).
    let mut seen = String::new();
    for _ in 0..20 {
        let poll_out = tools
            .call(
                "bash",
                serde_json::json!({"mode": "poll", "proc_id": proc_id, "since": 0}),
                c.clone(),
            )
            .await
            .expect("poll ok");
        seen = poll_out;
        if seen.contains("echoback") {
            break;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    assert!(seen.contains("echoback"), "expected echoed stdin, got: {seen}");

    tools
        .call(
            "bash",
            serde_json::json!({"mode": "kill", "proc_id": proc_id}),
            c,
        )
        .await
        .expect("cleanup kill");
}

#[tokio::test]
async fn bash_resize_reports_new_dimensions() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let c = ctx(std::env::temp_dir());

    let spawn_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "spawn", "command": "sh", "rows": 24, "cols": 80}),
            c.clone(),
        )
        .await
        .expect("spawn sh");
    let proc_id = spawn_out.strip_prefix("proc_id=").unwrap().to_string();

    let resize_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "resize", "proc_id": proc_id, "rows": 50, "cols": 132}),
            c.clone(),
        )
        .await
        .expect("resize ok");
    assert!(resize_out.contains("50x132"));

    tools
        .call(
            "bash",
            serde_json::json!({"mode": "kill", "proc_id": proc_id}),
            c,
        )
        .await
        .expect("cleanup kill");
}

#[tokio::test]
async fn bash_exec_times_out_and_leaves_process_pollable() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let c = ctx(std::env::temp_dir());

    let out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "exec", "command": "sleep", "args": ["2"], "timeout_ms": 100}),
            c.clone(),
        )
        .await
        .expect("exec ok (timeout path)");
    assert!(out.contains("still running"));
    let proc_id = out
        .lines()
        .find_map(|l| l.strip_prefix("still running after"))
        .is_some();
    assert!(proc_id);

    // Extract the proc_id text to clean it up.
    let id_str = out
        .split("proc_id=")
        .nth(1)
        .and_then(|s| s.split_whitespace().next())
        .expect("proc_id present");
    tools
        .call(
            "bash",
            serde_json::json!({"mode": "kill", "proc_id": id_str}),
            c,
        )
        .await
        .expect("cleanup kill");
}

#[tokio::test]
async fn bash_unknown_mode_field_is_rejected() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let c = ctx(std::env::temp_dir());

    let err = tools
        .call("bash", serde_json::json!({"mode": "not_a_mode"}), c)
        .await
        .expect_err("bogus mode should fail");
    assert!(err.to_string().to_lowercase().contains("invalid"));
}

/// Regression test: the tool schema must be a flat object, not a tagged-union
/// `oneOf`. Nested `oneOf` schemas are where tool-use models (Anthropic
/// included) reliably fail to fill array fields like `args` correctly,
/// sending a JSON-stringified array instead of a real one.
#[tokio::test]
async fn bash_schema_is_a_flat_object_not_a_oneof() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let def = tools
        .definitions()
        .into_iter()
        .find(|d| d.name == "bash")
        .expect("bash tool registered");

    assert_eq!(
        def.input_schema.get("type").and_then(|v| v.as_str()),
        Some("object"),
        "schema must be a plain object so `args` is a top-level array field"
    );
    assert!(
        def.input_schema.get("oneOf").is_none(),
        "schema must not be a oneOf union"
    );
    let args_schema = def
        .input_schema
        .get("properties")
        .and_then(|p| p.get("args"))
        .expect("args field present at top level");
    assert_eq!(args_schema.get("type").and_then(|v| v.as_str()), Some("array"));
}

/// Regression test: `command` must reject multi-word strings (shell syntax
/// smuggled through as one token) with a clear, actionable error instead of
/// a confusing PATH lookup failure.
#[tokio::test]
async fn bash_rejects_command_with_embedded_spaces() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let c = ctx(std::env::temp_dir());

    let err = tools
        .call(
            "bash",
            serde_json::json!({"mode": "exec", "command": "echo hello"}),
            c,
        )
        .await
        .expect_err("command with a space should be rejected before spawning");
    let msg = err.to_string();
    assert!(msg.contains("no spaces"), "got: {msg}");
    assert!(msg.contains("args"), "should point at 'args' as the fix: {msg}");
}

/// The documented escape hatch for shell syntax: run bash -c explicitly.
#[tokio::test]
async fn bash_exec_with_shell_syntax_via_bash_dash_c() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let c = ctx(std::env::temp_dir());

    let out = tools
        .call(
            "bash",
            serde_json::json!({
                "mode": "exec",
                "command": "bash",
                "args": ["-c", "echo left && echo right"]
            }),
            c,
        )
        .await
        .expect("exec ok");
    assert!(out.contains("left"));
    assert!(out.contains("right"));
    assert!(out.contains("exit_code=0"));
}

// ---------------------------------------------------------------------------
// glob
// ---------------------------------------------------------------------------

#[tokio::test]
async fn glob_finds_matching_files() {
    let dir = tmp_workdir("glob");
    std::fs::write(dir.join("a.rs"), "fn main() {}").unwrap();
    std::fs::write(dir.join("b.txt"), "not rust").unwrap();
    std::fs::create_dir_all(dir.join("sub")).unwrap();
    std::fs::write(dir.join("sub/c.rs"), "fn sub() {}").unwrap();

    let mut tools = ToolRegistry::default();
    register_glob_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call("glob", serde_json::json!({"pattern": "**/*.rs"}), c)
        .await
        .expect("glob ok");

    assert!(out.contains("a.rs"));
    assert!(out.contains("sub/c.rs") || out.contains("sub\\c.rs"));
    assert!(!out.contains("b.txt"));

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn glob_respects_gitignore_by_default() {
    let dir = tmp_workdir("glob-ignore");
    std::fs::write(dir.join(".gitignore"), "ignored.rs\n").unwrap();
    std::fs::write(dir.join("kept.rs"), "fn kept() {}").unwrap();
    std::fs::write(dir.join("ignored.rs"), "fn ignored() {}").unwrap();
    // `ignore` only applies .gitignore inside an actual git repo tree by
    // default git-related checks; init a minimal repo dir marker.
    std::fs::create_dir_all(dir.join(".git")).unwrap();

    let mut tools = ToolRegistry::default();
    register_glob_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call("glob", serde_json::json!({"pattern": "*.rs"}), c)
        .await
        .expect("glob ok");
    assert!(out.contains("kept.rs"));
    assert!(!out.contains("ignored.rs"));

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn glob_no_matches_reports_cleanly() {
    let dir = tmp_workdir("glob-empty");
    let mut tools = ToolRegistry::default();
    register_glob_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call("glob", serde_json::json!({"pattern": "**/*.nonexistent"}), c)
        .await
        .expect("glob ok");
    assert_eq!(out, "no matches");

    std::fs::remove_dir_all(&dir).ok();
}

// ---------------------------------------------------------------------------
// grep
// ---------------------------------------------------------------------------

#[tokio::test]
async fn grep_finds_matching_lines_with_path_and_line_number() {
    let dir = tmp_workdir("grep");
    std::fs::write(dir.join("a.rs"), "fn main() {\n    println!(\"needle\");\n}\n").unwrap();
    std::fs::write(dir.join("b.rs"), "fn other() {}\n").unwrap();

    let mut tools = ToolRegistry::default();
    register_grep_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call("grep", serde_json::json!({"pattern": "needle"}), c)
        .await
        .expect("grep ok");

    assert!(out.contains("a.rs:2:"));
    assert!(out.contains("needle"));
    assert!(!out.contains("b.rs"));

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn grep_filters_by_glob() {
    let dir = tmp_workdir("grep-glob");
    std::fs::write(dir.join("a.rs"), "target_word\n").unwrap();
    std::fs::write(dir.join("a.txt"), "target_word\n").unwrap();

    let mut tools = ToolRegistry::default();
    register_grep_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call(
            "grep",
            serde_json::json!({"pattern": "target_word", "glob": "*.rs"}),
            c,
        )
        .await
        .expect("grep ok");

    assert!(out.contains("a.rs"));
    assert!(!out.contains("a.txt"));

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn grep_returns_context_lines() {
    let dir = tmp_workdir("grep-context");
    std::fs::write(dir.join("a.txt"), "one\ntwo\nMATCH\nfour\nfive\n").unwrap();

    let mut tools = ToolRegistry::default();
    register_grep_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call(
            "grep",
            serde_json::json!({"pattern": "MATCH", "context": 1}),
            c,
        )
        .await
        .expect("grep ok");

    assert!(out.contains("a.txt-2: two"));
    assert!(out.contains("a.txt:3: MATCH"));
    assert!(out.contains("a.txt-4: four"));

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn grep_is_case_insensitive_when_requested() {
    let dir = tmp_workdir("grep-case");
    std::fs::write(dir.join("a.txt"), "Hello World\n").unwrap();

    let mut tools = ToolRegistry::default();
    register_grep_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call(
            "grep",
            serde_json::json!({"pattern": "hello world", "ignore_case": true}),
            c,
        )
        .await
        .expect("grep ok");
    assert!(out.contains("Hello World"));

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn grep_rejects_invalid_regex() {
    let dir = tmp_workdir("grep-badregex");
    let mut tools = ToolRegistry::default();
    register_grep_tool(&mut tools);
    let c = ctx(dir.clone());

    let err = tools
        .call("grep", serde_json::json!({"pattern": "("}), c)
        .await
        .expect_err("invalid regex should fail");
    assert!(err.to_string().to_lowercase().contains("bad pattern"));

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn grep_no_matches_reports_cleanly() {
    let dir = tmp_workdir("grep-empty");
    std::fs::write(dir.join("a.txt"), "nothing here\n").unwrap();

    let mut tools = ToolRegistry::default();
    register_grep_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call("grep", serde_json::json!({"pattern": "zzz_absent"}), c)
        .await
        .expect("grep ok");
    assert_eq!(out, "no matches");

    std::fs::remove_dir_all(&dir).ok();
}

// ---------------------------------------------------------------------------
// flexible argument typing (models emit "200" for 200, "true" for true)
// ---------------------------------------------------------------------------

/// Regression test: models (and the harnesses relaying them) routinely emit
/// numeric tool arguments as strings — e.g. `"limit": "200"` — especially
/// for union-typed schema fields like `["integer", "null"]`. Tools must
/// coerce these instead of failing the whole call.
#[tokio::test]
async fn glob_accepts_stringified_limit() {
    let dir = tmp_workdir("glob-strlimit");
    for name in ["a.rs", "b.rs", "c.rs"] {
        std::fs::write(dir.join(name), "fn x() {}").unwrap();
    }

    let mut tools = ToolRegistry::default();
    register_glob_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call(
            "glob",
            serde_json::json!({"pattern": "*.rs", "limit": "2"}),
            c,
        )
        .await
        .expect("string limit must be coerced, not rejected");
    assert!(
        out.contains("truncated"),
        "limit=2 should truncate 3 matches: {out}"
    );

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn glob_rejects_non_numeric_string_limit() {
    let dir = tmp_workdir("glob-badlimit");
    std::fs::write(dir.join("a.rs"), "fn x() {}").unwrap();

    let mut tools = ToolRegistry::default();
    register_glob_tool(&mut tools);
    let c = ctx(dir.clone());

    let err = tools
        .call(
            "glob",
            serde_json::json!({"pattern": "*.rs", "limit": "abc"}),
            c,
        )
        .await
        .expect_err("garbage limit must still fail");
    assert!(err.to_string().contains("not an integer"));

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn grep_accepts_stringified_numbers_and_bools() {
    let dir = tmp_workdir("grep-flex");
    std::fs::write(dir.join("log.txt"), "Match here\nfiller\nanother MATCH\n").unwrap();

    let mut tools = ToolRegistry::default();
    register_grep_tool(&mut tools);
    let c = ctx(dir.clone());

    let out = tools
        .call(
            "grep",
            serde_json::json!({
                "pattern": "match",
                "ignore_case": "true",
                "context": "1",
                "limit": "10"
            }),
            c,
        )
        .await
        .expect("stringified args must be coerced, not rejected");
    assert!(out.contains("Match here"), "got: {out}");
    assert!(out.contains("another MATCH"), "got: {out}");

    std::fs::remove_dir_all(&dir).ok();
}

#[tokio::test]
async fn bash_accepts_stringified_timeout() {
    let mut tools = ToolRegistry::default();
    register_bash_tool(&mut tools);
    let c = ctx(std::env::temp_dir());

    let out = tools
        .call(
            "bash",
            serde_json::json!({
                "mode": "exec",
                "command": "echo",
                "args": ["flex"],
                "timeout_ms": "5000"
            }),
            c,
        )
        .await
        .expect("string timeout_ms must be coerced, not rejected");
    assert!(out.contains("exit_code=0"));
    assert!(out.contains("flex"));
}