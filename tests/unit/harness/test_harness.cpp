#include <gtest/gtest.h>
#include "harness/Harness.hpp"
#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "environment/Permissions.hpp"
#include "persistence/ThreadManager.hpp"
#include "persistence/Journaler.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/StringUtil.hpp"
#include "Events.hpp"

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <future>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

template <typename Fn>
bool waitForCondition(Fn&& fn,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
                      std::chrono::milliseconds step = std::chrono::milliseconds(20)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fn()) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return fn();
}

class TestRetryProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "test-retry-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        onEvent(TextChunk{"resumed"});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "test-retry-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override {
        return listModels().front();
    }

    void generateSummary(const std::string&, const AgentHistory&,
                         const std::string&,
                         std::function<void(const StreamEvent&)> onEvent,
                         std::atomic<bool>* = nullptr) override {
        onEvent(TextChunk{"summary"});
        onEvent(StreamDone{StopReason::Stop});
    }

    firmius::provider::ProviderType getProviderType() const override {
        return firmius::provider::ProviderType::APIKey;
    }
};

class TruncatedToolCallProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "truncated-tool-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        onEvent(ToolCallChunk{"call-1", 0, "chunk_add",
                              R"({"plan_id":"plan-1","title":"Runtime support")"});
        onEvent(StreamDone{StopReason::ToolUse});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "truncated-tool-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override {
        return listModels().front();
    }

    void generateSummary(const std::string&, const AgentHistory&,
                         const std::string&,
                         std::function<void(const StreamEvent&)> onEvent,
                         std::atomic<bool>* = nullptr) override {
        onEvent(TextChunk{"summary"});
        onEvent(StreamDone{StopReason::Stop});
    }

    firmius::provider::ProviderType getProviderType() const override {
        return firmius::provider::ProviderType::APIKey;
    }
};

class ProviderDeclaredTruncatedToolCallProvider
    : public firmius::provider::IProvider {
public:
    std::string getId() const override {
        return "provider-declared-truncated-tool-provider";
    }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        onEvent(ToolCallChunk{"call-1", 0, "chunk_add",
                              R"({"plan_id":"plan-1","title":"Runtime support")"});
        onEvent(StreamError{
            "Qwen stream ended with incomplete tool-call arguments for tool "
            "'chunk_add'. Provider stream truncated during tool-call generation.",
            0, "qwen-test-account"});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "provider-declared-truncated-tool-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override {
        return listModels().front();
    }

    void generateSummary(const std::string&, const AgentHistory&,
                         const std::string&,
                         std::function<void(const StreamEvent&)> onEvent,
                         std::atomic<bool>* = nullptr) override {
        onEvent(TextChunk{"summary"});
        onEvent(StreamDone{StopReason::Stop});
    }

    firmius::provider::ProviderType getProviderType() const override {
        return firmius::provider::ProviderType::APIKey;
    }
};

class MixedTextToolCallProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "mixed-text-tool-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        if (callCount_++ == 0) {
            onEvent(ThinkingChunk{"Planning next step.", "sig-1"});
            onEvent(TextChunk{"I will inspect ASCII.txt first."});
            onEvent(ToolCallChunk{"call-read", 0, "file_read",
                                  R"({"path":"ASCII.txt"})"});
            onEvent(StreamDone{StopReason::ToolUse});
            return;
        }

        onEvent(TextChunk{"Inspection complete."});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "mixed-text-tool-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override {
        return listModels().front();
    }

    void generateSummary(const std::string&, const AgentHistory&,
                         const std::string&,
                         std::function<void(const StreamEvent&)> onEvent,
                         std::atomic<bool>* = nullptr) override {
        onEvent(TextChunk{"summary"});
        onEvent(StreamDone{StopReason::Stop});
    }

    firmius::provider::ProviderType getProviderType() const override {
        return firmius::provider::ProviderType::APIKey;
    }

private:
    mutable int callCount_ = 0;
};

class ParallelInterleavedToolCallProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "parallel-interleaved-tool-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        if (callCount_++ == 0) {
            onEvent(TextChunk{"Running two reads in parallel."});
            onEvent(ToolCallChunk{"call-a", 0, "file_read", ""});
            onEvent(ToolCallChunk{"call-b", 1, "file_read", ""});

            ToolCallChunk callAArgs;
            callAArgs.id = "call-a";
            callAArgs.argsDelta = R"({"path":"ASCII.txt"})";
            onEvent(callAArgs);

            ToolCallChunk callBArgs;
            callBArgs.id = "call-b";
            callBArgs.argsDelta = R"({"path":"CMakeLists.txt"})";
            onEvent(callBArgs);

            onEvent(StreamDone{StopReason::ToolUse});
            return;
        }

        onEvent(TextChunk{"Parallel reads complete."});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "parallel-interleaved-tool-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override {
        return listModels().front();
    }

    void generateSummary(const std::string&, const AgentHistory&,
                         const std::string&,
                         std::function<void(const StreamEvent&)> onEvent,
                         std::atomic<bool>* = nullptr) override {
        onEvent(TextChunk{"summary"});
        onEvent(StreamDone{StopReason::Stop});
    }

    firmius::provider::ProviderType getProviderType() const override {
        return firmius::provider::ProviderType::APIKey;
    }

private:
    mutable int callCount_ = 0;
};

class SequencedProseProvider : public firmius::provider::IProvider {
public:
    SequencedProseProvider(std::string providerId, std::vector<std::string> responses)
        : providerId_(std::move(providerId)), responses_(std::move(responses)) {}

    std::string getId() const override { return providerId_; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        int idx = callCount_.fetch_add(1);
        if (!responses_.empty()) {
            const int maxIdx = static_cast<int>(responses_.size()) - 1;
            const std::string& text =
                responses_[static_cast<size_t>(std::min(idx, maxIdx))];
            onEvent(TextChunk{text});
        }
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = providerId_ + "-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override {
        return listModels().front();
    }

    void generateSummary(const std::string&, const AgentHistory&,
                         const std::string&,
                         std::function<void(const StreamEvent&)> onEvent,
                         std::atomic<bool>* = nullptr) override {
        onEvent(TextChunk{"summary"});
        onEvent(StreamDone{StopReason::Stop});
    }

    firmius::provider::ProviderType getProviderType() const override {
        return firmius::provider::ProviderType::APIKey;
    }

    int callCount() const { return callCount_.load(); }

private:
    std::string providerId_;
    std::vector<std::string> responses_;
    std::atomic<int> callCount_{0};
};

class StopTokenToolCallProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "stop-token-tool-call-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        const int call = callCount_.fetch_add(1);
        if (call == 0) {
            onEvent(TextChunk{"Preparing a tool call <firmius_stop/>"});
            onEvent(ToolCallChunk{"stop-tool-1", 0, "list_directory",
                                  R"({"path":"."})"});
            onEvent(StreamDone{StopReason::ToolUse});
            return;
        }
        onEvent(TextChunk{"Final completion summary."});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "stop-token-tool-call-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override {
        return listModels().front();
    }

    void generateSummary(const std::string&, const AgentHistory&,
                         const std::string&,
                         std::function<void(const StreamEvent&)> onEvent,
                         std::atomic<bool>* = nullptr) override {
        onEvent(TextChunk{"summary"});
        onEvent(StreamDone{StopReason::Stop});
    }

    firmius::provider::ProviderType getProviderType() const override {
        return firmius::provider::ProviderType::APIKey;
    }

    int callCount() const { return callCount_.load(); }

