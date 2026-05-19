#include "tools/TodoWriteTool.hpp"
#include "persistence/ThreadManager.hpp"
#include "hosts/LocalHost.hpp"
#include "../mocks/MockEnvironment.hpp"
#include "Context.hpp"
#include "IAgent.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <rapidjson/document.h>
#include <string>

using namespace firmius::core;
using namespace firmius::shared;
using ::testing::HasSubstr;
using ::testing::Not;

namespace {

class TestAgent : public IAgent {
public:
  TestAgent(const std::string &threadId, const std::string &agentId) {
    context_.history = std::make_shared<AgentHistory>();
    context_.history->threadId = threadId;
    context_.identity.id = agentId;
    environment_ = std::make_shared<firmius::test::MockEnvironment>();
    permissions_ = std::make_shared<firmius::test::MockPermissions>();
  }

  void reset() override {}
  void run(const std::string &, std::function<void(const StreamEvent &)>,
           const std::vector<ImageContent> &) override {}
  void resume(std::function<void(const StreamEvent &)>) override {}
  void interrupt() override {}
  bool isInterrupted() const override { return false; }
  void clearInterrupt() override {}
  void compactNow(std::function<void(const StreamEvent &)>) override {}
  void setModel(const std::string &, const std::string &) override {}
  void setModel(const std::string &, const std::string &,
                const std::string &) override {}
  bool isRunning() const override { return false; }
  bool isBooting() const override { return false; }
  void setBooting(bool) override {}
  const AgentContext &getContext() const override { return context_; }
  AgentContext &getMutableContext() override { return context_; }
  void saveHistory() override {}
  void appendHistoryTurn(const AgentTurn &turn) override {
    if (!context_.history) {
      context_.history = std::make_shared<AgentHistory>();
    }
    context_.history->turns.push_back(turn);
  }

  ModelChoice getPreferredModel() const override {
    ModelChoice choice;
    choice.providerId = context_.config.providerId;
    choice.modelId = context_.config.modelId;
    return choice;
  }

  std::shared_ptr<IEnvironment> getEnvironment() const override {
    return environment_;
  }
  std::shared_ptr<IPermissions> getPermissions() const override {
    return permissions_;
  }
  std::shared_ptr<firmius::shared::IHost> getHost() override {
    return environment_->getHost();
  }

private:
  AgentContext context_;
  std::shared_ptr<IEnvironment> environment_;
  std::shared_ptr<IPermissions> permissions_;
};

rapidjson::Document parseResultData(const ToolResult &result) {
  rapidjson::Document doc;
  doc.Parse(result.data.c_str());
  return doc;
}

} // namespace

class TodoWriteToolTest : public ::testing::Test {
protected:
  std::string threadId_;
  std::string agentId_;
  std::shared_ptr<LocalHost> host_;
  std::shared_ptr<TestAgent> agent_;
  std::filesystem::path testHome_;
  std::filesystem::path threadsBase_;
  std::string originalHome_;
  bool hadHome_ = false;
  TodoWriteTool tool_;

  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_todo_test_home_" +
                 std::to_string(std::chrono::system_clock::now()
                                    .time_since_epoch()
                                    .count()));
    threadsBase_ = testHome_ / ".firmius" / "threads";
    std::filesystem::create_directories(threadsBase_);
    hadHome_ = std::getenv("HOME") != nullptr;
    originalHome_ = hadHome_ ? std::getenv("HOME") : "";
    setenv("HOME", testHome_.c_str(), 1);

    ThreadManager tm(ThreadManager::defaultBasePath());
    ThreadMetadata metadata;
    metadata.title = "Todo Tool Test";
    metadata.cwd = testHome_.string();
    metadata.hostOptions.type = HostType::Local;
    metadata.leadPersona = "lead";
    threadId_ = tm.createThread(metadata);
    agentId_ = "test-agent";
    host_ = std::make_shared<LocalHost>();
    agent_ = std::make_shared<TestAgent>(threadId_, agentId_);
  }

  void TearDown() override {
    std::filesystem::remove_all(testHome_);
    if (hadHome_) {
      setenv("HOME", originalHome_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
  }

  ToolContext makeCtx() { return ToolContext{*host_, *agent_, "todo-call"}; }

  ToolResult run(const std::string &json) {
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    EXPECT_FALSE(doc.HasParseError())
        << "Test JSON failed to parse: " << json;
    auto ctx = makeCtx();
    return tool_.execute(doc, ctx);
  }

  AgentTodoList readPersisted() {
    ThreadManager tm(ThreadManager::defaultBasePath());
    return tm.getAgentTodo(threadId_, agentId_);
  }
};

// ---------------------------------------------------------------------------
// schema
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, SchemaOnlyRequiresAction) {
  auto schema = tool_.getSchema();
  ASSERT_NE(schema, nullptr);
  // Empty input must fail (action missing).
  rapidjson::Document doc;
  doc.SetObject();
  auto v = schema->validate(doc, "root");
  EXPECT_FALSE(v.success);
  EXPECT_THAT(v.error, HasSubstr("action"));
}

