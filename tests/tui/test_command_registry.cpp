#include <gtest/gtest.h>

#include "CommandRegistry.hpp"
#include "ModalSystem.hpp"
#include "AppState.hpp"

using namespace firmius::tui;

class CommandRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = std::make_unique<CommandRegistry>();
        auto state = std::make_shared<AppState>();
        ModalSystem modalSystem(state);
        registry_->init(modalSystem);
    }

    std::unique_ptr<CommandRegistry> registry_;
};

TEST_F(CommandRegistryTest, ExecuteValidCommand) {
    auto cmd = registry_->findCommand("model");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_TRUE(registry_->execute("/model gpt-4"));
}

TEST_F(CommandRegistryTest, ExecuteInvalidCommand) {
    EXPECT_FALSE(registry_->execute("/invalidcommand"));
}

TEST_F(CommandRegistryTest, ExecuteNotACommand) {
    EXPECT_FALSE(registry_->execute("hello world"));
}

TEST_F(CommandRegistryTest, ExecuteEmptyString) {
    EXPECT_FALSE(registry_->execute(""));
}

TEST_F(CommandRegistryTest, ExecuteJustSlash) {
    EXPECT_FALSE(registry_->execute("/"));
}

TEST_F(CommandRegistryTest, FindExactMatch) {
    auto cmd = registry_->findCommand("model");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "model");
}

TEST_F(CommandRegistryTest, FindNonExistentCommand) {
    auto cmd = registry_->findCommand("nonexistent");
    EXPECT_FALSE(cmd.has_value());
}

TEST_F(CommandRegistryTest, FuzzyFindModel) {
    auto cmd = registry_->findCommand("mdel");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "model");
}

TEST_F(CommandRegistryTest, FuzzyFindModels) {
    auto cmd = registry_->findCommand("mdels");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "models");
}

TEST_F(CommandRegistryTest, FuzzyFindThread) {
    auto cmd = registry_->findCommand("thred");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "thread");
}

TEST_F(CommandRegistryTest, FuzzyFindConfig) {
    auto cmd = registry_->findCommand("cnfig");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "config");
}

TEST_F(CommandRegistryTest, FuzzyFindUndo) {
    auto cmd = registry_->findCommand("und");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "undo");
}

TEST_F(CommandRegistryTest, FuzzyFindNew) {
    registry_->findCommand("neww");
}

TEST_F(CommandRegistryTest, FuzzyFindClear) {
    registry_->findCommand("clearr");
}

TEST_F(CommandRegistryTest, FuzzyFindCompact) {
    registry_->findCommand("compactt");
}

TEST_F(CommandRegistryTest, FuzzyFindFocus) {
    auto cmd = registry_->findCommand("focs");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "focus");
}

TEST_F(CommandRegistryTest, FuzzyFindTooDifferent) {
    auto cmd = registry_->findCommand("xyzabc");
    EXPECT_FALSE(cmd.has_value());
}

TEST_F(CommandRegistryTest, GetAllCommands) {
    auto commands = registry_->getCommands();
    EXPECT_GE(commands.size(), 10u);
}

TEST_F(CommandRegistryTest, RegisterNewCommand) {
    Command customCmd;
    customCmd.name = "custom";
    customCmd.description = "A custom command";
    customCmd.usage = "/custom [arg]";
    customCmd.handler = [](const std::vector<std::string>&) {};
    
    registry_->registerCommand(std::move(customCmd));
    
    auto cmd = registry_->findCommand("custom");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "custom");
}

TEST_F(CommandRegistryTest, ExecuteWithNoArgs) {
    EXPECT_TRUE(registry_->execute("/undo"));
}

TEST_F(CommandRegistryTest, ExecuteWithMultipleArgs) {
    EXPECT_TRUE(registry_->execute("/model gpt-4 temperature 0.7"));
}

TEST_F(CommandRegistryTest, ExecuteWithSpaces) {
    auto cmd = registry_->findCommand("undo");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_TRUE(registry_->execute("/undo   1  "));
}

TEST_F(CommandRegistryTest, ModelCommandExists) {
    auto cmd = registry_->findCommand("model");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "Switch the current model");
    EXPECT_EQ(cmd->usage, "/model <provider>://<model>");
}

TEST_F(CommandRegistryTest, ModelsCommandExists) {
    auto cmd = registry_->findCommand("models");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "List available models");
    EXPECT_EQ(cmd->usage, "/models");
}

TEST_F(CommandRegistryTest, ThreadCommandExists) {
    auto cmd = registry_->findCommand("thread");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "Switch to a specific thread");
    EXPECT_EQ(cmd->usage, "/thread <thread_id>");
}

TEST_F(CommandRegistryTest, ThreadsCommandExists) {
    auto cmd = registry_->findCommand("threads");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "List all threads");
    EXPECT_EQ(cmd->usage, "/threads");
}

TEST_F(CommandRegistryTest, ConfigCommandExists) {
    auto cmd = registry_->findCommand("config");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "Show configuration editor");
    EXPECT_EQ(cmd->usage, "/config");
}

TEST_F(CommandRegistryTest, UndoCommandExists) {
    auto cmd = registry_->findCommand("undo");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "Undo last N turns");
    EXPECT_EQ(cmd->usage, "/undo [n]");
}

TEST_F(CommandRegistryTest, NewCommandExists) {
    auto cmd = registry_->findCommand("new");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "Start a new thread");
    EXPECT_EQ(cmd->usage, "/new [cwd]");
}

TEST_F(CommandRegistryTest, ClearCommandExists) {
    auto cmd = registry_->findCommand("clear");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "Clear chat history");
    EXPECT_EQ(cmd->usage, "/clear");
}

TEST_F(CommandRegistryTest, CompactCommandExists) {
    auto cmd = registry_->findCommand("compact");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "Force context compaction");
    EXPECT_EQ(cmd->usage, "/compact");
}

TEST_F(CommandRegistryTest, FocusCommandExists) {
    auto cmd = registry_->findCommand("focus");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->description, "Focus on a specific agent");
    EXPECT_EQ(cmd->usage, "/focus <agent_id>");
}