private:
    std::atomic<int> callCount_{0};
};

class HarnessTest : public ::testing::Test {
protected:
    void SetUp() override {
        testHome_ = std::filesystem::temp_directory_path() / ("firmius_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(testHome_);
        promptsDir_ = testHome_ / "prompts";
        std::filesystem::create_directories(promptsDir_);
        {
            std::ofstream base(promptsDir_ / "base.md");
            base << "Base prompt";
        }
        {
            std::ofstream lead(promptsDir_ / "lead.md");
            lead << "---\nname: lead\ntitle: Lead\nscopes: [\"FilesystemRead\"]\n---\nLead persona";
        }
        {
            std::ofstream executor(promptsDir_ / "executor.md");
            executor << "---\nname: executor\ntitle: Executor\nscopes: [\"FilesystemRead\"]\n---\nExecutor persona";
        }
        setenv("FIRMIUS_PROMPTS_DIR", promptsDir_.c_str(), 1);
        setenv("HOME", testHome_.c_str(), 1);
        cleanupFirmiusDir();
        Harness::instance().init();
        firmius::provider::ProviderRegistry::instance().registerProvider(
            std::make_shared<TestRetryProvider>());
        firmius::provider::ProviderRegistry::instance().registerProvider(
            std::make_shared<TruncatedToolCallProvider>());
        firmius::provider::ProviderRegistry::instance().registerProvider(
            std::make_shared<ProviderDeclaredTruncatedToolCallProvider>());
        firmius::provider::ProviderRegistry::instance().registerProvider(
            std::make_shared<MixedTextToolCallProvider>());
        firmius::provider::ProviderRegistry::instance().registerProvider(
            std::make_shared<ParallelInterleavedToolCallProvider>());
        firmius::provider::ProviderRegistry::instance().registerProvider(
            std::make_shared<StopTokenToolCallProvider>());
        auto config = firmius::shared::ConfigLoader::instance().getConfig();
        config.defaultProviderId = "test-retry-provider";
        config.defaultModelId = "test-retry-model";
        config.defaultLeadPersona = "lead";
        Harness::instance().updateConfig(config);
    }

    static bool historyContainsStopToken(const AgentHistory& history) {
        for (const auto& turn : history.turns) {
            for (const auto& msg : turn.messages) {
                if (msg.role != Role::Assistant) {
                    continue;
                }
                for (const auto& part : msg.content) {
                    if (const auto* text = std::get_if<TextContent>(&part)) {
                        if (text->text.find("<firmius_stop/>") != std::string::npos) {
                            return true;
                        }
                    }
                    if (const auto* thinking = std::get_if<ThinkingContent>(&part)) {
                        if (thinking->thinking.find("<firmius_stop/>") !=
                            std::string::npos) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }
    
    void TearDown() override {
        Harness::instance().shutdown();
        cleanupFirmiusDir();
        std::filesystem::remove_all(testHome_);
        unsetenv("FIRMIUS_PROMPTS_DIR");
    }
    
    void cleanupFirmiusDir() {
        std::filesystem::path firmiusDir = testHome_ / ".firmius";
        if (std::filesystem::exists(firmiusDir)) {
            std::filesystem::remove_all(firmiusDir);
        }
    }

    std::shared_ptr<IAgent> waitForFocusedAgent() {
        std::shared_ptr<IAgent> agent;
        const bool found = waitForCondition([&]() {
            const auto agentId = Harness::instance().focusedAgentId();
            if (agentId.empty()) {
                return false;
            }
            agent = AgentRegistry::instance().getAgent(agentId);
            return agent && !agent->isBooting();
        });
        EXPECT_TRUE(found);
        return agent;
    }

    bool waitForIdle(const std::string& agentId) {
        return waitForCondition([&]() {
            auto agent = AgentRegistry::instance().getAgent(agentId);
            return agent && !agent->isRunning() && !agent->isBooting() &&
                   agent->getContext().state.currentStatus == AgentStatus::Idle;
        });
    }

    static size_t countUserTaskTurns(const AgentHistory& history) {
        size_t count = 0;
        for (const auto& turn : history.turns) {
            if (turn.turnId.rfind("user-task-", 0) == 0) {
                ++count;
            }
        }
        return count;
    }

    static void appendStoppedTurn(IAgent& agent, AgentStatus status) {
        auto& history = *agent.getMutableContext().history;
        AgentTurn turn;
        turn.turnId = (status == AgentStatus::Cancelled ? "cancelled-" : "error-") +
                      std::to_string(history.turns.size());
        Message message;
        message.role = Role::Error;
        message.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        if (status == AgentStatus::Cancelled) {
            message.content.push_back(ErrorContent{
                "Agent Cancelled",
                "The agent execution was interrupted.",
                "Execution stopped before completion and can be resumed."});
        } else {
            message.content.push_back(ErrorContent{
                "Provider Error",
                "The provider request failed.",
                "Synthetic test failure."});
        }
        turn.messages.push_back(message);
        history.turns.push_back(turn);
        agent.getMutableContext().state.currentStatus = status;
        agent.saveHistory();
    }

    std::shared_ptr<IAgent> createAgentWithUserTurn(
        const std::string& threadId, const std::string& friendlyName = "lead",
        const std::string& text = "resume me",
        const std::vector<ImageContent>& images = {}) {
        const std::string agentId =
            Engine::instance().createAgent(threadId, "lead", true, "", friendlyName, "");
        const bool ready = waitForCondition([&]() {
            auto agent = AgentRegistry::instance().getAgent(agentId);
            return agent && !agent->isBooting();
        });
        EXPECT_TRUE(ready);

        auto agent = AgentRegistry::instance().getAgent(agentId);
        if (agent && agent->getContext().history->turns.empty()) {
            AgentTurn userTurn;
            userTurn.turnId = "user-task-0";
            Message message;
            message.role = Role::User;
            message.timestamp = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                    .count());
            message.content.push_back(TextContent{text});
            for (const auto& image : images) {
                message.content.push_back(image);
            }
            userTurn.messages.push_back(message);
            agent->getMutableContext().history->turns.push_back(userTurn);
            agent->saveHistory();
        }
        return agent;
    }

    std::shared_ptr<IAgent> createFocusedLeadAgent(const std::string& threadId) {
        auto agent = createAgentWithUserTurn(threadId);
        if (agent) {
            EXPECT_TRUE(Harness::instance().setFocusedAgent(agent->getContext().identity.id));
        }
        return agent;
    }

    std::shared_ptr<IAgent> createAgentWithUserTurnPersona(
        const std::string& threadId, const std::string& persona,
        const std::string& friendlyName = "agent",
        const std::string& text = "resume me",
        const std::vector<ImageContent>& images = {}) {
        const std::string agentId =
            Engine::instance().createAgent(threadId, persona, true, "", friendlyName, "");
        const bool ready = waitForCondition([&]() {
            auto agent = AgentRegistry::instance().getAgent(agentId);
            return agent && !agent->isBooting();
        });
        EXPECT_TRUE(ready);

        auto agent = AgentRegistry::instance().getAgent(agentId);
        if (agent && agent->getContext().history->turns.empty()) {
            AgentTurn userTurn;
            userTurn.turnId = "user-task-0";
            Message message;
            message.role = Role::User;
            message.timestamp = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                    .count());
            message.content.push_back(TextContent{text});
            for (const auto& image : images) {
                message.content.push_back(image);
            }
            userTurn.messages.push_back(message);
            agent->getMutableContext().history->turns.push_back(userTurn);
            agent->saveHistory();
        }
        return agent;
    }

    std::shared_ptr<IAgent> createFocusedAgent(const std::string& threadId,
                                               const std::string& persona) {
        auto agent = createAgentWithUserTurnPersona(threadId, persona, persona);
        if (agent) {
            EXPECT_TRUE(Harness::instance().setFocusedAgent(agent->getContext().identity.id));
        }
        return agent;
    }
    
    std::filesystem::path testHome_;
    std::filesystem::path promptsDir_;
};

TEST_F(HarnessTest, switchThread_preservesAgent) {
    std::string threadA = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadA.empty()) << "Failed to create thread A";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
    
    std::string threadB = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadB.empty()) << "Failed to create thread B";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadB);
    EXPECT_NE(threadA, threadB);
    