TEST_F(TodoWriteToolTest, SchemaAcceptsPreferredArrayShapes) {
  auto schema = tool_.getSchema();
  ASSERT_NE(schema, nullptr);

  rapidjson::Document addDoc;
  addDoc.Parse(R"({"action":"add","items":["one",{"text":"two","status":"done"}]})");
  ASSERT_FALSE(addDoc.HasParseError());
  auto v1 = schema->validate(addDoc, "root");
  EXPECT_TRUE(v1.success) << v1.violationToPretty();

  rapidjson::Document completeDoc;
  completeDoc.Parse(R"({"action":"complete","ids":[1,2,3]})");
  ASSERT_FALSE(completeDoc.HasParseError());
  auto v2 = schema->validate(completeDoc, "root");
  EXPECT_TRUE(v2.success) << v2.violationToPretty();
}

TEST_F(TodoWriteToolTest, MissingActionFails) {
  auto result = run(R"({})");
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("action"));
}

TEST_F(TodoWriteToolTest, UnknownActionFails) {
  auto result = run(R"({"action":"banana"})");
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("Unknown action"));
}

// ---------------------------------------------------------------------------
// list & clear
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, ListOnEmptyReturnsEmpty) {
  auto result = run(R"({"action":"list"})");
  EXPECT_TRUE(result.success) << result.error;
  auto doc = parseResultData(result);
  EXPECT_TRUE(doc.HasMember("result"));
  EXPECT_THAT(std::string(doc["result"].GetString()), HasSubstr("empty"));
  EXPECT_TRUE(doc["items"].IsArray());
  EXPECT_EQ(doc["items"].Size(), 0u);
}

TEST_F(TodoWriteToolTest, ClearOnEmptyIsIdempotent) {
  auto result = run(R"({"action":"clear"})");
  EXPECT_TRUE(result.success) << result.error;
  auto doc = parseResultData(result);
  EXPECT_THAT(std::string(doc["result"].GetString()),
              HasSubstr("already empty"));
}

// ---------------------------------------------------------------------------
// add
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, AddSingleStringShorthand) {
  auto result = run(R"({"action":"add","items":"buy milk"})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  ASSERT_EQ(persisted.items.size(), 1u);
  EXPECT_EQ(persisted.items[0].text, "buy milk");
  EXPECT_EQ(persisted.items[0].status, TodoStatus::Pending);
  EXPECT_EQ(persisted.items[0].id, 1);
}

TEST_F(TodoWriteToolTest, AddArrayOfStrings) {
  auto result = run(
      R"({"action":"add","items":["one","two","three"]})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  ASSERT_EQ(persisted.items.size(), 3u);
  EXPECT_EQ(persisted.items[0].text, "one");
  EXPECT_EQ(persisted.items[1].text, "two");
  EXPECT_EQ(persisted.items[2].text, "three");
  EXPECT_EQ(persisted.items[0].id, 1);
  EXPECT_EQ(persisted.items[1].id, 2);
  EXPECT_EQ(persisted.items[2].id, 3);
}

TEST_F(TodoWriteToolTest, AddJsonEncodedArrayStringIsNormalized) {
  auto result = run(
      R"({"action":"add","items":"[{\"text\":\"one\"},{\"text\":\"two\",\"status\":\"in_progress\"}]"})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  ASSERT_EQ(persisted.items.size(), 2u);
  EXPECT_EQ(persisted.items[0].text, "one");
  EXPECT_EQ(persisted.items[0].status, TodoStatus::Pending);
  EXPECT_EQ(persisted.items[1].text, "two");
  EXPECT_EQ(persisted.items[1].status, TodoStatus::InProgress);
}

TEST_F(TodoWriteToolTest, AddObjectsWithStatus) {
  auto result = run(
      R"({"action":"add","items":[{"text":"running","status":"in_progress"},{"text":"queued"}]})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  ASSERT_EQ(persisted.items.size(), 2u);
  EXPECT_EQ(persisted.items[0].status, TodoStatus::InProgress);
  EXPECT_EQ(persisted.items[1].status, TodoStatus::Pending);
}

TEST_F(TodoWriteToolTest, AddPreservesExistingItems) {
  EXPECT_TRUE(run(R"({"action":"add","items":"first"})").success);
  EXPECT_TRUE(run(R"({"action":"add","items":"second"})").success);
  auto persisted = readPersisted();
  ASSERT_EQ(persisted.items.size(), 2u);
  EXPECT_EQ(persisted.items[0].text, "first");
  EXPECT_EQ(persisted.items[1].text, "second");
  EXPECT_EQ(persisted.items[1].id, 2);
}

TEST_F(TodoWriteToolTest, AddRequiresItems) {
  auto result = run(R"({"action":"add"})");
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("items"));
}

TEST_F(TodoWriteToolTest, AddRejectsEmptyText) {
  auto result = run(R"({"action":"add","items":["   "]})");
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("empty"));
}

// ---------------------------------------------------------------------------
// complete
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, CompleteSingularIdMarksDone) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a","b","c"]})").success);
  auto result = run(R"({"action":"complete","id":2})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  EXPECT_EQ(persisted.items[0].status, TodoStatus::Pending);
  EXPECT_EQ(persisted.items[1].status, TodoStatus::Done);
  EXPECT_EQ(persisted.items[2].status, TodoStatus::Pending);
}

