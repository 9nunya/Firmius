#include <gtest/gtest.h>
#include "agents/HintingLoader.hpp"
#include "agents/PurposeLoader.hpp"
#include "ConfigLoader.hpp"
#include "Context.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

std::filesystem::path repoRootFromSourceFile() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

std::string readRepoFile(const std::filesystem::path &path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace

class PurposeLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto envOrEmpty = [](const char *name) {
            const char *value = std::getenv(name);
            return value ? std::optional<std::string>(value) : std::nullopt;
        };
        originalPromptsDirEnv = envOrEmpty("FIRMIUS_PROMPTS_DIR");
        originalHintingDirEnv = envOrEmpty("FIRMIUS_HINTING_DIR");
        originalHomeEnv = envOrEmpty("HOME");
        originalCwd = std::filesystem::current_path();

        testPromptsDir = std::filesystem::temp_directory_path() / "firmius_test_prompts";
        testHintingDir = std::filesystem::temp_directory_path() / "firmius_test_hinting";
        std::filesystem::create_directories(testPromptsDir);
        std::filesystem::create_directories(testHintingDir);
        setenv("FIRMIUS_PROMPTS_DIR", testPromptsDir.c_str(), 1);
        unsetenv("FIRMIUS_HINTING_DIR");

        // Create a dummy base.md
        std::ofstream baseFile(testPromptsDir / "base.md");
        baseFile << "Title: {{AGENT_TITLE}}\nName: {{AGENT_NAME}}\nCWD: {{CWD}}\nPurposes: {{REGISTERED_PURPOSES}}\nCustom: {{CUSTOM_VAR}}";
        baseFile.close();

        // Create a dummy persona
        std::ofstream coderFile(testPromptsDir / "test_coder.md");
        coderFile << "---\nname: test_coder\ntitle: Test Coder\ndescription: A test persona\nwork_role: lead\nscopes: [\"fs:read\"]\nswitchable: true\ncanSpawn: true\n---\nYou are a test coder.";
        coderFile.close();

        std::ofstream workerFile(testPromptsDir / "test_worker.md");
        workerFile << "---\nname: test_worker\ntitle: Test Worker\ndescription: Another persona\nwork_role: worker\nscopes: [\"fs:read\"]\nswitchable: false\ncanSpawn: false\n---\nYou are a test worker.";
        workerFile.close();
    }

    void TearDown() override {
        std::filesystem::remove_all(testPromptsDir);
        std::filesystem::remove_all(testHintingDir);
        std::filesystem::current_path(originalCwd);
        if (originalPromptsDirEnv.has_value()) {
            setenv("FIRMIUS_PROMPTS_DIR", originalPromptsDirEnv->c_str(), 1);
        } else {
            unsetenv("FIRMIUS_PROMPTS_DIR");
        }
        if (originalHintingDirEnv.has_value()) {
            setenv("FIRMIUS_HINTING_DIR", originalHintingDirEnv->c_str(), 1);
        } else {
            unsetenv("FIRMIUS_HINTING_DIR");
        }
        if (originalHomeEnv.has_value()) {
            setenv("HOME", originalHomeEnv->c_str(), 1);
        } else {
            unsetenv("HOME");
        }
    }

    std::filesystem::path testPromptsDir;
    std::filesystem::path testHintingDir;
    std::filesystem::path originalCwd;
    std::optional<std::string> originalPromptsDirEnv;
    std::optional<std::string> originalHintingDirEnv;
    std::optional<std::string> originalHomeEnv;
};

TEST_F(PurposeLoaderTest, isValid_check) {
    EXPECT_TRUE(PurposeLoader::isValid("test_coder"));
    EXPECT_FALSE(PurposeLoader::isValid("non_existent"));
}

TEST_F(PurposeLoaderTest, load_persona_switchable) {
    Persona persona = PurposeLoader::load("test_coder");
    EXPECT_EQ(persona.name, "test_coder");
    EXPECT_TRUE(persona.hasWorkRole);
    EXPECT_EQ(PurposeLoader::resolveWorkRole(persona), PurposeWorkRole::Lead);
    EXPECT_TRUE(persona.switchable);
}