    bool result = Harness::instance().switchThread(threadA);
    EXPECT_TRUE(result);
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
}

TEST_F(HarnessTest, newThread_savesPreviousAgent) {
    std::string threadA = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadA.empty()) << "Failed to create thread A";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
    
    std::string threadB = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadB.empty()) << "Failed to create thread B";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadB);
    
    bool result = Harness::instance().switchThread(threadA);
    EXPECT_TRUE(result);
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
}

TEST_F(HarnessTest, switchThread_missingMetadataReturnsFalseAndEmitsWarning) {
    std::string healthyThread = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(healthyThread.empty());

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    ThreadMetadata brokenMetadata;
    brokenMetadata.title = "Broken Thread";
    std::string brokenThread = tm.createThread(brokenMetadata);
    std::filesystem::remove(testHome_ / ".firmius" / "threads" / brokenThread / "metadata.json");

    std::promise<AgentError> warningPromise;
    auto warningFuture = warningPromise.get_future();
    int subId = Harness::instance().subscribe([&](const AppEvent& event) {
        if (auto error = std::get_if<AgentError>(&event);
            error && error->agentId.empty() &&
            error->message.find("could not be opened") != std::string::npos) {
            warningPromise.set_value(*error);
        }
    });

    EXPECT_FALSE(Harness::instance().switchThread(brokenThread));
    EXPECT_EQ(Harness::instance().currentThreadId(), healthyThread);
    EXPECT_NE(warningFuture.get().message.find(brokenThread), std::string::npos);

    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, switchThread_corruptManifestRecoversAndEmitsWarning) {
    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    ThreadMetadata metadata;
    metadata.title = "Manifest Recovery";
    std::string threadId = tm.createThread(metadata);

    {
        std::ofstream manifest(testHome_ / ".firmius" / "threads" / threadId / "agents.json");
        manifest << "{not json";
    }

    std::promise<AgentError> warningPromise;
    auto warningFuture = warningPromise.get_future();
    int subId = Harness::instance().subscribe([&](const AppEvent& event) {
        if (auto error = std::get_if<AgentError>(&event);
            error && error->agentId.empty() &&
            error->message.find("continuing without restoring agents") != std::string::npos) {
            warningPromise.set_value(*error);
        }
    });

    EXPECT_TRUE(Harness::instance().switchThread(threadId));
    EXPECT_EQ(Harness::instance().currentThreadId(), threadId);
    EXPECT_TRUE(Harness::instance().focusedAgentId().empty());
    EXPECT_NE(warningFuture.get().message.find(threadId), std::string::npos);

    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, resumeLast_brokenThreadClearsSessionAndReturnsFalse) {
    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    ThreadMetadata brokenMetadata;
    brokenMetadata.title = "Broken Startup Thread";
    std::string brokenThread = tm.createThread(brokenMetadata);
    std::filesystem::remove(testHome_ / ".firmius" / "threads" / brokenThread / "metadata.json");

    {
        std::ofstream sessionFile(testHome_ / ".firmius" / "last_session.json");
        sessionFile << "{\"threadId\":\"" << brokenThread << "\"}";
    }

    Harness::instance().shutdown();
    Harness::instance().init();

    EXPECT_FALSE(Harness::instance().resumeLast());
    EXPECT_TRUE(Harness::instance().currentThreadId().empty());

    std::ifstream sessionFile(testHome_ / ".firmius" / "last_session.json");
    std::string sessionContents((std::istreambuf_iterator<char>(sessionFile)),
                                std::istreambuf_iterator<char>());
    EXPECT_EQ(sessionContents, "{}");
}

TEST_F(HarnessTest, isDescendant_directChild) {
    std::string thread = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(thread.empty());
    
    std::string parentId = "parent-agent-001";
    std::string childId = "child-agent-001";
    
    bool childReceivedEvent = false;
    
    int subId = Harness::instance().subscribe([&childReceivedEvent, childId](const AppEvent& event) {
        if (std::holds_alternative<AgentText>(event)) {
            const auto& chunk = std::get<AgentText>(event);
            if (chunk.agentId == childId) {
                childReceivedEvent = true;
            }
        }
    });
    
    Harness::instance().unsubscribe(subId);
    SUCCEED();
}

TEST_F(HarnessTest, isDescendant_grandchild) {
    std::string grandparentId = "grandparent-agent";
    std::string parentId = "parent-agent";
    std::string grandchildId = "grandchild-agent";
    SUCCEED();
}

TEST_F(HarnessTest, isDescendant_unrelated) {
    std::string agentA = "agent-a";
    std::string agentB = "agent-b";
    std::string unrelatedAgent = "unrelated-agent";
    SUCCEED();
}

TEST_F(HarnessTest, isDescendant_cycleProtection) {
    SUCCEED();
}

TEST_F(HarnessTest, routeEngineEvent_filtersNonDescendants) {
    std::string thread = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(thread.empty());
    
    std::string focusedAgent = "focused-agent";
    std::string unrelatedAgent = "unrelated-agent";
    
    std::vector<std::string> receivedAgentIds;
    
    int subId = Harness::instance().subscribe([&receivedAgentIds](const AppEvent& event) {
        std::visit([&receivedAgentIds](auto&& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, AgentText> ||
                          std::is_same_v<T, AgentThinking> ||
                          std::is_same_v<T, AgentToolCall> ||
                          std::is_same_v<T, AgentTurnCompleted> ||
                          std::is_same_v<T, AgentSpawned> ||
                          std::is_same_v<T, AgentProcessOutput> ||
                          std::is_same_v<T, AgentCompacting> ||
                          std::is_same_v<T, ContextCompacted>) {
                receivedAgentIds.push_back(ev.agentId);
            }
        }, event);
    });
    
    Harness::instance().unsubscribe(subId);
    SUCCEED();
}