TEST_F(TodoWriteToolTest, CompletePluralIdsMarksMultiple) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a","b","c"]})").success);
  auto result = run(R"({"action":"complete","ids":[1,3]})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  EXPECT_EQ(persisted.items[0].status, TodoStatus::Done);
  EXPECT_EQ(persisted.items[1].status, TodoStatus::Pending);
  EXPECT_EQ(persisted.items[2].status, TodoStatus::Done);
}

TEST_F(TodoWriteToolTest, CompleteJsonEncodedIdsStringIsNormalized) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a","b","c"]})").success);
  auto result = run(R"({"action":"complete","ids":"[1,3]"})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  EXPECT_EQ(persisted.items[0].status, TodoStatus::Done);
  EXPECT_EQ(persisted.items[1].status, TodoStatus::Pending);
  EXPECT_EQ(persisted.items[2].status, TodoStatus::Done);
}

TEST_F(TodoWriteToolTest, CompleteMissingIdEmitsWarningButDoesNotFail) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a"]})").success);
  auto result = run(R"({"action":"complete","ids":[1,99]})");
  EXPECT_TRUE(result.success) << result.error;
  auto doc = parseResultData(result);
  EXPECT_THAT(std::string(doc["result"].GetString()),
              HasSubstr("99 not found"));
}

TEST_F(TodoWriteToolTest, CompleteRequiresIds) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a"]})").success);
  auto result = run(R"({"action":"complete"})");
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("id"));
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, UpdateChangesStatusViaShorthand) {
  ASSERT_TRUE(run(R"({"action":"add","items":["x"]})").success);
  auto result = run(R"({"action":"update","id":1,"status":"in_progress"})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  EXPECT_EQ(persisted.items[0].status, TodoStatus::InProgress);
  EXPECT_EQ(persisted.items[0].text, "x");
}

TEST_F(TodoWriteToolTest, UpdateChangesTextAndStatusViaArray) {
  ASSERT_TRUE(run(R"({"action":"add","items":["original"]})").success);
  auto result = run(
      R"({"action":"update","updates":[{"id":1,"text":"renamed","status":"done"}]})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  EXPECT_EQ(persisted.items[0].text, "renamed");
  EXPECT_EQ(persisted.items[0].status, TodoStatus::Done);
}

TEST_F(TodoWriteToolTest, UpdateJsonEncodedUpdatesStringIsNormalized) {
  ASSERT_TRUE(run(R"({"action":"add","items":["original"]})").success);
  auto result = run(
      R"({"action":"update","updates":"[{\"id\":1,\"text\":\"renamed\",\"status\":\"done\"}]"})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  EXPECT_EQ(persisted.items[0].text, "renamed");
  EXPECT_EQ(persisted.items[0].status, TodoStatus::Done);
}

TEST_F(TodoWriteToolTest, UpdateRequiresIdAndAField) {
  ASSERT_TRUE(run(R"({"action":"add","items":["x"]})").success);
  auto missingId = run(R"({"action":"update","updates":[{"text":"foo"}]})");
  EXPECT_FALSE(missingId.success);
  EXPECT_THAT(missingId.error, HasSubstr("id"));

  auto missingField = run(R"({"action":"update","updates":[{"id":1}]})");
  EXPECT_FALSE(missingField.success);
  EXPECT_THAT(missingField.error, HasSubstr("status"));
}

// ---------------------------------------------------------------------------
// remove
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, RemoveDropsRequestedIds) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a","b","c"]})").success);
  auto result = run(R"({"action":"remove","ids":[1,3]})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  ASSERT_EQ(persisted.items.size(), 1u);
  EXPECT_EQ(persisted.items[0].id, 2);
  EXPECT_EQ(persisted.items[0].text, "b");
}

