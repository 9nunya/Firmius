//! Account plumbing: per-account persistence, legacy migration, the kind
//! registry, and the setup wizards — all hermetic (temp data dirs only).

use firmius_core::kinds::alibaba::{ALIBABA_CN_BASE_URL, ALIBABA_INTL_BASE_URL};
use firmius_core::kinds::opencode_go::OPENCODE_GO_BASE_URL;
use firmius_core::{
    AccountKind, AccountRecord, AlibabaTokenPlanKind, AnthropicSubscriptionKind, ApiKeyKind,
    ApiType, CodexKind, OpencodeGoKind, ProviderManager, ProviderSchema, SelectOption, Step,
    match_select, run_wizard,
};
use std::path::PathBuf;
use std::sync::Arc;

fn tmp_dir(name: &str) -> PathBuf {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let dir = std::env::temp_dir().join(format!("firmius-accounts-test-{name}-{nanos}"));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn openai_schema(id: &str, base_url: &str) -> ProviderSchema {
    ProviderSchema {
        id: id.to_string(),
        api_type: ApiType::OpenAI,
        base_url: Some(base_url.to_string()),
        api_key_env: None,
        models: Vec::new(),
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

#[test]
fn account_round_trip_and_id_normalization() {
    let dir = tmp_dir("roundtrip");
    // Deliberately disagree the schema id: the record id must win on load.
    let mut schema = openai_schema("stale-id", "https://example.test/v1");
    schema.id = "stale-id".to_string();
    let record = AccountRecord {
        id: "myprov".to_string(),
        kind: "api-key".to_string(),
        schema,
        credentials: serde_json::json!({ "api_key": "k" }),
    };
    firmius_core::persistence::save_account_at(&dir, &record).unwrap();
    let loaded = firmius_core::persistence::load_account_at(&dir, "myprov").unwrap();
    assert_eq!(loaded.id, "myprov");
    assert_eq!(
        loaded.schema.id, "myprov",
        "schema id must follow the record id"
    );
    assert_eq!(loaded.kind, "api-key");

    let summaries = firmius_core::persistence::list_accounts_at(&dir);
    assert_eq!(summaries.len(), 1);
    assert_eq!(summaries[0].id, "myprov");
}

#[test]
fn corrupt_account_files_are_skipped_not_fatal() {
    let dir = tmp_dir("corrupt");
    let record = AccountRecord {
        id: "good".to_string(),
        kind: "api-key".to_string(),
        schema: openai_schema("good", "https://example.test/v1"),
        credentials: serde_json::json!({}),
    };
    firmius_core::persistence::save_account_at(&dir, &record).unwrap();
    std::fs::write(dir.join("accounts/bad.json"), "{ not json").unwrap();

    let summaries = firmius_core::persistence::list_accounts_at(&dir);
    assert_eq!(summaries.len(), 1);
    assert_eq!(summaries[0].id, "good");
}

#[test]
fn account_credentials_are_replaced_atomically_and_privately() {
    let dir = tmp_dir("private-atomic-account");
    let mut record = AccountRecord {
        id: "oauth".into(),
        kind: "anthropic".into(),
        schema: openai_schema("oauth", "https://example.test/v1"),
        credentials: serde_json::json!({ "access_token": "old" }),
    };
    firmius_core::persistence::save_account_at(&dir, &record).unwrap();
    record.credentials["access_token"] = "new".into();
    firmius_core::persistence::save_account_at(&dir, &record).unwrap();

    assert_eq!(
        firmius_core::persistence::load_account_at(&dir, "oauth")
            .unwrap()
            .credentials["access_token"],
        "new"
    );
    let account_dir = dir.join("accounts");
    assert!(
        std::fs::read_dir(&account_dir)
            .unwrap()
            .flatten()
            .all(|entry| !entry.file_name().to_string_lossy().contains(".tmp."))
    );
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = std::fs::metadata(account_dir.join("oauth.json"))
            .unwrap()
            .permissions()
            .mode();
        assert_eq!(mode & 0o777, 0o600);
    }
}

// ---------------------------------------------------------------------------
// Legacy migration
// ---------------------------------------------------------------------------

#[test]
fn migration_joins_legacy_auth_and_providers_into_accounts() {
    let dir = tmp_dir("migrate");
    std::fs::write(
        dir.join("auth.json"),
        r#"{"providers": {"legacy": {"api_key": "old-key"}}}"#,
    )
    .unwrap();
    std::fs::write(
        dir.join("providers.json"),
        r#"[{"id": "legacy", "api_type": "openai", "base_url": "https://example.test/v1"},
           {"id": "keyless", "api_type": "anthropic"}]"#,
    )
    .unwrap();

    let migrated = firmius_core::persistence::migrate_legacy(&dir).unwrap();
    assert_eq!(migrated, 2);

    // Legacy files neutralized, account files written.
    assert!(!dir.join("auth.json").exists());
    assert!(!dir.join("providers.json").exists());
    assert!(dir.join("auth.json.migrated").exists());
    assert!(dir.join("providers.json.migrated").exists());

    let legacy = firmius_core::persistence::load_account_at(&dir, "legacy").unwrap();
    assert_eq!(legacy.kind, "api-key");
    assert_eq!(legacy.credentials["api_key"], "old-key");
    let keyless = firmius_core::persistence::load_account_at(&dir, "keyless").unwrap();
    assert!(keyless.credentials.get("api_key").is_none());

    // Idempotent: legacy files are gone, so a second run is a no-op.
    assert_eq!(firmius_core::persistence::migrate_legacy(&dir).unwrap(), 0);

    // And the manager picks it all up.
    let mut mgr = ProviderManager::new().with_data_dir(dir.clone());
    mgr.load().unwrap();
    let provider = mgr.build("legacy").unwrap();
    assert_eq!(provider.id(), "legacy");
    // Keyless account without an env fallback reports the missing key.
    let err = mgr.build("keyless").err().unwrap();
    assert!(err.contains("no API key"), "got: {err}");
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

#[test]
fn manager_register_save_load_build_round_trip() {
    let dir = tmp_dir("manager");
    {
        let mut mgr = ProviderManager::new().with_data_dir(dir.clone());
        mgr.register_schema(openai_schema("prov", "https://example.test/v1"));
        mgr.set_api_key("prov", "sk-test");
        mgr.save().unwrap();
    }
    let mut mgr = ProviderManager::new().with_data_dir(dir.clone());
    mgr.load().unwrap();
    assert!(mgr.provider_ids().contains(&"prov"));
    let provider = mgr.build("prov").unwrap();
    assert_eq!(provider.id(), "prov");

    let err = mgr.build("nope").err().unwrap();
    assert!(err.contains("unknown provider"), "got: {err}");
}

#[test]
fn provider_kind_resolves_account_id_to_kind_name() {
    let mut mgr = ProviderManager::new();
    let schema = firmius_core::kinds::codex::schema_template("codex-account");
    mgr.register_account(AccountRecord {
        id: "codex-account-9d8sq8dh-ew7dya-s7wbw".into(),
        kind: "codex".into(),
        schema,
        credentials: serde_json::json!({}),
    });

    assert_eq!(
        mgr.provider_kind("codex-account-9d8sq8dh-ew7dya-s7wbw"),
        Some("codex")
    );
    assert_eq!(mgr.provider_kind("missing"), None);
}

#[test]
fn registering_a_codex_account_refreshes_stale_model_metadata() {
    let mut mgr = ProviderManager::new();
    mgr.register_kind(Arc::new(CodexKind));
    let mut stale = firmius_core::kinds::codex::schema_template("codex-account");
    stale.models.clear();
    mgr.register_account(AccountRecord {
        id: "codex-account".into(),
        kind: "codex".into(),
        schema: stale,
        credentials: serde_json::json!({}),
    });

    let info = mgr
        .model_info_for("codex-account", "gpt-5.6-sol")
        .expect("current Codex catalog should replace stale account metadata");
    assert_eq!(info.context_window, 272_000);
    assert!(info.effort_mode("low").is_some());
    assert!(info.effort_mode("ultra").is_some());
}

#[test]
fn anthropic_subscription_account_refreshes_models_builds_and_exposes_quota() {
    let dir = tmp_dir("anthropic-subscription");
    let mut mgr = ProviderManager::new().with_data_dir(dir);
    mgr.register_kind(Arc::new(AnthropicSubscriptionKind));
    let mut stale = firmius_core::kinds::anthropic_subscription::schema_template("anthropic-user");
    stale.models.clear();
    mgr.register_account(AccountRecord {
        id: "anthropic-user".into(),
        kind: "anthropic".into(),
        schema: stale,
        credentials: serde_json::json!({
            "access_token": "sk-ant-oat-test",
            "refresh_token": "refresh-test",
            "expires_at": 4_102_444_800_i64,
            "account_id": "user"
        }),
    });

    let info = mgr
        .model_info_for("anthropic-user", "claude-sonnet-5")
        .expect("current Anthropic catalog should replace stale metadata");
    assert_eq!(info.context_window, 1_000_000);
    assert!(info.effort_mode("max").is_some());
    assert_eq!(mgr.build("anthropic-user").unwrap().id(), "anthropic-user");

    let quota = mgr
        .quota_capability("anthropic-user")
        .unwrap()
        .expect("Anthropic subscription accounts expose usage");
    assert_eq!(quota.descriptor.auth, firmius_core::QuotaAuth::WebSession);
    assert!(quota.descriptor.meters.contains(&"five_hour".into()));
    assert!(quota.source.is_some());
}

#[test]
fn anthropic_subscription_rejects_incomplete_oauth_credentials() {
    let mut mgr = ProviderManager::new();
    mgr.register_kind(Arc::new(AnthropicSubscriptionKind));
    mgr.register_account(AccountRecord {
        id: "anthropic-broken".into(),
        kind: "anthropic".into(),
        schema: firmius_core::kinds::anthropic_subscription::schema_template("anthropic-broken"),
        credentials: serde_json::json!({ "access_token": "access-only" }),
    });

    let error = mgr.build("anthropic-broken").err().unwrap();
    assert!(error.contains("missing a refresh token"), "got: {error}");
}

#[test]
fn set_api_key_is_a_noop_for_unknown_providers() {
    let mut mgr = ProviderManager::new();
    mgr.set_api_key("ghost", "k");
    assert!(mgr.account("ghost").is_none());
}

#[test]
fn kinds_registry_lists_and_resolves() {
    let mut mgr = ProviderManager::new();
    mgr.register_kind(Arc::new(OpencodeGoKind));
    mgr.register_kind(Arc::new(AlibabaTokenPlanKind));
    let names = mgr.kind_names();
    assert_eq!(names, vec!["alibaba-token-plan", "api-key", "opencode-go"]);
    assert!(mgr.kind("opencode-go").is_some());
    assert!(mgr.kind("oauth-something").is_none());
}

#[tokio::test]
async fn wizard_outcome_registers_persists_and_builds() {
    let dir = tmp_dir("wizard-register");
    let kind = OpencodeGoKind;
    let mut wizard = kind.wizard();
    let (schema, credentials) = run_wizard(wizard.as_mut(), ["oc-key"]).await.unwrap();

    let mut mgr = ProviderManager::new().with_data_dir(dir.clone());
    mgr.register_kind(Arc::new(OpencodeGoKind));
    mgr.register_account(AccountRecord {
        id: schema.id.clone(),
        kind: "opencode-go".to_string(),
        schema,
        credentials,
    });
    mgr.save_account_file("opencode-go").unwrap();

    // A fresh manager (product kinds registered explicitly — load() does
    // not invent kinds) sees the account and can build it.
    let mut reloaded = ProviderManager::new().with_data_dir(dir);
    reloaded.register_kind(Arc::new(OpencodeGoKind));
    reloaded.load().unwrap();
    let provider = reloaded.build("opencode-go").unwrap();
    assert_eq!(provider.id(), "opencode-go");
}

// ---------------------------------------------------------------------------
// Wizards — driven through the headless harness
// ---------------------------------------------------------------------------

#[tokio::test]
async fn opencode_go_wizard_is_one_key_step() {
    let kind = OpencodeGoKind;
    let mut wizard = kind.wizard();
    assert!(matches!(
        wizard.start().await,
        Step::Prompt { secret: true, .. }
    ));

    let (schema, credentials) = run_wizard(wizard.as_mut(), ["oc-key"]).await.unwrap();
    assert_eq!(schema.id, "opencode-go");
    assert_eq!(schema.base_url.as_deref(), Some(OPENCODE_GO_BASE_URL));
    assert_eq!(schema.api_type, ApiType::OpenAI);
    assert_eq!(schema.models.len(), 25);
    assert_eq!(credentials["api_key"], "oc-key");
}

#[tokio::test]
async fn opencode_go_wizard_rejects_empty_key() {
    let mut wizard = OpencodeGoKind.wizard();
    wizard.start().await;
    let err = wizard.answer("   ".to_string()).await.unwrap_err();
    assert!(err.to_string().contains("must not be empty"));
}

#[tokio::test]
async fn alibaba_wizard_region_selects_base_url_and_is_persisted() {
    let kind = AlibabaTokenPlanKind;

    // China first: constrained select, then a key; region lands in both the
    // base URL and the stored credentials.
    let mut wizard = kind.wizard();
    assert!(matches!(wizard.start().await, Step::Select { .. }));
    let (schema, credentials) = run_wizard(wizard.as_mut(), ["china", "sk-sp-test"])
        .await
        .unwrap();
    assert_eq!(schema.base_url.as_deref(), Some(ALIBABA_CN_BASE_URL));
    assert_eq!(credentials["region"], "china");
    assert_eq!(credentials["api_key"], "sk-sp-test");
    assert_eq!(schema.models.len(), 17);

    // International variant.
    let (schema, _) = run_wizard(kind.wizard().as_mut(), ["international", "k"])
        .await
        .unwrap();
    assert_eq!(schema.base_url.as_deref(), Some(ALIBABA_INTL_BASE_URL));
}

#[tokio::test]
async fn alibaba_wizard_rejects_unknown_regions_with_choices() {
    let mut wizard = AlibabaTokenPlanKind.wizard();
    wizard.start().await;
    let err = wizard.answer("mars".to_string()).await.unwrap_err();
    let msg = err.to_string();
    assert!(msg.contains("international"), "got: {msg}");
    assert!(msg.contains("china"), "got: {msg}");
}

#[tokio::test]
async fn generic_api_key_wizard_walks_four_steps() {
    let kind = ApiKeyKind;
    let mut wizard = kind.wizard();
    let (schema, credentials) = run_wizard(
        wizard.as_mut(),
        ["custom", "anthropic", "https://custom.test", "key123"],
    )
    .await
    .unwrap();
    assert_eq!(schema.id, "custom");
    assert_eq!(schema.api_type, ApiType::Anthropic);
    assert_eq!(schema.base_url.as_deref(), Some("https://custom.test"));
    assert_eq!(credentials["api_key"], "key123");
}

#[tokio::test]
async fn generic_api_key_wizard_blank_base_url_means_default() {
    let (schema, _) = run_wizard(ApiKeyKind.wizard().as_mut(), ["custom2", "openai", "", "k"])
        .await
        .unwrap();
    assert_eq!(schema.api_type, ApiType::OpenAI);
    assert!(schema.base_url.is_none());
    assert_eq!(schema.effective_base_url(), "https://api.openai.com/v1");
}

#[tokio::test]
async fn run_wizard_errors_when_answers_run_out() {
    let err = run_wizard(OpencodeGoKind.wizard().as_mut(), std::iter::empty::<&str>())
        .await
        .unwrap_err();
    assert!(err.contains("ran out of answers"), "got: {err}");
}

// ---------------------------------------------------------------------------
// Select matching
// ---------------------------------------------------------------------------

#[test]
fn match_select_is_case_insensitive_and_names_valid_choices() {
    let step = Step::Select {
        label: "r".to_string(),
        options: vec![
            SelectOption::new("alpha", "Alpha"),
            SelectOption::new("beta", "Beta"),
        ],
    };
    assert_eq!(match_select(&step, " ALPHA ").unwrap(), "alpha");
    assert_eq!(match_select(&step, "beta").unwrap(), "beta");
    let err = match_select(&step, "gamma").unwrap_err();
    assert!(err.to_string().contains("alpha, beta"));

    // Non-select steps refuse to match.
    let prompt = Step::Prompt {
        label: "x".to_string(),
        secret: false,
    };
    assert!(match_select(&prompt, "x").is_err());
}