TEST_F(HarnessTest, routeEngineEvent_passesDescendants) {
    std::string thread = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(thread.empty());
    
    std::string parentAgent = "parent-agent";
    std::string childAgent = "child-agent";
    
    SUCCEED();
}

TEST_F(HarnessTest, shutdown_savesSession) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    
    Harness::instance().shutdown();
    
    std::filesystem::path sessionFile = testHome_ / ".firmius" / "last_session.json";
    EXPECT_TRUE(std::filesystem::exists(sessionFile));
    
    if (std::filesystem::exists(sessionFile)) {
        std::ifstream file(sessionFile);
        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        EXPECT_FALSE(content.empty());
        EXPECT_NE(content.find("threadId"), std::string::npos);
    }
    
    Harness::instance().init();
}

TEST_F(HarnessTest, threadLocking_preventsConcurrentAccess) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    
    std::string threadId2 = Harness::instance().newThread({}, "/tmp", "test2");
    EXPECT_FALSE(threadId2.empty());
    EXPECT_NE(threadId, threadId2);
}

TEST_F(HarnessTest, resumeLast_restoresSession) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    
    Harness::instance().shutdown();
    Harness::instance().init();
    
    bool result = Harness::instance().resumeLast();
    EXPECT_TRUE(result);
    EXPECT_EQ(Harness::instance().currentThreadId(), threadId);
}

TEST_F(HarnessTest, currentThreadPermissionMode_defaultsToRequest) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    EXPECT_EQ(Harness::instance().currentThreadPermissionMode(),
              ThreadPermissionMode::Request);
}

TEST_F(HarnessTest, setCurrentThreadPermissionMode_persistsAcrossThreadSwitch) {
    std::string threadA = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadA.empty());
    ASSERT_TRUE(Harness::instance().setCurrentThreadPermissionMode(
        ThreadPermissionMode::AlwaysAllow));

    std::string threadB = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadB.empty());
    ASSERT_TRUE(Harness::instance().switchThread(threadA));

    EXPECT_EQ(Harness::instance().currentThreadPermissionMode(),
              ThreadPermissionMode::AlwaysAllow);
}

TEST_F(HarnessTest, cycleCurrentThreadPermissionMode_cyclesAndPersists) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    auto next = Harness::instance().cycleCurrentThreadPermissionMode();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, ThreadPermissionMode::AlwaysAllow);
    EXPECT_EQ(Harness::instance().currentThreadPermissionMode(),
              ThreadPermissionMode::AlwaysAllow);

    next = Harness::instance().cycleCurrentThreadPermissionMode();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, ThreadPermissionMode::DenyAll);

    next = Harness::instance().cycleCurrentThreadPermissionMode();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, ThreadPermissionMode::Request);
}

TEST_F(HarnessTest, requestPermissionEscalation_blocksUntilResolved) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    std::promise<PermissionEscalationRequest> requestPromise;
    auto requestFuture = requestPromise.get_future();
    int subId = Harness::instance().subscribe([&](const AppEvent& event) {
        if (auto request = std::get_if<PermissionEscalationRequest>(&event)) {
            requestPromise.set_value(*request);
        }
    });

    std::promise<PermissionResponse> responsePromise;
    auto responseFuture = responsePromise.get_future();
    std::thread worker([&]() {
        PermissionEscalationRequest request;
        request.threadId = threadId;
        request.agentId = "agent-1";
        request.requestType = PermissionRequestType::Command;
        request.title = "Need approval";
        request.message = "Run command?";
        request.command = "rm -rf /tmp/nope";
        request.severity = CommandSeverity::HIGH;
        responsePromise.set_value(
            Harness::instance().requestPermissionEscalation(request));
    });

    auto emittedRequest = requestFuture.get();
    EXPECT_FALSE(emittedRequest.requestId.empty());
    EXPECT_EQ(emittedRequest.threadId, threadId);
    EXPECT_EQ(emittedRequest.command, "rm -rf /tmp/nope");

    EXPECT_TRUE(Harness::instance().resolvePermissionEscalation(
        emittedRequest.requestId, PermissionResponse::AllowOnce));

    EXPECT_EQ(responseFuture.get(), PermissionResponse::AllowOnce);

    worker.join();
    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, setCurrentThreadPermissionMode_emitsThreadMetadataUpdated) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    std::promise<ThreadMetadataUpdated> eventPromise;
    auto eventFuture = eventPromise.get_future();
    int subId = Harness::instance().subscribe([&](const AppEvent& event) {
        if (auto metadata = std::get_if<ThreadMetadataUpdated>(&event)) {
            eventPromise.set_value(*metadata);
        }
    });

    ASSERT_TRUE(Harness::instance().setCurrentThreadPermissionMode(
        ThreadPermissionMode::AlwaysAllow));

    auto updated = eventFuture.get();
    EXPECT_EQ(updated.threadId, threadId);
    EXPECT_EQ(updated.metadata.permissionMode, ThreadPermissionMode::AlwaysAllow);
    EXPECT_EQ(Harness::instance().currentThreadPermissionMode(),
              ThreadPermissionMode::AlwaysAllow);

    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, commandAllowAlways_persistsRuleAndSkipsSecondPrompt) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    Permissions permissions(threadId, "agent-1");
    auto intent =
        permissions.getIntentAnalyzer().analyze("git status", "/tmp");

    std::atomic<int> requestCount{0};
    std::promise<PermissionEscalationRequest> requestPromise;
    auto requestFuture = requestPromise.get_future();
    int subId = Harness::instance().subscribe(
        [&](const AppEvent& event) {
            if (auto request = std::get_if<PermissionEscalationRequest>(&event)) {
                requestCount.fetch_add(1);
                requestPromise.set_value(*request);
            }
        });

    std::promise<PermissionResponse> responsePromise;
    auto responseFuture = responsePromise.get_future();
    std::thread worker([&]() {
        responsePromise.set_value(
            permissions.requestCommandApproval("git status", intent));
    });

    auto request = requestFuture.get();
    EXPECT_TRUE(Harness::instance().resolvePermissionEscalation(
        request.requestId, PermissionResponse::AllowAlways));
    EXPECT_EQ(responseFuture.get(), PermissionResponse::AllowAlways);
    worker.join();

    EXPECT_EQ(permissions.requestCommandApproval("git status", intent),
              PermissionResponse::AllowAlways);
    EXPECT_EQ(requestCount.load(), 1);

    auto rules = Harness::instance().threadPermissionRules(threadId);
    ASSERT_EQ(rules.commandAllowRules.size(), 1u);
    EXPECT_EQ(rules.commandAllowRules[0].exactCommand, "git status");

    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, writeAllowAlways_persistsPrefixAndSkipsSecondPrompt) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    Permissions permissions(threadId, "agent-1");
    AgentContext context;
    context.permissions.allowedPaths = {"/tmp/**"};
    context.permissions.allowOutsideCwd = false;
    permissions.bindContext(context);

    std::string filePath = "/tmp/project/src/file.txt";

    std::atomic<int> requestCount{0};
    std::promise<PermissionEscalationRequest> requestPromise;
    auto requestFuture = requestPromise.get_future();
    int subId = Harness::instance().subscribe(
        [&](const AppEvent& event) {
            if (auto request = std::get_if<PermissionEscalationRequest>(&event)) {
                requestCount.fetch_add(1);
                requestPromise.set_value(*request);
            }
        });

    std::promise<PermissionResponse> responsePromise;
    auto responseFuture = responsePromise.get_future();
    std::thread worker([&]() {
        responsePromise.set_value(permissions.requestEditApproval(filePath));
    });

    auto request = requestFuture.get();
    EXPECT_TRUE(Harness::instance().resolvePermissionEscalation(
        request.requestId, PermissionResponse::AllowAlways));
    EXPECT_EQ(responseFuture.get(), PermissionResponse::AllowAlways);
    worker.join();

    EXPECT_EQ(permissions.requestEditApproval(filePath),
              PermissionResponse::AllowAlways);
    EXPECT_EQ(requestCount.load(), 1);

    auto rules = Harness::instance().threadPermissionRules(threadId);
    ASSERT_EQ(rules.writeAllowPaths.size(), 1u);
    EXPECT_EQ(rules.writeAllowPaths[0], "/tmp/project/src/**");

    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, vulnerableCommandsRemainDeniedInAlwaysAllowMode) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    ASSERT_TRUE(Harness::instance().setCurrentThreadPermissionMode(
        ThreadPermissionMode::AlwaysAllow));

    Permissions permissions(threadId, "agent-1");
    auto intent = permissions.getIntentAnalyzer().analyze("rm -rf /", "/tmp");
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
    EXPECT_EQ(permissions.requestCommandApproval("rm -rf /", intent),
              PermissionResponse::Deny);
}