TEST_F(TodoWriteToolTest, RemoveMissingIdWarnsButSucceeds) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a","b"]})").success);
  auto result = run(R"({"action":"remove","ids":[2,42]})");
  EXPECT_TRUE(result.success) << result.error;
  auto doc = parseResultData(result);
  EXPECT_THAT(std::string(doc["result"].GetString()),
              HasSubstr("42 not found"));
  auto persisted = readPersisted();
  ASSERT_EQ(persisted.items.size(), 1u);
  EXPECT_EQ(persisted.items[0].id, 1);
}

// ---------------------------------------------------------------------------
// set
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, SetReplacesEntireList) {
  ASSERT_TRUE(run(R"({"action":"add","items":["old1","old2"]})").success);
  auto result =
      run(R"({"action":"set","items":["fresh1","fresh2","fresh3"]})");
  EXPECT_TRUE(result.success) << result.error;
  auto persisted = readPersisted();
  ASSERT_EQ(persisted.items.size(), 3u);
  EXPECT_EQ(persisted.items[0].text, "fresh1");
  EXPECT_EQ(persisted.items[1].text, "fresh2");
  EXPECT_EQ(persisted.items[2].text, "fresh3");
}

TEST_F(TodoWriteToolTest, SetEmptyItemsClears) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a","b"]})").success);
  auto result = run(R"({"action":"set","items":[]})");
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_TRUE(readPersisted().items.empty());
}

// ---------------------------------------------------------------------------
// clear
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, ClearWipesPersistedList) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a","b"]})").success);
  auto result = run(R"({"action":"clear"})");
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_TRUE(readPersisted().items.empty());
}

// ---------------------------------------------------------------------------
// lenient inputs and synonyms
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, ActionSynonymsResolve) {
  EXPECT_TRUE(run(R"({"action":"append","items":"x"})").success);
  EXPECT_TRUE(run(R"({"action":"done","id":1})").success);
  EXPECT_TRUE(run(R"({"action":"show"})").success);
  EXPECT_TRUE(run(R"({"action":"reset"})").success);
  EXPECT_TRUE(readPersisted().items.empty());
}

TEST_F(TodoWriteToolTest, StatusSynonymsResolve) {
  ASSERT_TRUE(run(R"({"action":"add","items":[{"text":"x","status":"WIP"}]})")
                  .success);
  EXPECT_EQ(readPersisted().items[0].status, TodoStatus::InProgress);
}

TEST_F(TodoWriteToolTest, IdAsStringAccepted) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a"]})").success);
  auto result = run(R"({"action":"complete","id":"1"})");
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ(readPersisted().items[0].status, TodoStatus::Done);
}

// ---------------------------------------------------------------------------
// canonical listing always returned
// ---------------------------------------------------------------------------

TEST_F(TodoWriteToolTest, ResultAlwaysContainsNumberedListing) {
  ASSERT_TRUE(run(R"({"action":"add","items":["alpha","beta"]})").success);
  auto result = run(R"({"action":"list"})");
  ASSERT_TRUE(result.success) << result.error;
  auto doc = parseResultData(result);
  const std::string prose = doc["result"].GetString();
  EXPECT_THAT(prose, HasSubstr("1. [ ] alpha"));
  EXPECT_THAT(prose, HasSubstr("2. [ ] beta"));
}

TEST_F(TodoWriteToolTest, ResultProseShowsStatusBreakdown) {
  ASSERT_TRUE(run(R"({"action":"add","items":["a","b","c"]})").success);
  ASSERT_TRUE(run(R"({"action":"complete","id":1})").success);
  auto result = run(R"({"action":"update","id":2,"status":"in_progress"})");
  ASSERT_TRUE(result.success) << result.error;
  auto doc = parseResultData(result);
  const std::string prose = doc["result"].GetString();
  EXPECT_THAT(prose, HasSubstr("1 done"));
  EXPECT_THAT(prose, HasSubstr("1 in-progress"));
  EXPECT_THAT(prose, HasSubstr("1 pending"));
}
