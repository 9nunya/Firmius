//! End-to-end MCP client tests against a real stdio server.
//!
//! The ast-grep test downloads `ast-grep-mcp` from npm on first run, so it is
//! marked `#[ignore]` and exercised explicitly with `--ignored`.

use std::sync::Arc;

use firmius_core::{
    AgentState, LocalHost, McpManager, McpServerConfig, McpSettings, ToolContext, ToolRegistry,
    register_tool_specs,
};
use tokio_util::sync::CancellationToken;

fn npx_available() -> bool {
    std::process::Command::new("npx")
        .arg("--version")
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false)
}

fn tool_ctx() -> ToolContext {
    ToolContext {
        workdir: std::env::temp_dir(),
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

#[tokio::test]
#[ignore = "downloads ast-grep-mcp from npm; run with --ignored"]
async fn ast_grep_stdio_end_to_end() {
    if !npx_available() {
        eprintln!("npx not available; skipping ast-grep MCP test");
        return;
    }

    let workdir = std::env::temp_dir().join(format!(
        "firmius-astgrep-{}",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir_all(&workdir).unwrap();
    std::fs::write(workdir.join("sample.py"), "def hello():\n    print('hi')\n").unwrap();

    let mut settings = McpSettings::default();
    settings
        .upsert(McpServerConfig {
            name: "ast-grep".into(),
            command: Some("npx".into()),
            args: vec!["-y".into(), "ast-grep-mcp".into()],
            env: Default::default(),
            url: None,
            cwd: Some(workdir.to_string_lossy().into_owned()),
            enabled: true,
        })
        .unwrap();
    let manager = Arc::new(McpManager::from_settings(settings));

    let specs = tokio::time::timeout(
        std::time::Duration::from_secs(120),
        manager.start("ast-grep"),
    )
    .await
    .expect("timed out starting ast-grep")
    .expect("failed to start ast-grep");

    let search_spec = specs
        .iter()
        .find(|spec| spec.name == "ast_grep_search")
        .expect("ast_grep_search not discovered")
        .clone();
    assert!(
        !search_spec.description.is_empty(),
        "tool description must be exposed to the model"
    );
    assert!(
        search_spec.input_schema.is_object(),
        "tool schema must be exposed to the model"
    );

    // Register as a first-class tool (the path agents actually use) and invoke
    // it through the shared registry, not the manager directly.
    let tools = ToolRegistry::default();
    register_tool_specs(&tools, manager.clone(), specs);

    let definition = tools
        .definitions()
        .into_iter()
        .find(|def| def.name == "mcp__ast-grep__ast_grep_search")
        .expect("mcp tool not registered");
    assert_eq!(definition.input_schema, search_spec.input_schema);

    let output = tokio::time::timeout(
        std::time::Duration::from_secs(60),
        tools.call(
            "mcp__ast-grep__ast_grep_search",
            serde_json::json!({ "pattern": "def hello()", "lang": "python" }),
            tool_ctx(),
        ),
    )
    .await
    .expect("timed out calling ast_grep_search")
    .expect("ast_grep_search failed");

    assert!(
        output.contains("hello") || output.contains("sample.py"),
        "unexpected output: {output}"
    );

    manager.stop("ast-grep").await.unwrap();
    assert!(manager.status().await.iter().all(|s| !s.running));
    let _ = std::fs::remove_dir_all(&workdir);
}
