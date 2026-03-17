#include "ICommandIntent.hpp"
#include "environment/CommandIntentAnalyzer.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace firmius::core;
using namespace firmius::shared;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class CommandIntentAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override {
        analyzer = std::make_unique<CommandIntentAnalyzer>();
    }

    std::unique_ptr<CommandIntentAnalyzer> analyzer;
};

// Basic command parsing
TEST_F(CommandIntentAnalyzerTest, ParsesSimpleCommand) {
    auto intent = analyzer->analyze("ls -la", "/home/user");
    
    EXPECT_EQ(intent.originalCommand, "ls -la");
    EXPECT_EQ(intent.primaryCommand, "ls");
    EXPECT_THAT(intent.arguments, ElementsAre("-la"));
    EXPECT_EQ(intent.severity, CommandSeverity::LOW);
}

TEST_F(CommandIntentAnalyzerTest, ParsesCommandWithMultipleArgs) {
    auto intent = analyzer->analyze("grep -r \"pattern\" /path/to/search", "/home/user");
    
    EXPECT_EQ(intent.primaryCommand, "grep");
    EXPECT_THAT(intent.arguments, ElementsAre("-r", "\"pattern\"", "/path/to/search"));
}

// Command chain parsing
TEST_F(CommandIntentAnalyzerTest, SplitsSemicolonCommands) {
    auto intent = analyzer->analyze("cd /tmp; ls -la; pwd", "/home/user");
    
    EXPECT_THAT(intent.parsedCommands, ElementsAre("cd /tmp", "ls -la", "pwd"));
}

TEST_F(CommandIntentAnalyzerTest, SplitsAndCommands) {
    auto intent = analyzer->analyze("make && make install", "/home/user");
    
    EXPECT_THAT(intent.parsedCommands, ElementsAre("make", "make install"));
}

TEST_F(CommandIntentAnalyzerTest, SplitsOrCommands) {
    auto intent = analyzer->analyze("./script.sh || echo 'Failed'", "/home/user");
    
    EXPECT_THAT(intent.parsedCommands, ElementsAre("./script.sh", "echo 'Failed'"));
}

TEST_F(CommandIntentAnalyzerTest, HandlesMixedChains) {
    auto intent = analyzer->analyze("cd /tmp && ls -la; pwd || echo 'error'", "/home/user");

    EXPECT_EQ(intent.parsedCommands.size(), 4);
}

// Pipes and subshells
TEST_F(CommandIntentAnalyzerTest, DetectsPipes) {
    auto intent = analyzer->analyze("cat file.txt | grep pattern | wc -l", "/home/user");
    
    EXPECT_TRUE(intent.hasPipesOrSubshells);
    EXPECT_EQ(intent.severity, CommandSeverity::MEDIUM);
}

TEST_F(CommandIntentAnalyzerTest, DetectsSubshells) {
    auto intent = analyzer->analyze("echo $(date)", "/home/user");
    
    EXPECT_TRUE(intent.hasPipesOrSubshells);
}

TEST_F(CommandIntentAnalyzerTest, DetectsBackticks) {
    auto intent = analyzer->analyze("echo `date`", "/home/user");
    
    EXPECT_TRUE(intent.hasPipesOrSubshells);
}

// Elevation detection
TEST_F(CommandIntentAnalyzerTest, DetectsSudo) {
    auto intent = analyzer->analyze("sudo apt install package", "/home/user");
    
    EXPECT_TRUE(intent.usesElevation);
    EXPECT_EQ(intent.primaryCommand, "apt");
}

TEST_F(CommandIntentAnalyzerTest, DetectsSu) {
    auto intent = analyzer->analyze("su -c 'whoami'", "/home/user");
    
    EXPECT_TRUE(intent.usesElevation);
}

// Vulnerable command detection
TEST_F(CommandIntentAnalyzerTest, DetectsRmRfRoot) {
    auto intent = analyzer->analyze("rm -rf /", "/home/user");
    
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
    EXPECT_TRUE(intent.isDestructive);
}

