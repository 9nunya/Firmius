#include "utils/TerminalUtil.hpp"
#include "tools/ProcessTool.hpp"
#include "../mocks/MockAgent.hpp"
#include "../mocks/MockHost.hpp"
#include "../mocks/MockHostProcess.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::test;

namespace {

class ProcessInputRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        mockHost = std::make_shared<MockHost>();
        mockEnv = std::make_shared<MockEnvironment>(mockHost);
        mockPerms = std::make_shared<MockPermissions>();
        mockPerms->allowedPaths_ = {"/tmp"};

        AgentContext ctx;
        ctx.permissions.allowedScopes = {ToolScope::Process};
        ctx.permissions.allowedPaths = {"/tmp"};
        ctx.environment.cwd = "/tmp";

        mockAgent = std::make_unique<MockAgent>(ctx, mockEnv, mockPerms);
        tool = std::make_unique<ProcessTool>();
    }

    ToolResult executeInput(const std::string& processId, const std::string& text) {
        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("action", rapidjson::Value("Input", alloc).Move(), alloc);
        doc.AddMember("process_id", rapidjson::Value(processId.c_str(), alloc).Move(), alloc);
        doc.AddMember("input", rapidjson::Value(text.c_str(), alloc).Move(), alloc);
        ToolContext ctx{*mockHost, *mockAgent, "test_call"};
        return tool->execute(doc, ctx);
    }

    std::shared_ptr<MockHost> mockHost;
    std::shared_ptr<MockEnvironment> mockEnv;
    std::shared_ptr<MockPermissions> mockPerms;
    std::unique_ptr<MockAgent> mockAgent;
    std::unique_ptr<ProcessTool> tool;
};

TEST_F(ProcessInputRegressionTest, EscapeSequenceNewline) {
    std::string input = "Hello\\nWorld";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\nWorld");
}

TEST_F(ProcessInputRegressionTest, EscapeSequenceTab) {
    std::string input = "Col1\\tCol2";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Col1\tCol2");
}

TEST_F(ProcessInputRegressionTest, EscapeSequenceBackslash) {
    std::string input = "Path\\\\File";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Path\\File");
}

TEST_F(ProcessInputRegressionTest, EscapeSequenceCarriageReturn) {
    std::string input = "Line1\\rLine2";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Line1\rLine2");
}

TEST_F(ProcessInputRegressionTest, ControlTagEnter) {
    std::string input = "Hello{Enter}";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\n");
}

TEST_F(ProcessInputRegressionTest, ControlTagTab) {
    std::string input = "Hello{Tab}";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\t");
}

TEST_F(ProcessInputRegressionTest, CtrlCTranslatedToETX) {
    std::string result = TerminalUtil::translate("{Ctrl+C}");
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], '\x03');
}

TEST_F(ProcessInputRegressionTest, CtrlDTranslatedToEOT) {
    std::string result = TerminalUtil::translate("{Ctrl+D}");
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], '\x04');
}

TEST_F(ProcessInputRegressionTest, CtrlZTranslatedToSUB) {
    std::string result = TerminalUtil::translate("{Ctrl+Z}");
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], '\x1a');
}

TEST_F(ProcessInputRegressionTest, AltModifier) {
    std::string result = TerminalUtil::translate("{Alt+A}");
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], '\x1b');
    EXPECT_EQ(result[1], 'A');
}

TEST_F(ProcessInputRegressionTest, ArrowKeys) {
    EXPECT_EQ(TerminalUtil::translate("{Up}"), "\x1b[A");
    EXPECT_EQ(TerminalUtil::translate("{Down}"), "\x1b[B");
    EXPECT_EQ(TerminalUtil::translate("{Right}"), "\x1b[C");
    EXPECT_EQ(TerminalUtil::translate("{Left}"), "\x1b[D");
}

TEST_F(ProcessInputRegressionTest, FunctionKeys) {
    std::string f1 = TerminalUtil::translate("{F1}");
    EXPECT_FALSE(f1.empty());
    EXPECT_EQ(f1[0], '\x1b');
}

TEST_F(ProcessInputRegressionTest, MixedEscapeSequencesAndControlTags) {
    std::string input = "Line1\\nLine2{Enter}End";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Line1\nLine2\nEnd");
}

