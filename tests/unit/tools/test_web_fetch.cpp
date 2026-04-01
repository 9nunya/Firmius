#include "IAgent.hpp"
#include "ITool.hpp"
#include "hosts/LocalHost.hpp"
#include "tools/WebFetchTool.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rapidjson/document.h>

using namespace firmius::core;
using namespace firmius::shared;
using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

#include "../mocks/MockEnvironment.hpp"

// Local MockAgent matching patterns in test_tools.cpp
class WebFetchMockAgent : public IAgent {
public:
  firmius::shared::AgentContext defaultCtx;
  std::shared_ptr<firmius::test::MockEnvironment> mockEnv_;
  std::shared_ptr<firmius::test::MockPermissions> mockPerms_;

  WebFetchMockAgent()
      : mockEnv_(std::make_shared<firmius::test::MockEnvironment>()),
        mockPerms_(std::make_shared<firmius::test::MockPermissions>()) {
    if (!defaultCtx.history) {
      defaultCtx.history = std::make_shared<AgentHistory>();
    }
  }

  std::shared_ptr<IEnvironment> getEnvironment() const override {
    return mockEnv_;
  }
  std::shared_ptr<IPermissions> getPermissions() const override {
    return mockPerms_;
  }

  MOCK_METHOD(void, reset, (), (override));
  MOCK_METHOD(void, run,
              (const std::string &, (std::function<void(const StreamEvent &)>),
               const std::vector<ImageContent> &),
              (override));
  MOCK_METHOD(void, resume, ((std::function<void(const StreamEvent &)>)),
              (override));
  MOCK_METHOD((const AgentContext &), getContext, (), (const, override));
  MOCK_METHOD(AgentContext &, getMutableContext, (), (override));
  MOCK_METHOD(ModelChoice, getPreferredModel, (), (const, override));
  MOCK_METHOD(void, interrupt, (), (override));
  MOCK_METHOD(bool, isInterrupted, (), (const, override));
  MOCK_METHOD(void, clearInterrupt, (), (override));
  MOCK_METHOD(void, compactNow, (std::function<void(const StreamEvent &)>),
              (override));
  MOCK_METHOD(void, saveHistory, (), (override));
  MOCK_METHOD(void, setModel, (const std::string &, const std::string &),
              (override));
  MOCK_METHOD(void, setModel,
              (const std::string &, const std::string &, const std::string &),
              (override));
  MOCK_METHOD(bool, isRunning, (), (const, override));
  MOCK_METHOD(bool, isBooting, (), (const, override));
  MOCK_METHOD(void, setBooting, (bool), (override));
  MOCK_METHOD((std::shared_ptr<IHost>), getHost, (), (override));
};

// ---------------------------------------------------------------------------
// Deterministic unit tests using raw curl (no mocks needed for the tool itself)
// ---------------------------------------------------------------------------

TEST(WebFetchToolTest, metadataReturnsWebFetchName) {
  WebFetchTool tool;
  auto meta = tool.getMetadata();
  EXPECT_EQ(meta.name, "web_fetch");
  EXPECT_EQ(meta.scope, ToolScope::Web);
}

TEST(WebFetchToolTest, schemaRequiresUrlField) {
  WebFetchTool tool;
  auto schema = tool.getSchema();
  ASSERT_NE(schema, nullptr);
  // Schema should require "url" — validate by checking the schema JSON
  rapidjson::Document doc;
  doc.Parse(schema->toString().c_str());
  ASSERT_TRUE(doc.HasMember("required"));
  bool hasUrl = false;
  for (auto &item : doc["required"].GetArray()) {
    if (std::string(item.GetString()) == "url")
      hasUrl = true;
  }
  EXPECT_TRUE(hasUrl);
}

TEST(WebFetchToolTest, malformedUrlFails) {
  WebFetchTool tool;
  NiceMock<WebFetchMockAgent> mockAgent;
  LocalHost localHost;
  localHost.init();

  mockAgent.mockPerms_->allowOutsideCwd_ = true;
  ON_CALL(mockAgent, getContext())
      .WillByDefault(ReturnRef(mockAgent.defaultCtx));

  ToolContext ctx{localHost, mockAgent, "test_call"};

  // Use a clearly invalid URL that curl will reject
  WebFetchInput input;
  input.url = "not-a-valid-url-xyz://[invalid]";
  auto result = tool.execute(input, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("Fetch failed"));

  localHost.destroy();
}

TEST(WebFetchToolTest, emptyUrlFails) {
  WebFetchTool tool;
  NiceMock<WebFetchMockAgent> mockAgent;
  LocalHost localHost;
  localHost.init();

  mockAgent.mockPerms_->allowOutsideCwd_ = true;
  ON_CALL(mockAgent, getContext())
      .WillByDefault(ReturnRef(mockAgent.defaultCtx));

  ToolContext ctx{localHost, mockAgent, "test_call"};

  WebFetchInput input;
  input.url = "";
  auto result = tool.execute(input, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("Fetch failed"));

  localHost.destroy();
}

// ---------------------------------------------------------------------------
// Real URL fetch test — uses a stable public endpoint
// ---------------------------------------------------------------------------

TEST(WebFetchToolTest, realUrlFetchSucceeds) {
  WebFetchTool tool;
  NiceMock<WebFetchMockAgent> mockAgent;
  LocalHost localHost;
  localHost.init();

  mockAgent.mockPerms_->allowOutsideCwd_ = true;
  ON_CALL(mockAgent, getContext())
      .WillByDefault(ReturnRef(mockAgent.defaultCtx));

  ToolContext ctx{localHost, mockAgent, "test_call"};

  WebFetchInput input;
  // Use httpbin.org which is a stable, simple test endpoint
  input.url = "https://httpbin.org/html";
  auto result = tool.execute(input, ctx);

  // If network is unavailable, report it explicitly rather than silently
  // passing
  if (!result.success) {
    // Check for network-level failures
    bool networkError = result.error.find("Fetch failed") != std::string::npos;
    if (networkError) {
      GTEST_SKIP() << "Network unavailable: " << result.error;
    }
  }

  ASSERT_TRUE(result.success) << "Expected success, got: " << result.error;

  rapidjson::Document doc;
  doc.Parse(result.data.c_str());
  ASSERT_TRUE(doc.HasMember("content"))
      << "Response should have 'content' field";
  ASSERT_TRUE(doc["content"].IsString());
  std::string content = doc["content"].GetString();
  EXPECT_FALSE(content.empty()) << "Content should not be empty";
  // httpbin.org/html returns HTML with <title>Herman Melville</title>
  // After htmlToMarkdown conversion, expect some content
  EXPECT_GT(content.size(), 10u) << "Content should be substantial";

  localHost.destroy();
}

TEST(WebFetchToolTest, realUrlFetchNotFoundReturnsHttpError) {
  WebFetchTool tool;
  NiceMock<WebFetchMockAgent> mockAgent;
  LocalHost localHost;
  localHost.init();

  mockAgent.mockPerms_->allowOutsideCwd_ = true;
  ON_CALL(mockAgent, getContext())
      .WillByDefault(ReturnRef(mockAgent.defaultCtx));

  ToolContext ctx{localHost, mockAgent, "test_call"};

  WebFetchInput input;
  // httpbin.org returns 404 for unknown paths
  input.url = "https://httpbin.org/status/404";
  auto result = tool.execute(input, ctx);

  if (!result.success &&
      result.error.find("Fetch failed") != std::string::npos) {
    GTEST_SKIP() << "Network unavailable: " << result.error;
  }

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("HTTP Error"));
  EXPECT_THAT(result.error, HasSubstr("404"));

  localHost.destroy();
}