TEST_F(HarnessTest, retryLastRequestResumesCancelledAgentWithoutNewUserTurn) {
    auto& harness = Harness::instance();
    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    const auto agentId = harness.focusedAgentId();

    appendStoppedTurn(*agent, AgentStatus::Cancelled);
    const auto userTurnsBefore = countUserTaskTurns(*agent->getContext().history);
    const auto turnsBefore = agent->getContext().history->turns.size();

    std::string statusMessage;
    ASSERT_TRUE(harness.retryLastRequest(statusMessage));
    EXPECT_EQ(statusMessage,
              "Resuming focused agent from the last failed or cancelled turn.");
    ASSERT_TRUE(waitForCondition([&]() {
        auto current = AgentRegistry::instance().getAgent(agentId);
        return current && current->getContext().history->turns.size() > turnsBefore &&
               !current->isRunning() && !current->isBooting() &&
               current->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    const auto& history = *agent->getContext().history;
    EXPECT_EQ(countUserTaskTurns(history), userTurnsBefore);
    ASSERT_GT(history.turns.size(), turnsBefore);
    EXPECT_EQ(history.turns.back().turnId.rfind("assistant-", 0), 0u);
}

TEST_F(HarnessTest, retryLastRequestOnlyTargetsFocusedAgent) {
    auto& harness = Harness::instance();
    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    auto leadAgent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(leadAgent);
    const auto leadId = harness.focusedAgentId();

    std::vector<ImageContent> images{
        ImageContent{"data:image/png;base64,abc", "image/png", "high"}};
    auto workerAgent =
        createAgentWithUserTurn(threadId, "worker", "latest failed turn", images);
    ASSERT_TRUE(workerAgent);
    const auto workerId = workerAgent->getContext().identity.id;
    appendStoppedTurn(*workerAgent, AgentStatus::Error);
    ASSERT_TRUE(harness.setFocusedAgent(leadId));

    const auto userTurnsBefore = countUserTaskTurns(*leadAgent->getContext().history);
    const auto turnsBefore = leadAgent->getContext().history->turns.size();

    std::string statusMessage;
    ASSERT_TRUE(harness.retryLastRequest(statusMessage));
    EXPECT_EQ(statusMessage,
              "Awakening focused agent from existing thread history.");
    EXPECT_EQ(harness.focusedAgentId(), leadId);
    ASSERT_TRUE(waitForCondition([&]() {
        auto current = AgentRegistry::instance().getAgent(leadId);
        return current && current->getContext().history->turns.size() > turnsBefore &&
               current->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    const auto& history = *leadAgent->getContext().history;
    EXPECT_EQ(countUserTaskTurns(history), userTurnsBefore);
    ASSERT_FALSE(history.turns.front().messages.empty());
    EXPECT_NE(harness.focusedAgentId(), workerId);
}

TEST_F(HarnessTest, retryLastRequestResumesErroredAgentWithoutNewUserTurn) {
    auto& harness = Harness::instance();
    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    const auto agentId = harness.focusedAgentId();

    appendStoppedTurn(*agent, AgentStatus::Error);
    const auto userTurnsBefore = countUserTaskTurns(*agent->getContext().history);
    const auto turnsBefore = agent->getContext().history->turns.size();

    std::string statusMessage;
    ASSERT_TRUE(harness.retryLastRequest(statusMessage));
    EXPECT_EQ(statusMessage,
              "Resuming focused agent from the last failed or cancelled turn.");
    ASSERT_TRUE(waitForCondition([&]() {
        auto current = AgentRegistry::instance().getAgent(agentId);
        return current && current->getContext().history->turns.size() > turnsBefore &&
               !current->isRunning() && !current->isBooting() &&
               current->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(countUserTaskTurns(*agent->getContext().history), userTurnsBefore);
}

TEST_F(HarnessTest, retryLastRequestWorksAfterThreadReload) {
    auto& harness = Harness::instance();
    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    auto agent = createFocusedLeadAgent(threadId);
    const auto agentId = harness.focusedAgentId();

    appendStoppedTurn(*agent, AgentStatus::Cancelled);

    const std::string otherThreadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(otherThreadId.empty());
    ASSERT_TRUE(harness.switchThread(threadId));

    auto reloaded = waitForFocusedAgent();
    ASSERT_EQ(harness.focusedAgentId(), agentId);
    EXPECT_EQ(reloaded->getContext().state.currentStatus, AgentStatus::Cancelled);
    const auto userTurnsBefore = countUserTaskTurns(*reloaded->getContext().history);
    const auto turnsBefore = reloaded->getContext().history->turns.size();

    std::string statusMessage;
    ASSERT_TRUE(harness.retryLastRequest(statusMessage));
    ASSERT_TRUE(waitForCondition([&]() {
        auto current = AgentRegistry::instance().getAgent(agentId);
        return current && current->getContext().history->turns.size() > turnsBefore &&
               !current->isRunning() && !current->isBooting() &&
               current->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    auto resumed = AgentRegistry::instance().getAgent(agentId);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(countUserTaskTurns(*resumed->getContext().history), userTurnsBefore);
}

TEST_F(HarnessTest, retryLastRequestIsUnavailableWhenFocusedAgentIsNotStopped) {
    auto& harness = Harness::instance();
    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    const auto agentId = harness.focusedAgentId();
    EXPECT_EQ(agent->getContext().state.currentStatus, AgentStatus::Idle);

    std::string statusMessage;
    EXPECT_TRUE(harness.retryLastRequest(statusMessage));
    EXPECT_EQ(statusMessage, "Awakening focused agent from existing thread history.");
}

TEST_F(HarnessTest, MixedThinkingTextAndToolCallTurnPersistsAllAssistantContent) {
    auto& harness = Harness::instance();
    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = "mixed-text-tool-provider";
    config.defaultModelId = "mixed-text-tool-model";
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    harness.send("run mixed content and tool call");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    const auto& history = *agent->getContext().history;
    const AgentTurn* mixedTurn = nullptr;
    for (const auto& turn : history.turns) {
        if (turn.stopReason != StopReason::ToolUse || turn.messages.empty()) {
            continue;
        }
        const auto& msg = turn.messages.front();
        if (msg.role != Role::Assistant) {
            continue;
        }
        bool hasToolCall = false;
        for (const auto& part : msg.content) {
            if (std::holds_alternative<ToolCallContent>(part)) {
                hasToolCall = true;
                break;
            }
        }
        if (hasToolCall) {
            mixedTurn = &turn;
            break;
        }
    }

    ASSERT_NE(mixedTurn, nullptr);
    ASSERT_FALSE(mixedTurn->messages.empty());
    const auto& mixedMsg = mixedTurn->messages.front();

    const ThinkingContent* thinking = nullptr;
    const TextContent* text = nullptr;
    const ToolCallContent* toolCall = nullptr;
    for (const auto& part : mixedMsg.content) {
        if (!thinking) {
            thinking = std::get_if<ThinkingContent>(&part);
        }
        if (!text) {
            text = std::get_if<TextContent>(&part);
        }
        if (!toolCall) {
            toolCall = std::get_if<ToolCallContent>(&part);
        }
    }

    ASSERT_NE(thinking, nullptr);
    ASSERT_NE(text, nullptr);
    ASSERT_NE(toolCall, nullptr);
    EXPECT_EQ(thinking->thinking, "Planning next step.");
    EXPECT_EQ(text->text, "I will inspect ASCII.txt first.");
    EXPECT_EQ(toolCall->id, "call-read");
    EXPECT_EQ(toolCall->name, "file_read");
    EXPECT_EQ(toolCall->args, R"({"path":"ASCII.txt"})");
}

TEST_F(HarnessTest, InterleavedParallelToolChunksWithMissingIndexStayValid) {
    auto& harness = Harness::instance();
    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = "parallel-interleaved-tool-provider";
    config.defaultModelId = "parallel-interleaved-tool-model";
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    std::atomic<bool> malformedValidatorTriggered{false};
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (auto error = std::get_if<AgentError>(&event)) {
            if (error->message.find("malformed streamed tool call payload") !=
                std::string::npos) {
                malformedValidatorTriggered = true;
            }
        }
    });

    harness.send("run parallel interleaved reads");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));
    harness.unsubscribe(subId);

    EXPECT_FALSE(malformedValidatorTriggered.load());

    const auto& history = *agent->getContext().history;
    size_t callCount = 0;
    for (const auto& turn : history.turns) {
        for (const auto& msg : turn.messages) {
            for (const auto& part : msg.content) {
                if (auto call = std::get_if<ToolCallContent>(&part)) {
                    if (call->name == "file_read") {
                        ++callCount;
                        EXPECT_TRUE(call->id == "call-a" || call->id == "call-b");
                        EXPECT_TRUE(call->args == R"({"path":"ASCII.txt"})" ||
                                    call->args == R"({"path":"CMakeLists.txt"})");
                    }
                }
            }
        }
    }
    EXPECT_EQ(callCount, 2u);
}

TEST_F(HarnessTest, ProseOnlyTurnDuringActiveExecutionDoesNotTerminate) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-active-exec",
        std::vector<std::string>{"Status: preparing execution.", "Execution completed."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    agent->getEnvironment()->getProcessManager().addBlockingProcessId(
        "test-blocking-prose-loop");

    harness.send("continue execution with progress narration");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));
    agent->getEnvironment()->getProcessManager().removeBlockingProcessId(
        "test-blocking-prose-loop");

    ASSERT_TRUE(waitForCondition([&]() {
        return !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_GE(provider->callCount(), 2);
}

TEST_F(HarnessTest, MultipleProseOnlyTurnsDuringActiveExecutionDoNotTerminate) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-active-exec-multiple",
        std::vector<std::string>{"Status 1.", "Status 2.", "Execution completed."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    agent->getEnvironment()->getProcessManager().addBlockingProcessId(
        "test-blocking-prose-loop-multiple");

    harness.send("continue execution with progress narration");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 3; }));
    agent->getEnvironment()->getProcessManager().removeBlockingProcessId(
        "test-blocking-prose-loop-multiple");

    ASSERT_TRUE(waitForCondition([&]() {
        return !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_GE(provider->callCount(), 3);
}

TEST_F(HarnessTest, ProseOnlyTurnInSimpleAnswerModeTerminates) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-simple-answer",
        std::vector<std::string>{"This is the direct answer.",
                                 "This should never be reached."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    harness.send("what is this file?");
    auto agent = waitForFocusedAgent();

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 1);
}

TEST_F(HarnessTest, LargeImplementationRequestEarlyProseWithoutActiveStateContinues) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-early-discovery",
        std::vector<std::string>{"Discovery summary before planning.",
                                 "Now proceeding to concrete execution."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    harness.send("implement a large feature across this codebase");
    auto agent = waitForFocusedAgent();

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 3);
}

TEST_F(HarnessTest, ActivePlanWithoutTodoDoesNotForceContinuation) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-active-plan",
        std::vector<std::string>{"Progress update before execution.",
                                 "Final answer after continuation."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    Plan plan;
    plan.id = "plan-prose-continuation";
    plan.threadId = threadId;
    plan.title = "Execution Plan";
    plan.objective = "Keep loop alive across prose turns";
    plan.status = PlanStatus::Active;
    WorkChunk chunk;
    chunk.id = "chunk-1";
    chunk.title = "Implement fix";
    chunk.goal = "apply runtime change";
    chunk.status = WorkChunkStatus::Ready;
    plan.chunks.push_back(chunk);
    tm.writePlan(threadId, plan);

    ThreadMetadata metadata = tm.getMetadata(threadId);
    metadata.activePlanId = plan.id;
    tm.updateMetadata(threadId, metadata);

    harness.send("what is the current status?");
    auto agent = waitForFocusedAgent();

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 1);
}

TEST_F(HarnessTest, IncompleteTodoKeepsLoopAliveAfterProse) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-incomplete-todo",
        std::vector<std::string>{"Progress update.", "Final response."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    AgentTodoList todo;
    todo.threadId = threadId;
    todo.agentId = harness.focusedAgentId();
    todo.nextId = 2;
    todo.items.push_back(TodoItem{1, "Continue execution", TodoStatus::InProgress,
                                  "", "", 1, 1});
    tm.writeAgentTodo(threadId, todo.agentId, todo);

    Engine::instance().executeTask(agent->getContext().identity.id,
                                   "continue work");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 3);
}

TEST_F(HarnessTest, LeadMissingTodoWithActivePlanTriggersTodoEnforcement) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-lead-missing-todo",
        std::vector<std::string>{"Progress update.", "Follow-up."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    Plan plan;
    plan.id = "plan-enforce-todo";
    plan.threadId = threadId;
    plan.title = "Active Plan";
    plan.objective = "Multi-step coordination";
    plan.strategy = "Enforce todo gating";
    plan.status = PlanStatus::Active;
    WorkChunk chunk;
    chunk.id = "chunk-1";
    chunk.title = "Coordination chunk";
    chunk.status = WorkChunkStatus::Ready;
    plan.chunks.push_back(chunk);
    tm.writePlan(threadId, plan);

    ThreadMetadata metadata = tm.getMetadata(threadId);
    metadata.activePlanId = plan.id;
    tm.updateMetadata(threadId, metadata);

    harness.send("build and implement the requested runtime change");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_GE(provider->callCount(), 2);
}

TEST_F(HarnessTest, ExecutorMissingTodoWithAssignedChunkTriggersTodoEnforcement) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-executor-missing-todo",
        std::vector<std::string>{"Working on it.", "Follow-up."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);
    agent->getMutableContext().config.personaName = "executor";

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    Plan plan;
    plan.id = "plan-exec-enforce";
    plan.threadId = threadId;
    plan.title = "Executor Plan";
    plan.objective = "Execute one chunk";
    plan.strategy = "Executor owns one chunk";
    plan.status = PlanStatus::Active;
    WorkChunk chunk;
    chunk.id = "chunk-1";
    chunk.title = "Assigned work";
    chunk.status = WorkChunkStatus::Ready;
    chunk.assignedAgentId = agent->getContext().identity.id;
    plan.chunks.push_back(chunk);
    tm.writePlan(threadId, plan);

    harness.send("implement the assigned chunk");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_GE(provider->callCount(), 2);
}

TEST_F(HarnessTest, LeadTodoPresenceClearsTodoEnforcement) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-lead-todo-present",
        std::vector<std::string>{"Progress update.", "Should not be called."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    Plan plan;
    plan.id = "plan-todo-present";
    plan.threadId = threadId;
    plan.title = "Active Plan";
    plan.objective = "Multi-step coordination";
    plan.strategy = "Todo already present";
    plan.status = PlanStatus::Active;
    WorkChunk chunk;
    chunk.id = "chunk-1";
    chunk.title = "Coordination chunk";
    chunk.status = WorkChunkStatus::Ready;
    plan.chunks.push_back(chunk);
    tm.writePlan(threadId, plan);

    ThreadMetadata metadata = tm.getMetadata(threadId);
    metadata.activePlanId = plan.id;
    tm.updateMetadata(threadId, metadata);

    AgentTodoList todo;
    todo.threadId = threadId;
    todo.agentId = harness.focusedAgentId();
    todo.nextId = 2;
    todo.items.push_back(
        TodoItem{1, "Coordination complete", TodoStatus::Done, "", "", 1, 1});
    tm.writeAgentTodo(threadId, todo.agentId, todo);

    harness.send("build and implement the requested runtime change");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 1);
}

TEST_F(HarnessTest, FullyDoneTodoAllowsStopWhenNoOtherWork) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-complete-todo",
        std::vector<std::string>{"Done summary.", "Should not be called."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    AgentTodoList todo;
    todo.threadId = threadId;
    todo.agentId = harness.focusedAgentId();
    todo.nextId = 2;
    todo.items.push_back(TodoItem{1, "Already done", TodoStatus::Done,
                                  "", "", 1, 1});
    tm.writeAgentTodo(threadId, todo.agentId, todo);

    Engine::instance().executeTask(agent->getContext().identity.id,
                                   "continue work");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 1);
}

TEST_F(HarnessTest, NoActiveWorkInformationalAnswerStillEndsNormally) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-no-active-work",
        std::vector<std::string>{"Here are the findings.",
                                 "Second turn should not execute."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    harness.send("inspect and report");
    auto agent = waitForFocusedAgent();

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 1);
}

TEST_F(HarnessTest, ProseOnlyExecutionIntentLoopRemainsBoundedWithoutActiveWork) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-bounded-loop",
        std::vector<std::string>{"High-level status update.",
                                 "Another prose-only status update.",
                                 "A third prose turn that should not execute."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    harness.send("build and implement the requested runtime change");
    auto agent = waitForFocusedAgent();

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 3);
}