TEST_F(PurposeLoaderTest, load_persona_defaultsMissingIdentityFields) {
    std::ofstream customFile(testPromptsDir / "test_custom.md");
    customFile << "---\nwork_role: executor\n---\nYou are a custom executor.";
    customFile.close();

    Persona persona = PurposeLoader::load("test_custom");
    EXPECT_EQ(persona.name, "test_custom");
    EXPECT_EQ(persona.title, "test_custom");
    EXPECT_TRUE(persona.hasWorkRole);
    EXPECT_EQ(persona.workRole, PurposeWorkRole::Executor);
    EXPECT_EQ(PurposeLoader::resolveWorkRole(persona),
              PurposeWorkRole::Executor);
}

TEST_F(PurposeLoaderTest, load_persona_acceptsQuotedWorkRoleMetadata) {
    std::ofstream customFile(testPromptsDir / "test_quoted.md");
    customFile << "---\nname: quoted\nwork_role: \"auditor\"\n---\nYou are a quoted auditor.";
    customFile.close();

    Persona persona = PurposeLoader::load("test_quoted");
    EXPECT_TRUE(persona.hasWorkRole);
    EXPECT_EQ(persona.workRole, PurposeWorkRole::Auditor);
    EXPECT_EQ(PurposeLoader::resolveWorkRole(persona),
              PurposeWorkRole::Auditor);
}

TEST_F(PurposeLoaderTest, load_persona_parsesCommentedFrontmatterMetadata) {
    std::ofstream customFile(testPromptsDir / "test_rich.md");
    customFile << "---\n"
                  "# purpose metadata\n"
                  "name: \"test_rich\"\n"
                  "title: 'Rich Persona'\n"
                  "description:  Custom metadata payload\n"
                  "work_role: 'executor'\n"
                  "scopes: ['fs:read', \"semantic\"]\n"
                  "switchable: yes\n"
                  "canSpawn: 1\n"
                  "---\n"
                  "You are a rich persona.";
    customFile.close();

    Persona persona = PurposeLoader::load("test_rich");
    EXPECT_EQ(persona.name, "test_rich");
    EXPECT_EQ(persona.title, "Rich Persona");
    EXPECT_EQ(persona.description, "Custom metadata payload");
    EXPECT_TRUE(persona.canSpawn);
    EXPECT_TRUE(persona.switchable);
    EXPECT_TRUE(persona.hasWorkRole);
    EXPECT_EQ(persona.workRole, PurposeWorkRole::Executor);
    ASSERT_EQ(persona.allowedScopes.size(), 2u);
    EXPECT_EQ(persona.allowedScopes[0], ToolScope::FilesystemRead);
    EXPECT_EQ(persona.allowedScopes[1], ToolScope::Semantic);
    EXPECT_EQ(persona.purposeKey, "test_rich");
  }

TEST_F(PurposeLoaderTest, resolveWorkRoleFallsBackForLegacyBuiltins) {
    EXPECT_EQ(PurposeLoader::resolveWorkRole("lead"), PurposeWorkRole::Lead);
    EXPECT_EQ(PurposeLoader::resolveWorkRole("hotrun"), PurposeWorkRole::Lead);
    EXPECT_EQ(PurposeLoader::resolveWorkRole("executor"),
              PurposeWorkRole::Executor);
}

TEST_F(PurposeLoaderTest, load_personaDoesNotInferRoleFromFrontmatterName) {
    std::ofstream customFile(testPromptsDir / "test_named_lead.md");
    customFile << "---\nname: lead\ntitle: Custom Lead Label\n---\nYou are not actually lead-role.";
    customFile.close();

    Persona persona = PurposeLoader::load("test_named_lead");
    EXPECT_FALSE(persona.hasWorkRole);
    EXPECT_EQ(persona.name, "lead");
    EXPECT_EQ(persona.purposeKey, "test_named_lead");
    EXPECT_EQ(PurposeLoader::resolveWorkRole(persona),
              PurposeWorkRole::Unknown);
}

TEST_F(PurposeLoaderTest, load_persona_rejects_invalidWorkRoleMetadata) {
    std::ofstream invalidFile(testPromptsDir / "test_invalid.md");
    invalidFile << "---\nname: test_invalid\ntitle: Bad Persona\nwork_role: wizard\n---\nYou are invalid.";
    invalidFile.close();

    EXPECT_THROW(PurposeLoader::load("test_invalid"), std::runtime_error);
}

