//! Integration tests for the `bash`, `grep`, and `glob` tools.

use std::{collections::HashSet, sync::Arc};

use firmius_core::{
    AgentState, LocalHost, Session, ToolContext, ToolError, ToolRegistry, TypedTool,
    register_bash_tool, register_delegate_tool, register_edit_tool, register_glob_tool,
    register_grep_tool, register_list_tool, register_read_tool,
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
        allowed_scopes: None,
    }
}

fn session_ctx(workdir: std::path::PathBuf, session: Arc<Session>) -> ToolContext {
    ToolContext {
        workdir,
        cancellation: CancellationToken::new(),
        tool_call_id: "test-call".into(),
        agent_id: "test-agent".into(),
        session_id: "test-session".into(),
        state: Arc::new(std::sync::RwLock::new(AgentState::default())),
        host: Arc::new(LocalHost::new()),
        session: Some(session),
        allowed_scopes: None,
    }
}

async fn session_and_ctx(workdir: std::path::PathBuf) -> (Arc<Session>, ToolContext) {
    let session = Session::new_handle();
    let ctx = session_ctx(workdir, session.clone());
    (session, ctx)
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
// read
// ---------------------------------------------------------------------------

#[tokio::test]
async fn read_supports_offset_and_max_lines() {
    let dir = tmp_workdir("read-offset");
    let path = dir.join("lines.txt");
    std::fs::write(&path, "one\ntwo\nthree\nfour\nfive\n").unwrap();

    let tools = ToolRegistry::default();
    register_read_tool(&tools);
    let out = tools
        .call(
            "read",
            serde_json::json!({"path": path, "offset": 1, "max_lines": 2}),
            ctx(dir.clone()),
        )
        .await
        .expect("regional read should work");
    assert_eq!(out, "two\nthree");
    std::fs::remove_dir_all(dir).ok();
}

#[tokio::test]
async fn read_supports_inclusive_start_and_end_lines() {
    let dir = tmp_workdir("read-lines");
    let path = dir.join("lines.txt");
    std::fs::write(&path, "one\ntwo\nthree\nfour\nfive\n").unwrap();

    let tools = ToolRegistry::default();
    register_read_tool(&tools);
    let out = tools
        .call(
            "read",
            serde_json::json!({"path": path, "start_line": 2, "end_line": 4}),
            ctx(dir.clone()),
        )
        .await
        .expect("regional read should work");
    assert_eq!(out, "two\nthree\nfour");
    std::fs::remove_dir_all(dir).ok();
}

#[tokio::test]
async fn read_rejects_partial_region_arguments() {
    let dir = tmp_workdir("read-invalid");
    let path = dir.join("lines.txt");
    std::fs::write(&path, "one\n").unwrap();

    let tools = ToolRegistry::default();
    register_read_tool(&tools);
    let error = tools
        .call(
            "read",
            serde_json::json!({"path": path, "offset": 0}),
            ctx(dir.clone()),
        )
        .await
        .expect_err("partial region should fail");
    assert!(error.to_string().contains("max_lines"));
    std::fs::remove_dir_all(dir).ok();
}

#[tokio::test]
async fn read_resolves_relative_paths_from_the_tool_workdir() {
    let dir = tmp_workdir("read-relative");
    std::fs::write(dir.join("notes.txt"), "alpha\nbeta\ngamma\n").unwrap();

    let tools = ToolRegistry::default();
    register_read_tool(&tools);
    let out = tools
        .call(
            "read",
            serde_json::json!({"path": "notes.txt", "start_line": 2, "limit": 1}),
            ctx(dir.clone()),
        )
        .await
        .expect("relative regional read should work");
    assert_eq!(out, "beta");
    std::fs::remove_dir_all(dir).ok();
}

#[test]
fn read_schema_exposes_only_the_simple_region_controls() {
    let tools = ToolRegistry::default();
    register_read_tool(&tools);
    let definition = tools
        .definitions()
        .into_iter()
        .find(|definition| definition.name == "read")
        .expect("read tool registered");
    let properties = definition.input_schema["properties"]
        .as_object()
        .expect("read properties");
    assert!(properties.contains_key("path"));
    assert!(properties.contains_key("start_line"));
    assert!(properties.contains_key("limit"));
    assert!(!properties.contains_key("offset"));
    assert!(!properties.contains_key("max_lines"));
    assert!(!properties.contains_key("end_line"));
}

// ---------------------------------------------------------------------------
// bash
// ---------------------------------------------------------------------------

#[tokio::test]
async fn bash_exec_runs_and_returns_output() {
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
    let c = ctx(std::env::temp_dir());

    let out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "exec", "command": "echo", "args": ["hi-from-bash"], "intent": "test echo"}),
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
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
    let c = ctx(std::env::temp_dir());

    // spawn a background sleeper
    let spawn_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "spawn", "command": "sleep", "args": ["30"], "intent": "test sleep"}),
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
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
    let c = ctx(std::env::temp_dir());

    let spawn_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "spawn", "command": "cat", "intent": "test cat stdin"}),
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
    assert!(
        seen.contains("echoback"),
        "expected echoed stdin, got: {seen}"
    );

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
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
    let c = ctx(std::env::temp_dir());

    let spawn_out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "spawn", "command": "sh", "rows": 24, "cols": 80, "intent": "test pty resize"}),
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
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
    let c = ctx(std::env::temp_dir());

    let out = tools
        .call(
            "bash",
            serde_json::json!({"mode": "exec", "command": "sleep", "args": ["2"], "timeout_ms": 100, "intent": "test exec timeout"}),
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
async fn bash_exec_cancellation_kills_an_interactive_process() {
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
    let c = ctx(std::env::temp_dir());
    let cancellation = c.cancellation.clone();
    let task = tokio::spawn(async move {
        tools
            .call(
                "bash",
                serde_json::json!({
                    "mode": "exec",
                    "command": "sh",
                    "args": ["-c", "printf 'pager-like process\\n'; sleep 30"],
                    "intent": "test cancellation"
                }),
                c,
            )
            .await
    });

    tokio::time::sleep(std::time::Duration::from_millis(100)).await;
    cancellation.cancel();
    let result = tokio::time::timeout(std::time::Duration::from_secs(5), task)
        .await
        .expect("cancellation should return promptly")
        .expect("tool task should not panic")
        .expect_err("cancelled exec should report cancellation");
    assert!(result.to_string().contains("cancelled"));
}

#[tokio::test]
async fn bash_unknown_mode_field_is_rejected() {
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
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
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
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
    assert!(
        def.description.contains("git --no-pager"),
        "bash instructions must prevent Git pager hangs"
    );
    assert!(
        def.input_schema
            .get("required")
            .and_then(|required| required.as_array())
            .is_none_or(|required| required.iter().all(|field| field != "mode")),
        "ordinary bash calls must be able to omit mode"
    );
    let args_schema = def
        .input_schema
        .get("properties")
        .and_then(|p| p.get("args"))
        .expect("args field present at top level");
    assert_eq!(
        args_schema.get("type").and_then(|v| v.as_str()),
        Some("array")
    );
}

/// The normal interface is one complete shell command with no `args` array.
#[tokio::test]
async fn bash_accepts_a_plain_shell_command_string() {
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
    let c = ctx(std::env::temp_dir());

    let out = tools
        .call(
            "bash",
            serde_json::json!({
                "command": "printf 'left\\n' && printf 'right\\n'",
                "intent": "test plain shell command"
            }),
            c,
        )
        .await
        .expect("plain shell command should run");
    assert!(out.contains("left"));
    assert!(out.contains("right"));
}

/// The documented escape hatch for shell syntax: run bash -c explicitly.
#[tokio::test]
async fn bash_exec_with_shell_syntax_via_bash_dash_c() {
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
    let c = ctx(std::env::temp_dir());

    let out = tools
        .call(
            "bash",
            serde_json::json!({
                "mode": "exec",
                "command": "bash",
                "args": ["-c", "echo left && echo right"],
                "intent": "test bash -c syntax"
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

    let tools = ToolRegistry::default();
    register_glob_tool(&tools);
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

    let tools = ToolRegistry::default();
    register_glob_tool(&tools);
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
    let tools = ToolRegistry::default();
    register_glob_tool(&tools);
    let c = ctx(dir.clone());

    let out = tools
        .call(
            "glob",
            serde_json::json!({"pattern": "**/*.nonexistent"}),
            c,
        )
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
    std::fs::write(
        dir.join("a.rs"),
        "fn main() {\n    println!(\"needle\");\n}\n",
    )
    .unwrap();
    std::fs::write(dir.join("b.rs"), "fn other() {}\n").unwrap();

    let tools = ToolRegistry::default();
    register_grep_tool(&tools);
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

    let tools = ToolRegistry::default();
    register_grep_tool(&tools);
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

    let tools = ToolRegistry::default();
    register_grep_tool(&tools);
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

    let tools = ToolRegistry::default();
    register_grep_tool(&tools);
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
    let tools = ToolRegistry::default();
    register_grep_tool(&tools);
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

    let tools = ToolRegistry::default();
    register_grep_tool(&tools);
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

    let tools = ToolRegistry::default();
    register_glob_tool(&tools);
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

    let tools = ToolRegistry::default();
    register_glob_tool(&tools);
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

    let tools = ToolRegistry::default();
    register_grep_tool(&tools);
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
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);
    let c = ctx(std::env::temp_dir());

    let out = tools
        .call(
            "bash",
            serde_json::json!({
                "mode": "exec",
                "command": "echo",
                "args": ["flex"],
                "timeout_ms": "5000",
                "intent": "test stringified timeout"
            }),
            c,
        )
        .await
        .expect("string timeout_ms must be coerced, not rejected");
    assert!(out.contains("exit_code=0"));
    assert!(out.contains("flex"));
}

// ---------------------------------------------------------------------------
// tool scopes
// ---------------------------------------------------------------------------

fn scope_set(scopes: &[&str]) -> HashSet<String> {
    scopes.iter().map(|scope| (*scope).to_string()).collect()
}

#[tokio::test]
async fn builtin_tools_declare_expected_scopes_and_filter_definitions() {
    let tools = ToolRegistry::default();
    register_list_tool(&tools);
    register_read_tool(&tools);
    register_glob_tool(&tools);
    register_grep_tool(&tools);
    register_edit_tool(&tools);
    register_bash_tool(&tools);
    register_delegate_tool(&tools);

    let all_names = tools
        .definitions()
        .into_iter()
        .map(|definition| definition.name)
        .collect::<HashSet<_>>();
    assert_eq!(
        all_names,
        scope_set(&["list", "read", "glob", "grep", "edit", "bash", "delegate"])
    );

    let fs_read_names = tools
        .definitions_scoped(Some(&scope_set(&["fs_read"])))
        .into_iter()
        .map(|definition| definition.name)
        .collect::<HashSet<_>>();
    // `delegate` has no tool-level required scope: its run/spawn/poll/wait and
    // send modes are permission-checked inside the handler (delegation vs
    // agent_message respectively), so the tool stays visible to workers.
    assert_eq!(
        fs_read_names,
        scope_set(&["list", "read", "glob", "grep", "delegate"])
    );

    let process_names = tools
        .definitions_scoped(Some(&scope_set(&["processes"])))
        .into_iter()
        .map(|definition| definition.name)
        .collect::<HashSet<_>>();
    assert_eq!(process_names, scope_set(&["bash", "delegate"]));

    let delegation_names = tools
        .definitions_scoped(Some(&scope_set(&["delegation"])))
        .into_iter()
        .map(|definition| definition.name)
        .collect::<HashSet<_>>();
    assert_eq!(delegation_names, scope_set(&["delegate"]));
}

#[tokio::test]
async fn scoped_dispatch_denies_tools_missing_required_scope() {
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);

    let err = tools
        .call_scoped(
            "bash",
            serde_json::json!({"mode": "list"}),
            ctx(std::env::temp_dir()),
            Some(&scope_set(&["fs_read"])),
        )
        .await
        .expect_err("bash should require processes scope");

    match err {
        ToolError::PermissionDenied {
            tool,
            required,
            allowed,
        } => {
            assert_eq!(tool, "bash");
            assert_eq!(required, vec!["processes".to_string()]);
            assert_eq!(allowed, vec!["fs_read".to_string()]);
        }
        other => panic!("expected permission denied, got {other:?}"),
    }
}

#[tokio::test]
async fn scoped_dispatch_allows_legacy_none_unrestricted_behavior() {
    let tools = ToolRegistry::default();
    register_bash_tool(&tools);

    let out = tools
        .call_scoped(
            "bash",
            serde_json::json!({"mode": "exec", "command": "echo", "args": ["legacy"], "intent": "test legacy dispatch"}),
            ctx(std::env::temp_dir()),
            None,
        )
        .await
        .expect("None should preserve unrestricted legacy dispatch");

    assert!(out.contains("legacy"));
}

#[tokio::test]
async fn scoped_dispatch_requires_all_declared_scopes() {
    let tools = ToolRegistry::default();
    tools.register(
        TypedTool::new(
            "multi_scope",
            "test tool requiring multiple scopes",
            |_args: serde_json::Value, _ctx: ToolContext| Box::pin(async { Ok("ok".to_string()) }),
        )
        .with_required_scopes(["fs_read", "processes"]),
    );

    assert!(
        tools
            .definitions_scoped(Some(&scope_set(&["fs_read"])))
            .is_empty(),
        "definition should be hidden until every required scope is allowed"
    );

    let denied = tools
        .call_scoped(
            "multi_scope",
            serde_json::json!({}),
            ctx(std::env::temp_dir()),
            Some(&scope_set(&["fs_read"])),
        )
        .await
        .expect_err("one missing scope should deny dispatch");
    assert!(matches!(denied, ToolError::PermissionDenied { .. }));

    let out = tools
        .call_scoped(
            "multi_scope",
            serde_json::json!({}),
            ctx(std::env::temp_dir()),
            Some(&scope_set(&["fs_read", "processes"])),
        )
        .await
        .expect("all scopes should allow dispatch");
    assert_eq!(out, "ok");
}

// ---------------------------------------------------------------------------
// artifacts
// ---------------------------------------------------------------------------

#[tokio::test]
async fn artifact_edit_read_list_round_trip() {
    let (_session, ctx) = session_and_ctx(std::env::temp_dir()).await;
    let tools = ToolRegistry::default();
    register_edit_tool(&tools);
    register_read_tool(&tools);
    register_list_tool(&tools);

    let out = tools
        .call(
            "edit",
            serde_json::json!({
                "patch": "*** Begin Patch\n*** Add File: artifact://note.md\n+hello\n+world\n*** End Patch\n"
            }),
            ctx.clone(),
        )
        .await
        .expect("artifact add");
    assert!(out.contains("created artifact://note.md"), "{out}");

    assert_eq!(
        tools
            .call(
                "read",
                serde_json::json!({ "path": "artifact://note.md" }),
                ctx.clone(),
            )
            .await
            .expect("artifact read"),
        "hello\nworld"
    );

    let listed = tools
        .call(
            "list",
            serde_json::json!({ "path": "artifact://" }),
            ctx.clone(),
        )
        .await
        .expect("artifact list");
    assert!(listed.contains("artifact://note.md"), "{listed}");

    let out = tools
        .call(
            "edit",
            serde_json::json!({
                "patch": "*** Begin Patch\n*** Update File: artifact://note.md\n@@\n-hello\n+HELLO\n*** End Patch\n"
            }),
            ctx.clone(),
        )
        .await
        .expect("artifact update");
    assert!(out.contains("updated artifact://note.md"), "{out}");
    assert_eq!(
        tools
            .call(
                "read",
                serde_json::json!({ "path": "artifact://note.md" }),
                ctx.clone(),
            )
            .await
            .expect("artifact read after update"),
        "HELLO\nworld"
    );

    let out = tools
        .call(
            "edit",
            serde_json::json!({
                "patch": "*** Begin Patch\n*** Delete File: artifact://note.md\n*** End Patch\n"
            }),
            ctx.clone(),
        )
        .await
        .expect("artifact delete");
    assert!(out.contains("deleted artifact://note.md"), "{out}");
    assert!(
        tools
            .call(
                "read",
                serde_json::json!({ "path": "artifact://note.md" }),
                ctx.clone(),
            )
            .await
            .is_err()
    );
}

#[tokio::test]
async fn artifact_glob_and_grep_search_session_memory() {
    let (_session, ctx) = session_and_ctx(std::env::temp_dir()).await;
    let tools = ToolRegistry::default();
    register_edit_tool(&tools);
    register_glob_tool(&tools);
    register_grep_tool(&tools);

    for (path, content) in [("plans/a.md", "one\ntwo"), ("plans/b.md", "two\nthree")] {
        tools
            .call(
                "edit",
                serde_json::json!({
                    "patch": format!(
                        "*** Begin Patch\n*** Add File: artifact://{path}\n+{}\n+{}\n*** End Patch\n",
                        content.lines().next().unwrap(),
                        content.lines().nth(1).unwrap(),
                    )
                }),
                ctx.clone(),
            )
            .await
            .expect("artifact add");
    }

    let globbed = tools
        .call(
            "glob",
            serde_json::json!({ "pattern": "*.md", "path": "artifact://plans" }),
            ctx.clone(),
        )
        .await
        .expect("artifact glob");
    assert!(globbed.contains("artifact://plans/a.md"), "{globbed}");
    assert!(globbed.contains("artifact://plans/b.md"), "{globbed}");

    let grepped = tools
        .call(
            "grep",
            serde_json::json!({ "pattern": "two", "path": "artifact://" }),
            ctx.clone(),
        )
        .await
        .expect("artifact grep");
    assert!(
        grepped.contains("artifact://plans/a.md:2: two"),
        "{grepped}"
    );
    assert!(
        grepped.contains("artifact://plans/b.md:1: two"),
        "{grepped}"
    );
}

#[tokio::test]
async fn artifact_read_supports_regions() {
    let (_session, ctx) = session_and_ctx(std::env::temp_dir()).await;
    let tools = ToolRegistry::default();
    register_edit_tool(&tools);
    register_read_tool(&tools);

    tools
        .call(
            "edit",
            serde_json::json!({
                "patch": "*** Begin Patch\n*** Add File: artifact://lines.txt\n+one\n+two\n+three\n+four\n*** End Patch\n"
            }),
            ctx.clone(),
        )
        .await
        .expect("artifact add");

    let out = tools
        .call(
            "read",
            serde_json::json!({ "path": "artifact://lines.txt", "start_line": 2, "limit": 2 }),
            ctx.clone(),
        )
        .await
        .expect("artifact region read");
    assert_eq!(out, "two\nthree");
}
