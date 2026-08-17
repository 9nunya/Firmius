//! End-to-end MCP client tests against a real stdio server.
//!
//! The ast-grep test downloads `ast-grep-mcp` from npm on first run, so it is
//! marked `#[ignore]` and exercised explicitly with `--ignored`.

use firmius_core::{McpManager, McpServerConfig, McpSettings};

fn npx_available() -> bool {
    std::process::Command::new("npx")
        .arg("--version")
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false)
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
    let manager = McpManager::from_settings(settings);

    let specs = tokio::time::timeout(
        std::time::Duration::from_secs(120),
        manager.start("ast-grep"),
    )
    .await
    .expect("timed out starting ast-grep")
    .expect("failed to start ast-grep");

    assert!(
        specs.iter().any(|spec| spec.name == "ast_grep_search"),
        "discovered tools: {specs:?}"
    );

    let output = tokio::time::timeout(
        std::time::Duration::from_secs(60),
        manager.call_tool(
            "ast-grep",
            "ast_grep_search",
            serde_json::json!({ "pattern": "def hello()", "lang": "python" }),
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