TEST_F(PurposeLoaderTest, load_persona_rejectsMalformedScopesMetadata) {
    std::ofstream invalidFile(testPromptsDir / "test_invalid_scopes.md");
    invalidFile << "---\nname: test_invalid_scopes\nscopes: [\"FilesystemRead\",\n---\nYou are invalid.";
    invalidFile.close();

    EXPECT_THROW(PurposeLoader::load("test_invalid_scopes"),
                 std::runtime_error);
}

TEST_F(PurposeLoaderTest, listPurposesIncludesNonSwitchablePersonas) {
    const auto all = PurposeLoader::listPurposes();
    const auto switchable = PurposeLoader::listSwitchablePurposes();

    EXPECT_NE(std::find(all.begin(), all.end(), "test_coder"), all.end());
    EXPECT_NE(std::find(all.begin(), all.end(), "test_worker"), all.end());
    EXPECT_NE(std::find(switchable.begin(), switchable.end(), "test_coder"),
              switchable.end());
    EXPECT_EQ(std::find(switchable.begin(), switchable.end(), "test_worker"),
              switchable.end());
}

TEST_F(PurposeLoaderTest, composeSystemPrompt_placeholders) {
    Persona persona;
    persona.name = "test_coder";
    persona.title = "Test Coder";
    persona.identityPrompt = "You are a test coder.";

    AgentContext ctx;
    ctx.environment.cwd = "/home/user/work";
    ctx.environment.identifier = "test-host";
    ctx.config.providerId = "openai";
    ctx.config.modelId = "gpt-5";

    PurposeLoader::registerPlaceholder("{{CUSTOM_VAR}}", "hello-world");

    auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
    cfg.modelRouterCategories.clear();
    cfg.modelRouterCategories["code"] = {"openai", "gpt-5-codex", "thinking"};
    cfg.defaultRouteCategory = "code";
    firmius::shared::ConfigLoader::instance().updateConfig(cfg);

    std::string prompt = PurposeLoader::composeSystemPrompt(persona, ctx, "");

    EXPECT_TRUE(prompt.find("Title: Test Coder") != std::string::npos);
    EXPECT_TRUE(prompt.find("Name: test_coder") != std::string::npos);
    EXPECT_TRUE(prompt.find("CWD: /home/user/work") != std::string::npos);
    EXPECT_TRUE(prompt.find("Purposes: test_coder") != std::string::npos);
    EXPECT_TRUE(prompt.find("Custom: hello-world") != std::string::npos);
    EXPECT_TRUE(prompt.find("Model Route Categories: code") != std::string::npos);
    EXPECT_TRUE(prompt.find("Default Route Category: code") != std::string::npos);
}

TEST(ModelHintResolverTest, DetectFamilyFromModelName) {
  EXPECT_EQ(ModelHintResolver::detectFamily("openai", "gpt-5", ""), "gpt");
  EXPECT_EQ(ModelHintResolver::detectFamily("openai", "gpt-5.4-mini", ""), "gpt");
  EXPECT_EQ(ModelHintResolver::detectFamily("openai", "gpt-5.3-codex", ""), "gpt");
  EXPECT_EQ(ModelHintResolver::detectFamily("foo", "openai/gpt-4o", ""), "gpt");
  EXPECT_EQ(ModelHintResolver::detectFamily("foo", "o3-mini", ""), "gpt");
    EXPECT_EQ(ModelHintResolver::detectFamily("anthropic", "claude-3-7-sonnet", ""), "claude");
    EXPECT_EQ(ModelHintResolver::detectFamily("google", "gemini-2.5-pro", ""), "gemini");
    EXPECT_EQ(ModelHintResolver::detectFamily("openrouter", "qwen-2.5-coder", ""), "qwen");
    EXPECT_EQ(ModelHintResolver::detectFamily("openrouter", "qwq-32b", ""), "qwen");
    EXPECT_EQ(ModelHintResolver::detectFamily("openrouter", "deepseek-chat", ""), "deepseek");
    EXPECT_EQ(ModelHintResolver::detectFamily("openrouter", "mistral-large", ""), "generic");
}