TEST_F(ProcessInputRegressionTest, LiteralNewlinePreserved) {
    std::string input = "Hello\nWorld";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\nWorld");
}

TEST_F(ProcessInputRegressionTest, ToolReturnsMeaningfulFeedback) {
    ON_CALL(mockEnv->mockProcessManager(), writeToProcess(testing::_, testing::_))
        .WillByDefault(testing::Invoke([this](const std::string& id, const std::string& data) {
            mockHost->writeToBackgroundProcess(id, data);
        }));

    MockHostProcessConfig procConfig;
    procConfig.systemId = "test-process-1";
    procConfig.running = true;
    auto mockProcess = std::make_unique<MockHostProcess>(procConfig);
    mockHost->registerBackgroundProcess("test-proc-id", std::move(mockProcess));

    auto result = executeInput("test-proc-id", "Hello\\n");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.data, "{}") << "REGRESSION: Tool must return meaningful feedback";

    rapidjson::Document doc;
    doc.Parse(result.data.c_str());
    EXPECT_TRUE(doc.HasMember("sent"));
    EXPECT_EQ(std::string(doc["sent"].GetString()), "Hello\n");
    EXPECT_TRUE(doc.HasMember("chars"));
    EXPECT_EQ(doc["chars"].GetInt(), 6);
}

TEST_F(ProcessInputRegressionTest, ToolTranslatesEscapeSequences) {
    ON_CALL(mockEnv->mockProcessManager(), writeToProcess(testing::_, testing::_))
        .WillByDefault(testing::Invoke([this](const std::string& id, const std::string& data) {
            mockHost->writeToBackgroundProcess(id, data);
        }));

    MockHostProcessConfig procConfig;
    procConfig.systemId = "test-process-1";
    procConfig.running = true;
    auto mockProcess = std::make_unique<MockHostProcess>(procConfig);
    mockHost->registerBackgroundProcess("test-proc-id", std::move(mockProcess));

    auto result = executeInput("test-proc-id", "Hello\\nWorld");
    ASSERT_TRUE(result.success);

    rapidjson::Document doc;
    doc.Parse(result.data.c_str());
    EXPECT_EQ(std::string(doc["sent"].GetString()), "Hello\nWorld");
}

TEST_F(ProcessInputRegressionTest, ToolTranslatesControlTags) {
    ON_CALL(mockEnv->mockProcessManager(), writeToProcess(testing::_, testing::_))
        .WillByDefault(testing::Invoke([this](const std::string& id, const std::string& data) {
            mockHost->writeToBackgroundProcess(id, data);
        }));

    MockHostProcessConfig procConfig;
    procConfig.systemId = "test-process-1";
    procConfig.running = true;
    auto mockProcess = std::make_unique<MockHostProcess>(procConfig);
    mockHost->registerBackgroundProcess("test-proc-id", std::move(mockProcess));

    auto result = executeInput("test-proc-id", "{Ctrl+C}");
    ASSERT_TRUE(result.success);

    rapidjson::Document doc;
    doc.Parse(result.data.c_str());
    std::string sent = doc["sent"].GetString();
    EXPECT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0], '\x03');
}

TEST_F(ProcessInputRegressionTest, EmptyInput) {
    ON_CALL(mockEnv->mockProcessManager(), writeToProcess(testing::_, testing::_))
        .WillByDefault(testing::Invoke([this](const std::string& id, const std::string& data) {
            mockHost->writeToBackgroundProcess(id, data);
        }));

    MockHostProcessConfig procConfig;
    procConfig.systemId = "test-process-1";
    procConfig.running = true;
    auto mockProcess = std::make_unique<MockHostProcess>(procConfig);
    mockHost->registerBackgroundProcess("test-proc-id", std::move(mockProcess));

    auto result = executeInput("test-proc-id", "");
    ASSERT_TRUE(result.success);

    rapidjson::Document doc;
    doc.Parse(result.data.c_str());
    EXPECT_TRUE(doc.HasMember("sent"));
    EXPECT_EQ(std::string(doc["sent"].GetString()), "");
}

} // namespace