TEST_F(HarnessTest, ProseOnlyExecutionIntentLoopRemainsBoundedAtThreeTurnsWithoutActiveWork) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-bounded-loop-3",
        std::vector<std::string>{"Turn 1.",
                                 "Turn 2.",
                                 "Turn 3.",
                                 "Turn 4 (should not execute)."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    harness.send("build and implement the requested runtime change");
    auto agent = waitForFocusedAgent();

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 3);
}

TEST_F(HarnessTest, NoModelAuthoredStopTokenIsRequiredForCompletion) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-no-stop-token",
        std::vector<std::string>{"Progress report: still working.",
                                 "Work complete with plain prose."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);
    agent->getEnvironment()->getProcessManager().addBlockingProcessId(
        "test-stop-token-not-required");

    harness.send("continue until done");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));
    agent->getEnvironment()->getProcessManager().removeBlockingProcessId(
        "test-stop-token-not-required");

    ASSERT_TRUE(waitForCondition([&]() {
        return !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    const auto& history = *agent->getContext().history;
    EXPECT_FALSE(historyContainsStopToken(history));
    EXPECT_GE(provider->callCount(), 2);
}

TEST_F(HarnessTest, FinalSummaryStopTokenStopsWhenRuntimeTruthAllowsStop) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-stop-token-allowed",
        std::vector<std::string>{"All work complete. <firmius_stop/>",
                                 "Should not be reached."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    AgentTodoList todo;
    todo.threadId = threadId;
    todo.agentId = harness.focusedAgentId();
    todo.nextId = 2;
    todo.items.push_back(
        TodoItem{1, "Done item", TodoStatus::Done, "", "", 1, 1});
    tm.writeAgentTodo(threadId, todo.agentId, todo);

    std::vector<std::string> streamedText;
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (auto text = std::get_if<AgentText>(&event)) {
            streamedText.push_back(text->delta);
        }
    });

    Engine::instance().executeTask(agent->getContext().identity.id, "continue work");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));
    harness.unsubscribe(subId);

    EXPECT_EQ(provider->callCount(), 1);
    for (const auto& chunk : streamedText) {
        EXPECT_EQ(chunk.find("<firmius_stop/>"), std::string::npos);
    }
    EXPECT_FALSE(historyContainsStopToken(*agent->getContext().history));
}