TEST_F(PurposeLoaderTest, resolveHintingDirHonorsResolutionChain) {
    auto envDir = testHintingDir / "env";
    auto homeRoot = testHintingDir / "home";
    auto userDir = homeRoot / ".firmius" / "hinting";
    std::filesystem::create_directories(envDir);
    std::filesystem::create_directories(userDir);

    {
        std::ofstream f(envDir / "generic.md");
        f << "---\nname: generic\n---\nenv";
    }
    {
        std::ofstream f(userDir / "generic.md");
        f << "---\nname: generic\n---\nuser";
    }

    setenv("FIRMIUS_HINTING_DIR", envDir.c_str(), 1);
    setenv("HOME", homeRoot.c_str(), 1);
    EXPECT_EQ(HintingLoader::resolveHintingDir(), envDir.string() + "/");

    unsetenv("FIRMIUS_HINTING_DIR");
    EXPECT_EQ(HintingLoader::resolveHintingDir(), userDir.string() + "/");
}

TEST_F(PurposeLoaderTest, hintingLoaderParsesFrontmatterAndBody) {
    std::filesystem::create_directories(testHintingDir);
    setenv("FIRMIUS_HINTING_DIR", testHintingDir.c_str(), 1);
    {
        std::ofstream f(testHintingDir / "gpt.md");
        f << "---\nname: gpt\ntitle: GPT Overlay\ndescription: test\nbuiltin: true\nenabled: true\npriority: 7\n---\nDo the work.\n";
    }

    auto overlay = HintingLoader::loadByFamily("gpt");
    ASSERT_TRUE(overlay.has_value());
    EXPECT_EQ(overlay->name, "gpt");
    EXPECT_EQ(overlay->title, "GPT Overlay");
    EXPECT_EQ(overlay->description, "test");
    EXPECT_TRUE(overlay->builtin);
    EXPECT_EQ(overlay->priority, 7);
    EXPECT_EQ(overlay->body, "Do the work.");
}

TEST_F(PurposeLoaderTest, unknownFamilyFallsBackToGenericHinting) {
    setenv("FIRMIUS_HINTING_DIR", testHintingDir.c_str(), 1);
    {
        std::ofstream f(testHintingDir / "generic.md");
        f << "---\nname: generic\n---\nGeneric overlay.";
    }

    auto overlay = HintingLoader::loadForModel("any", "unknown-model", "");
    ASSERT_TRUE(overlay.has_value());
    EXPECT_EQ(overlay->name, "generic");
    EXPECT_EQ(overlay->body, "Generic overlay.");
}

TEST_F(PurposeLoaderTest, composeSystemPromptIncludesHintingOverlay) {
    setenv("FIRMIUS_HINTING_DIR", testHintingDir.c_str(), 1);
    {
        std::ofstream f(testHintingDir / "gpt.md");
        f << "---\nname: gpt\n---\nRun checks before finishing.";
    }

    Persona persona;
    persona.name = "test_coder";
    persona.title = "Test Coder";
    persona.identityPrompt = "You are a test coder.";

    AgentContext ctx;
    ctx.environment.cwd = "/home/user/work";
    ctx.environment.identifier = "test-host";
    ctx.config.providerId = "openai";
    ctx.config.modelId = "gpt-4o";

    std::string prompt = PurposeLoader::composeSystemPrompt(persona, ctx, "");
    EXPECT_NE(prompt.find("# MODEL-SPECIFIC HINTING"), std::string::npos);
    EXPECT_NE(prompt.find("Detected Family: gpt"), std::string::npos);
    EXPECT_NE(prompt.find("Hinting File: gpt"), std::string::npos);
    EXPECT_NE(prompt.find("Run checks before finishing."), std::string::npos);
}

