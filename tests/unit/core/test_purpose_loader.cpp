#include <gtest/gtest.h>
#include "agents/PurposeLoader.hpp"
#include "Context.hpp"
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
        originalHomeEnv = envOrEmpty("HOME");

        testPromptsDir = std::filesystem::temp_directory_path() / "firmius_test_prompts";
        std::filesystem::create_directories(testPromptsDir);
        setenv("FIRMIUS_PROMPTS_DIR", testPromptsDir.c_str(), 1);

        // Create a dummy base.md
        std::ofstream baseFile(testPromptsDir / "base.md");
        baseFile << "Title: {{AGENT_TITLE}}\nName: {{AGENT_NAME}}\nCWD: {{CWD}}\nPurposes: {{REGISTERED_PURPOSES}}\nCustom: {{CUSTOM_VAR}}";
        baseFile.close();

        // Create a dummy persona
        std::ofstream coderFile(testPromptsDir / "test_coder.md");
        coderFile << "---\nname: test_coder\ntitle: Test Coder\ndescription: A test persona\nscopes: [\"fs:read\"]\nswitchable: true\ncanSpawn: true\n---\nYou are a test coder.";
        coderFile.close();
    }

    void TearDown() override {
        std::filesystem::remove_all(testPromptsDir);
        if (originalPromptsDirEnv.has_value()) {
            setenv("FIRMIUS_PROMPTS_DIR", originalPromptsDirEnv->c_str(), 1);
        } else {
            unsetenv("FIRMIUS_PROMPTS_DIR");
        }
        if (originalHomeEnv.has_value()) {
            setenv("HOME", originalHomeEnv->c_str(), 1);
        }
    }

    std::filesystem::path testPromptsDir;
    std::optional<std::string> originalPromptsDirEnv;
    std::optional<std::string> originalHomeEnv;
};

TEST_F(PurposeLoaderTest, isValid_check) {
    EXPECT_TRUE(PurposeLoader::isValid("test_coder"));
    EXPECT_FALSE(PurposeLoader::isValid("non_existent"));
}

TEST_F(PurposeLoaderTest, load_persona_switchable) {
    Persona persona = PurposeLoader::load("test_coder");
    EXPECT_EQ(persona.name, "test_coder");
    EXPECT_TRUE(persona.switchable);
}

TEST_F(PurposeLoaderTest, composeSystemPrompt_placeholders) {
    Persona persona;
    persona.name = "test_coder";
    persona.title = "Test Coder";
    persona.identityPrompt = "You are a test coder.";

    AgentContext ctx;
    ctx.environment.cwd = "/home/user/work";
    ctx.environment.identifier = "test-host";

    PurposeLoader::registerPlaceholder("{{CUSTOM_VAR}}", "hello-world");

    std::string prompt = PurposeLoader::composeSystemPrompt(persona, ctx, "");

    EXPECT_TRUE(prompt.find("Title: Test Coder") != std::string::npos);
    EXPECT_TRUE(prompt.find("Name: test_coder") != std::string::npos);
    EXPECT_TRUE(prompt.find("CWD: /home/user/work") != std::string::npos);
    EXPECT_TRUE(prompt.find("Purposes: test_coder") != std::string::npos);
    EXPECT_TRUE(prompt.find("Custom: hello-world") != std::string::npos);
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