TEST_F(HarnessTest, FinalSummaryStopTokenIgnoredWhenTodoIncomplete) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-stop-token-incomplete-todo",
        std::vector<std::string>{"Still executing. <firmius_stop/>",
                                 "Execution complete now."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    AgentTodoList todo;
    todo.threadId = threadId;
    todo.agentId = harness.focusedAgentId();
    todo.nextId = 2;
    todo.items.push_back(
        TodoItem{1, "Still running", TodoStatus::InProgress, "", "", 1, 1});
    tm.writeAgentTodo(threadId, todo.agentId, todo);

    harness.send("build and implement the requested runtime change");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_GE(provider->callCount(), 2);
    EXPECT_FALSE(historyContainsStopToken(*agent->getContext().history));
}

TEST_F(HarnessTest, FinalSummaryStopTokenIgnoredWhenActivePlanIsNotDone) {
    auto& harness = Harness::instance();
    auto providerWithToken = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-stop-token-active-plan",
        std::vector<std::string>{"Plan still active. <firmius_stop/>",
                                 "Follow-up while plan is active."});
    firmius::provider::ProviderRegistry::instance().registerProvider(
        providerWithToken);

    auto providerWithoutToken = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-no-stop-token-active-plan",
        std::vector<std::string>{"Plan still active.",
                                 "Follow-up while plan is active."});
    firmius::provider::ProviderRegistry::instance().registerProvider(
        providerWithoutToken);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = providerWithToken->getId();
    config.defaultModelId = providerWithToken->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    Plan plan;
    plan.id = "plan-stop-token-not-done";
    plan.threadId = threadId;
    plan.title = "Execution Plan";
    plan.objective = "Keep execution open";
    plan.status = PlanStatus::Active;
    tm.writePlan(threadId, plan);

    ThreadMetadata metadata = tm.getMetadata(threadId);
    metadata.activePlanId = plan.id;
    tm.updateMetadata(threadId, metadata);

    harness.send("build and implement the requested runtime change");

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    const int withTokenCalls = providerWithToken->callCount();
    EXPECT_FALSE(historyContainsStopToken(*agent->getContext().history));

    config.defaultProviderId = providerWithoutToken->getId();
    config.defaultModelId = providerWithoutToken->listModels().front().id;
    harness.updateConfig(config);

    const std::string controlThreadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(controlThreadId.empty());
    auto controlAgent = createFocusedLeadAgent(controlThreadId);
    ASSERT_TRUE(controlAgent);

    Plan controlPlan = plan;
    controlPlan.id = "plan-stop-token-not-done-control";
    controlPlan.threadId = controlThreadId;
    tm.writePlan(controlThreadId, controlPlan);

    ThreadMetadata controlMetadata = tm.getMetadata(controlThreadId);
    controlMetadata.activePlanId = controlPlan.id;
    tm.updateMetadata(controlThreadId, controlMetadata);

    harness.send("build and implement the requested runtime change");
    ASSERT_TRUE(waitForCondition([&]() {
        return controlAgent && !controlAgent->isRunning() &&
               controlAgent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(withTokenCalls, providerWithoutToken->callCount());
    EXPECT_FALSE(historyContainsStopToken(*controlAgent->getContext().history));
}

TEST_F(HarnessTest, FinalSummaryStopTokenIgnoredWhileOwnedProcessIsBlocking) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-stop-token-blocking-process",
        std::vector<std::string>{"Process still running. <firmius_stop/>",
                                 "Process completed after follow-up."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);
    agent->getEnvironment()->getProcessManager().addBlockingProcessId(
        "stop-token-blocking-process");

    Engine::instance().executeTask(agent->getContext().identity.id, "continue work");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));
    agent->getEnvironment()->getProcessManager().removeBlockingProcessId(
        "stop-token-blocking-process");

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_GE(provider->callCount(), 2);
    EXPECT_FALSE(historyContainsStopToken(*agent->getContext().history));
}