TEST_F(PurposeLoaderTest, composeSystemPromptIncludesGeminiOverlay) {
    setenv("FIRMIUS_HINTING_DIR", testHintingDir.c_str(), 1);
    {
        std::ofstream f(testHintingDir / "gemini.md");
        f << "---\nname: gemini\n---\nUse tools before claims.";
    }

    Persona persona;
    persona.name = "test_coder";
    persona.title = "Test Coder";
    persona.identityPrompt = "You are a test coder.";

    AgentContext ctx;
    ctx.environment.cwd = "/home/user/work";
    ctx.environment.identifier = "test-host";
    ctx.config.providerId = "google";
    ctx.config.modelId = "gemini-2.5-pro";

    std::string prompt = PurposeLoader::composeSystemPrompt(persona, ctx, "");
    EXPECT_NE(prompt.find("Detected Family: gemini"), std::string::npos);
    EXPECT_NE(prompt.find("Hinting File: gemini"), std::string::npos);
    EXPECT_NE(prompt.find("Use tools before claims."), std::string::npos);
}

TEST_F(PurposeLoaderTest, userOverrideHintingWinsOverBuiltinHinting) {
    auto homeRoot = testHintingDir / "override-home";
    auto userDir = homeRoot / ".firmius" / "hinting";
    auto fakeCwd = testHintingDir / "repo-like";
    auto builtinDir = fakeCwd / "hinting";
    std::filesystem::create_directories(userDir);
    std::filesystem::create_directories(builtinDir);

    {
        std::ofstream f(userDir / "gpt.md");
        f << "---\nname: gpt\n---\nUser override.";
    }
    {
        std::ofstream f(builtinDir / "gpt.md");
        f << "---\nname: gpt\n---\nBuiltin hint.";
    }

    unsetenv("FIRMIUS_HINTING_DIR");
    setenv("HOME", homeRoot.c_str(), 1);
    std::filesystem::current_path(fakeCwd);

    auto overlay = HintingLoader::loadByFamily("gpt");
    ASSERT_TRUE(overlay.has_value());
    EXPECT_EQ(overlay->body, "User override.");
}

TEST_F(PurposeLoaderTest, malformedHintingFileFailsGracefullyToGeneric) {
    setenv("FIRMIUS_HINTING_DIR", testHintingDir.c_str(), 1);
    auto isolatedHome = testHintingDir / "isolated-home";
    std::filesystem::create_directories(isolatedHome);
    setenv("HOME", isolatedHome.c_str(), 1);
    std::filesystem::current_path(testHintingDir);
    {
        std::ofstream f(testHintingDir / "gpt.md");
        f << "---\nname: gpt\n---\n";
    }
    {
        std::ofstream f(testHintingDir / "generic.md");
        f << "---\nname: generic\n---\nGeneric fallback body.";
    }

    EXPECT_NO_THROW({
        auto overlay = HintingLoader::loadForModel("openai", "gpt-4o", "");
        ASSERT_TRUE(overlay.has_value());
        EXPECT_EQ(overlay->name, "generic");
    });
}

TEST_F(PurposeLoaderTest, bootstrapHintingDefaultsIgnoresUnwritableUserCache) {
    auto builtinDir = testHintingDir / "builtin-hinting";
    std::filesystem::create_directories(builtinDir);
    {
        std::ofstream f(builtinDir / "gpt.md");
        f << "---\nname: gpt\n---\nbase";
    }
    {
        std::ofstream f(builtinDir / "generic.md");
        f << "---\nname: generic\n---\nbase";
    }

    auto fakeHome = testHintingDir / "home";
    auto blockedParent = fakeHome / ".firmius";
    auto blockedTarget = blockedParent / "hinting";
    std::filesystem::create_directories(blockedParent);
    std::ofstream blocker(blockedTarget);
    blocker << "not-a-directory";
    blocker.close();

    setenv("HOME", fakeHome.c_str(), 1);
    EXPECT_NO_THROW(HintingLoader::bootstrapDefaults(builtinDir.string()));
}

TEST_F(PurposeLoaderTest, resolvePromptsDirFallsBackWhenEnvDirIsUnreadableShape) {
    auto brokenDir = testPromptsDir / "broken";
    std::filesystem::create_directories(brokenDir);
    setenv("FIRMIUS_PROMPTS_DIR", brokenDir.c_str(), 1);
    auto fakeHome = testPromptsDir / "empty-home";
    std::filesystem::create_directories(fakeHome);
    setenv("HOME", fakeHome.c_str(), 1);

    std::string resolved = PurposeLoader::resolvePromptsDir();

    EXPECT_EQ(resolved, "prompts/");
}

