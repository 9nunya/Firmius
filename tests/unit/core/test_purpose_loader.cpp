#include <gtest/gtest.h>
#include "agents/PurposeLoader.hpp"
#include "Context.hpp"
#include <filesystem>
#include <fstream>

using namespace firmius::core;
using namespace firmius::shared;

class PurposeLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        testPromptsDir = std::filesystem::temp_directory_path() / "firmius_test_prompts";
        std::filesystem::create_directories(testPromptsDir);
        setenv("FIRMIUS_PROMPTS_DIR", testPromptsDir.c_str(), 1);

        // Create a dummy base.md
        std::ofstream baseFile(testPromptsDir / "base.md");
        baseFile << "Title: {{AGENT_TITLE}}\nName: {{AGENT_NAME}}\nCWD: {{CWD}}\nPurposes: {{REGISTERED_PURPOSES}}\nCustom: {{CUSTOM_VAR}}";
        baseFile.close();

        // Create a dummy persona
        std::ofstream coderFile(testPromptsDir / "test_coder.md");
        coderFile << "---\nname: test_coder\ntitle: Test Coder\ndescription: A test persona\nscopes: [\"fs:read\"]\nstop: [\"<done />\"]\ncanSpawn: true\n---\nYou are a test coder.";
        coderFile.close();
    }

    void TearDown() override {
        std::filesystem::remove_all(testPromptsDir);
        unsetenv("FIRMIUS_PROMPTS_DIR");
    }

    std::filesystem::path testPromptsDir;
};

TEST_F(PurposeLoaderTest, isValid_check) {
    EXPECT_TRUE(PurposeLoader::isValid("test_coder"));
    EXPECT_FALSE(PurposeLoader::isValid("non_existent"));
}

TEST_F(PurposeLoaderTest, load_persona_stop) {
    Persona persona = PurposeLoader::load("test_coder");
    EXPECT_EQ(persona.name, "test_coder");
    ASSERT_EQ(persona.stopSequences.size(), 1);
    EXPECT_EQ(persona.stopSequences[0], "<done />");
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