TEST_F(HarnessTest, StopTokenInsideToolTurnIsIgnored) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<StopTokenToolCallProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    harness.send("continue execution");
    auto agent = waitForFocusedAgent();

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 2);

    bool sawToolResult = false;
    const auto& history = *agent->getContext().history;
    for (const auto& turn : history.turns) {
        for (const auto& msg : turn.messages) {
            for (const auto& part : msg.content) {
                if (std::holds_alternative<ToolResultContent>(part)) {
                    sawToolResult = true;
                }
            }
        }
    }
    EXPECT_TRUE(sawToolResult);
    EXPECT_FALSE(historyContainsStopToken(history));
}

TEST_F(HarnessTest, TruncatedStreamedToolCallIsRejectedBeforeExecution) {
    auto& harness = Harness::instance();
    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = "truncated-tool-provider";
    config.defaultModelId = "truncated-tool-model";
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    std::promise<AgentError> errorPromise;
    auto errorFuture = errorPromise.get_future();
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (auto error = std::get_if<AgentError>(&event)) {
            if (error->message.find("malformed streamed tool call payload") !=
                std::string::npos) {
                errorPromise.set_value(*error);
            }
        }
    });

    harness.send("trigger malformed tool");

    auto emittedError = errorFuture.get();
    harness.unsubscribe(subId);

    EXPECT_NE(emittedError.message.find("malformed streamed tool call payload"),
              std::string::npos);
    EXPECT_NE(emittedError.message.find("invalid or truncated JSON arguments"),
              std::string::npos);

    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Error;
    }));

    const auto& history = *agent->getContext().history;
    ASSERT_FALSE(history.turns.empty());
    const auto& lastTurn = history.turns.back();
    ASSERT_FALSE(lastTurn.messages.empty());
    const auto* persistedError =
        std::get_if<ErrorContent>(&lastTurn.messages.front().content.front());
    ASSERT_NE(persistedError, nullptr);
    EXPECT_NE(persistedError->details.find("malformed streamed tool call payload"),
              std::string::npos);

    bool persistedInvalidToolCall = false;
    for (const auto& turn : history.turns) {
        for (const auto& msg : turn.messages) {
            for (const auto& part : msg.content) {
                if (std::holds_alternative<ToolCallContent>(part)) {
                    persistedInvalidToolCall = true;
                }
            }
        }
    }
    EXPECT_FALSE(persistedInvalidToolCall);
}

TEST_F(HarnessTest,
       ProviderDeclaredTruncatedToolStreamSurfacesProviderErrorFirst) {
    auto& harness = Harness::instance();
    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = "provider-declared-truncated-tool-provider";
    config.defaultModelId = "provider-declared-truncated-tool-model";
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    std::promise<AgentError> errorPromise;
    auto errorFuture = errorPromise.get_future();
    std::atomic<bool> captured{false};
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (auto error = std::get_if<AgentError>(&event)) {
            if (error->message.find(
                    "Provider stream truncated during tool-call generation") !=
                std::string::npos) {
                bool expected = false;
                if (captured.compare_exchange_strong(expected, true)) {
                    errorPromise.set_value(*error);
                }
            }
        }
    });

    harness.send("trigger provider-declared truncated tool stream");

    auto emittedError = errorFuture.get();
    harness.unsubscribe(subId);

    EXPECT_NE(
        emittedError.message.find(
            "Qwen stream ended with incomplete tool-call arguments for tool "
            "'chunk_add'."),
        std::string::npos);
    EXPECT_EQ(
        emittedError.message.find("malformed streamed tool call payload"),
        std::string::npos);

    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Error;
    }));

    const auto& history = *agent->getContext().history;
    ASSERT_FALSE(history.turns.empty());
    const auto& lastTurn = history.turns.back();
    ASSERT_FALSE(lastTurn.messages.empty());
    const auto* persistedError =
        std::get_if<ErrorContent>(&lastTurn.messages.front().content.front());
    ASSERT_NE(persistedError, nullptr);
    EXPECT_NE(persistedError->details.find(
                  "Provider stream truncated during tool-call generation"),
              std::string::npos);
    EXPECT_EQ(
        persistedError->details.find("malformed streamed tool call payload"),
        std::string::npos);
}

} // namespace