TEST_F(PurposeLoaderTest, bootstrapDefaultsIgnoresUnwritableUserPromptCache) {
    auto builtinDir = testPromptsDir / "builtin";
    std::filesystem::create_directories(builtinDir);

    {
        std::ofstream baseFile(builtinDir / "base.md");
        baseFile << "base";
    }
    {
        std::ofstream compactionFile(builtinDir / "COMPACTION_PROMPT.md");
        compactionFile << "compact";
    }

    auto fakeHome = testPromptsDir / "home";
    auto blockedParent = fakeHome / ".firmius";
    auto blockedTarget = blockedParent / "prompts";
    std::filesystem::create_directories(blockedParent);
    std::ofstream blocker(blockedTarget);
    blocker << "not-a-directory";
    blocker.close();

    setenv("HOME", fakeHome.c_str(), 1);

    EXPECT_NO_THROW(PurposeLoader::bootstrapDefaults(builtinDir.string()));
}

TEST(PromptContractsTest, basePromptRequiresNarrativeTextBetweenToolEpisodes) {
    const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "base.md");

    EXPECT_NE(prompt.find("Between tool-call episodes, emit concise plain-text progress or decision updates"),
              std::string::npos);
    EXPECT_NE(prompt.find("send it in a separate plain-text message between tool-call messages"),
              std::string::npos);
    EXPECT_NE(prompt.find("Only tools that exist in the current Firmius tool list are real."),
              std::string::npos);
    EXPECT_NE(prompt.find("`apply_patch` is not a Firmius tool and not a shell command in this harness."),
              std::string::npos);
    EXPECT_NE(prompt.find("Do not call `apply_patch` through `process_execute`."),
              std::string::npos);
    EXPECT_NE(prompt.find("FILE_EDIT MODE DECISION (SHORT RULE):"),
              std::string::npos);
    EXPECT_NE(prompt.find("mix `content` with Hashline `edits` in one `file_edit` call"),
              std::string::npos);
    EXPECT_NE(prompt.find("MODE SELECTION EXAMPLES"),
              std::string::npos);
}

TEST(PromptContractsTest, leadPromptRequiresAcceptanceBeforeDone) {
    const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "lead.md");

    EXPECT_NE(prompt.find("Executor self-report is not acceptance. The lead must review before any chunk becomes `Done`."),
              std::string::npos);
    EXPECT_NE(prompt.find("Do not create detailed downstream implementation chunks that assume an unresolved design/spec decision as committed truth."),
              std::string::npos);
    EXPECT_NE(prompt.find("A design/spec chunk is a planning gate. Its dependent detailed chunks stay blocked or generic until the lead reviews and accepts that design."),
              std::string::npos);
    EXPECT_NE(prompt.find("After a design/spec executor returns, inspect the proposed design yourself before using it as execution truth or unblocking detailed dependents."),
              std::string::npos);
    EXPECT_NE(prompt.find("After every `subagent_wait`, perform an explicit acceptance step before changing a chunk to `Done`."),
              std::string::npos);
    EXPECT_NE(prompt.find("Executors report implementation progress such as `Implemented`; the lead reviews and decides whether to accept, retry, audit, replan, or mark `Done`."),
              std::string::npos);
    EXPECT_NE(prompt.find("Chunks are delegated/reviewable work surfaces, not personal TODO notes."),
              std::string::npos);
    EXPECT_NE(prompt.find("If you intend to implement the next change personally, usually do that direct work without creating a chunk first."),
              std::string::npos);
    EXPECT_NE(prompt.find("After `chunk_add`, the normal next step is dispatch (`summon_subagent`) or waiting for dependency truth, not direct self-execution by lead."),
              std::string::npos);
}

TEST(PromptContractsTest, executorPromptRequiresVerificationEvidenceAndLeadAcceptance) {
    const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "executor.md");

    EXPECT_NE(prompt.find("Do not mark the chunk `Done`; the lead reviews and decides `Done`."),
              std::string::npos);
    EXPECT_NE(prompt.find("Verification evidence means concrete commands, tests, or outputs, not a vibe check."),
              std::string::npos);
    EXPECT_NE(prompt.find("Do not claim completion without evidence in `result_summary`."),
              std::string::npos);
}