TEST_F(CommandIntentAnalyzerTest, DetectsRmRfHome) {
    auto intent = analyzer->analyze("rm -rf ~", "/home/user");
    
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
}

TEST_F(CommandIntentAnalyzerTest, DetectsRmRfHomeSlash) {
    auto intent = analyzer->analyze("rm -rf ~/", "/home/user");
    
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
}

TEST_F(CommandIntentAnalyzerTest, DetectsSudoRmRf) {
    auto intent = analyzer->analyze("sudo rm -rf /home", "/home/user");
    
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
}

// Git destructive commands
TEST_F(CommandIntentAnalyzerTest, DetectsGitResetHard) {
    auto intent = analyzer->analyze("git reset --hard HEAD~1", "/home/user/project");
    
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
    EXPECT_THAT(intent.severityReason, ::testing::HasSubstr("git"));
}

TEST_F(CommandIntentAnalyzerTest, DetectsGitCheckoutForce) {
    auto intent = analyzer->analyze("git checkout -f feature-branch", "/home/user/project");
    
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
}

TEST_F(CommandIntentAnalyzerTest, AllowsGitStatus) {
    auto intent = analyzer->analyze("git status", "/home/user/project");
    
    EXPECT_EQ(intent.severity, CommandSeverity::LOW);
}

// DD operations
TEST_F(CommandIntentAnalyzerTest, DetectsDdToDev) {
    auto intent = analyzer->analyze("dd if=/dev/zero of=/dev/sda", "/home/user");
    
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
}

// Path extraction
TEST_F(CommandIntentAnalyzerTest, ExtractsReadPaths) {
    auto intent = analyzer->analyze("cat /etc/passwd", "/home/user");
    
    EXPECT_THAT(intent.filesRead, Contains("/etc/passwd"));
}

TEST_F(CommandIntentAnalyzerTest, ExtractsWritePaths) {
    auto intent = analyzer->analyze("cp file.txt /backup/", "/home/user");
    
    EXPECT_THAT(intent.filesWritten, Contains("/backup/file.txt"));
}

TEST_F(CommandIntentAnalyzerTest, ResolvesRelativePaths) {
    auto intent = analyzer->analyze("cat ../config.txt", "/home/user/project");
    
    EXPECT_THAT(intent.filesRead, Contains("/home/user/config.txt"));
}

TEST_F(CommandIntentAnalyzerTest, ExpandsTilde) {
    auto intent = analyzer->analyze("cat ~/.bashrc", "/home/user");
    
    // Should expand to actual home path
    ASSERT_FALSE(intent.filesRead.empty());
    EXPECT_THAT(intent.filesRead[0], ::testing::HasSubstr(".bashrc"));
}

// Multi-subcommand path detection
TEST_F(CommandIntentAnalyzerTest, ExtractsPathsFromAllSubcommands) {
    auto intent = analyzer->analyze("cat /etc/passwd; cat /etc/shadow", "/home/user");
    
    EXPECT_EQ(intent.filesRead.size(), 2);
    EXPECT_THAT(intent.filesRead, Contains("/etc/passwd"));
    EXPECT_THAT(intent.filesRead, Contains("/etc/shadow"));
}

TEST_F(CommandIntentAnalyzerTest, DetectsWriteInChain) {
    auto intent = analyzer->analyze("cat input.txt | tee output.txt", "/home/user");
    
    EXPECT_THAT(intent.filesRead, Contains("/home/user/input.txt"));
    EXPECT_THAT(intent.filesWritten, Contains("/home/user/output.txt"));
}

// Environment variables
TEST_F(CommandIntentAnalyzerTest, ExtractsEnvVarAssignment) {
    auto intent = analyzer->analyze("FOO=bar echo $FOO", "/home/user");
    
    EXPECT_THAT(intent.environmentVariables, ::testing::Contains(std::make_pair("FOO", "bar")));
}

TEST_F(CommandIntentAnalyzerTest, ExtractsEnvVarReference) {
    auto intent = analyzer->analyze("echo $HOME", "/home/user");
    
    EXPECT_THAT(intent.environmentVariables, ::testing::Contains(std::make_pair("HOME", "")));
}