TEST(PromptContractsTest, basePromptDefinesRecommendedTodoShapesByRole) {
    const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "base.md");

    EXPECT_NE(prompt.find("RECOMMENDED DEFAULT TODO SHAPES BY ROLE"),
              std::string::npos);
    EXPECT_NE(prompt.find("`hotrun`: reconstruct thread/runtime truth -> build issue ledger"),
              std::string::npos);
    EXPECT_NE(prompt.find("`scout`: restate bounded question -> inspect the minimum relevant files"),
              std::string::npos);
}

TEST(PromptContractsTest, scoutPromptDefinesMultiStepTodoApproach) {
    const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "scout.md");

    EXPECT_NE(prompt.find("Use `todo_write` when the reconnaissance is clearly multi-step."),
              std::string::npos);
    EXPECT_NE(prompt.find("Restate the bounded question in concrete terms"),
              std::string::npos);
    EXPECT_NE(prompt.find("If the answer is obvious after one or two direct inspections, skip the todo"),
              std::string::npos);
}

TEST(PromptContractsTest, hotrunPromptIsSwitchableRemediationLead) {
    const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "hotrun.md");

    EXPECT_NE(prompt.find("switchable: true"), std::string::npos);
    EXPECT_NE(prompt.find("canSpawn: true"), std::string::npos);
    EXPECT_NE(prompt.find("You are a top-level remediation lead."), std::string::npos);
    EXPECT_NE(prompt.find("fix waves"), std::string::npos);
}

TEST(HintingContractsTest, builtinGptHintingDefendsAgainstAskingAndPrematureCompletion) {
    const auto prompt = readRepoFile(repoRootFromSourceFile() / "hinting" / "gpt.md");

    EXPECT_NE(prompt.find("Do not ask the user whether to run builds, tests, reads, diffs, or reviews. Do them."),
              std::string::npos);
    EXPECT_NE(prompt.find("If you can name the next tool call, you should usually be making it instead of summarizing it."),
              std::string::npos);
    EXPECT_NE(prompt.find("subagent_wait"), std::string::npos);
    EXPECT_NE(prompt.find("file_read"), std::string::npos);
    EXPECT_NE(prompt.find("file_edit"), std::string::npos);
    EXPECT_NE(prompt.find("process_execute"), std::string::npos);
    EXPECT_NE(prompt.find("Only tools listed in the active Firmius tool block are real."),
              std::string::npos);
    EXPECT_NE(prompt.find("`apply_patch` is not an available Firmius tool or shell command."),
              std::string::npos);
    EXPECT_NE(prompt.find("never use `process_execute` as an editing tunnel"),
              std::string::npos);
    EXPECT_NE(prompt.find("never mix `content` with Hashline `edits` in one `file_edit` call"),
              std::string::npos);
    EXPECT_NE(prompt.find("if you are personally doing the next direct change, do it without manufacturing a chunk"),
              std::string::npos);
}

TEST(HintingContractsTest, builtinGeminiHintingDefendsAgainstNoToolAndOptimism) {
    const auto prompt = readRepoFile(repoRootFromSourceFile() / "hinting" / "gemini.md");

    EXPECT_NE(prompt.find("When the user asks you to do work, a response with no tool calls is usually a failed response."),
              std::string::npos);
    EXPECT_NE(prompt.find("Your optimism is not evidence."), std::string::npos);
    EXPECT_NE(prompt.find("process_execute"), std::string::npos);
    EXPECT_NE(prompt.find("summon_subagent"), std::string::npos);
    EXPECT_NE(prompt.find("chunk_ready_for_execution"), std::string::npos);
    EXPECT_NE(prompt.find("Only tools present in the active Firmius tool list are valid."),
              std::string::npos);
    EXPECT_NE(prompt.find("`apply_patch` is not a Firmius tool and not a shell command in this harness."),
              std::string::npos);
    EXPECT_NE(prompt.find("`process_execute` for verification or inspection, never as a file editing tunnel"),
              std::string::npos);
    EXPECT_NE(prompt.find("never mix `content` with Hashline `edits` in one `file_edit` call"),
              std::string::npos);
    EXPECT_NE(prompt.find("If you commit a chunk, treat it as a dispatch/review unit rather than a personal TODO note."),
              std::string::npos);
}