// Severity assessment
TEST_F(CommandIntentAnalyzerTest, LowSeverityForLs) {
    auto intent = analyzer->analyze("ls -la", "/home/user");
    
    EXPECT_EQ(intent.severity, CommandSeverity::LOW);
}

TEST_F(CommandIntentAnalyzerTest, MediumSeverityForElevatedLs) {
    auto intent = analyzer->analyze("sudo ls /root", "/home/user");
    
    EXPECT_EQ(intent.severity, CommandSeverity::MEDIUM);
}

TEST_F(CommandIntentAnalyzerTest, HighSeverityForRm) {
    auto intent = analyzer->analyze("rm important.txt", "/home/user");
    
    EXPECT_EQ(intent.severity, CommandSeverity::HIGH);
    EXPECT_TRUE(intent.isDestructive);
}

TEST_F(CommandIntentAnalyzerTest, HighSeverityForRmdir) {
    auto intent = analyzer->analyze("rmdir mydir", "/home/user");
    
    EXPECT_EQ(intent.severity, CommandSeverity::HIGH);
}

// Complex scenarios
TEST_F(CommandIntentAnalyzerTest, ComplexMultiCommandWithPaths) {
    auto intent = analyzer->analyze(
        "cd /tmp && cat input.log | grep ERROR | tee /var/log/errors.txt; rm input.log",
        "/home/user"
    );
    
    EXPECT_TRUE(intent.hasPipesOrSubshells);
    EXPECT_THAT(intent.filesRead, Contains("/tmp/input.log"));
    EXPECT_THAT(intent.filesWritten, Contains("/var/log/errors.txt"));
    EXPECT_EQ(intent.severity, CommandSeverity::HIGH); // Due to rm
}

TEST_F(CommandIntentAnalyzerTest, HiddenVulnerableInSubshell) {
    auto intent = analyzer->analyze("echo $(sudo rm -rf /home/user/docs)", "/home/user");
    
    // Even in subshell, should detect the vulnerability
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
}

// Edge cases
TEST_F(CommandIntentAnalyzerTest, EmptyCommand) {
    auto intent = analyzer->analyze("", "/home/user");
    
    EXPECT_TRUE(intent.parsedCommands.empty());
    EXPECT_EQ(intent.severity, CommandSeverity::LOW);
}

TEST_F(CommandIntentAnalyzerTest, WhitespaceOnly) {
    auto intent = analyzer->analyze("   ", "/home/user");
    
    EXPECT_TRUE(intent.parsedCommands.empty());
}

TEST_F(CommandIntentAnalyzerTest, CommandWithQuotes) {
    auto intent = analyzer->analyze("echo 'hello world'", "/home/user");
    
    EXPECT_EQ(intent.primaryCommand, "echo");
    EXPECT_THAT(intent.arguments, ElementsAre("'hello world'"));
}

TEST_F(CommandIntentAnalyzerTest, CommandWithDoubleQuotes) {
    auto intent = analyzer->analyze("echo \"hello world\"", "/home/user");
    
    EXPECT_EQ(intent.primaryCommand, "echo");
    EXPECT_THAT(intent.arguments, ElementsAre("\"hello world\""));
}

// Summary generation
TEST_F(CommandIntentAnalyzerTest, GeneratesSummary) {
    auto intent = analyzer->analyze("ls -la /tmp", "/home/user");
    
    EXPECT_FALSE(intent.summary.empty());
}

// Direct severity assessment
TEST_F(CommandIntentAnalyzerTest, AssessSeverityModifiesIntent) {
    CommandIntent intent;
    intent.originalCommand = "rm -rf /tmp/test";
    intent.primaryCommand = "rm";
    intent.isDestructive = true;
    intent.filesWritten.push_back("/tmp/test");
    
    auto severity = analyzer->assessSeverity(intent);
    
    EXPECT_EQ(severity, CommandSeverity::HIGH);
    EXPECT_EQ(intent.severity, CommandSeverity::HIGH);
}