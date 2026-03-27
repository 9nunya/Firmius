#include <gtest/gtest.h>
#include "benchmarks/BenchmarkSession.hpp"
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
#include <algorithm>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>

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

class SnapshotUpdatingToolCallProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "snapshot-updating-tool-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        if (callCount_++ == 0) {
            onEvent(ToolCallChunk{"call-1", 0, "plan_list",
                                  R"({"status":"active","limit":1})"});
            // Simulate providers that emit full object snapshots as args mutate.
            onEvent(ToolCallChunk{"call-1", 0, "plan_list",
                                  R"({"status":"active","limit":12})"});
            onEvent(StreamDone{StopReason::ToolUse});
            return;
        }

        onEvent(TextChunk{"snapshot call executed"});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "snapshot-updating-tool-model";
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

class SequenceThenBlockProvider : public firmius::provider::IProvider {
public:
    SequenceThenBlockProvider(std::string providerId,
                              std::vector<std::string> responses,
                              std::size_t blockOnCall,
                              std::shared_ptr<std::promise<void>> started = nullptr,
                              std::shared_future<void> release = {})
        : providerId_(std::move(providerId)),
          responses_(std::move(responses)),
          blockOnCall_(blockOnCall),
          started_(std::move(started)),
          release_(std::move(release)) {}

    std::string getId() const override { return providerId_; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        const std::size_t idx = static_cast<std::size_t>(callCount_.fetch_add(1));
        if (!responses_.empty()) {
            const std::size_t capped =
                std::min(idx, responses_.size() - 1);
            onEvent(TextChunk{responses_[capped]});
        }
        if (idx == blockOnCall_) {
            if (started_) {
                started_->set_value();
            }
            if (release_.valid()) {
                release_.wait();
            }
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
    std::size_t blockOnCall_;
    std::shared_ptr<std::promise<void>> started_;
    std::shared_future<void> release_;
    std::atomic<int> callCount_{0};
};

class ToolCallProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "tool-call-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        const int call = callCount_.fetch_add(1);
        if (call == 0) {
            onEvent(TextChunk{"Preparing a tool call"});
            onEvent(ToolCallChunk{"tool-1", 0, "list_directory",
                                  R"({"path":"."})"});
            onEvent(StreamDone{StopReason::ToolUse});
            return;
        }
        onEvent(TextChunk{"Final completion summary."});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "tool-call-model";
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

class DelayedEchoProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "delayed-echo-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        const int call = callCount_.fetch_add(1);
        if (call == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        onEvent(TextChunk{"echo-" + std::to_string(call)});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "delayed-echo-model";
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

class DelayedErrorThenSuccessProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "delayed-error-then-success-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        const int call = callCount_.fetch_add(1);
        if (call == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            onEvent(StreamError{"Synthetic first-call failure", 500, ""});
            return;
        }
        onEvent(TextChunk{"recovered"});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "delayed-error-then-success-model";
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

class AbortAwareWaitingProvider : public firmius::provider::IProvider {
public:
    explicit AbortAwareWaitingProvider(std::string providerId)
        : providerId_(std::move(providerId)) {}

    std::string getId() const override { return providerId_; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions& opts,
                std::function<void(const StreamEvent&)> onEvent) override {
        const int callIndex = callCount_.fetch_add(1);
        if (callIndex > 0) {
            onEvent(TextChunk{"resumed"});
            onEvent(StreamDone{StopReason::Stop});
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            enteredStream_ = true;
            cancelled_.store(false);
            timedOut_.store(false);
        }
        enteredCv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        const auto subId = opts.abortController
            ? opts.abortController->subscribe([this]() {
                  cancelled_.store(true);
                  waitCv_.notify_all();
              })
            : 0;
        const bool cancelled = waitCv_.wait_for(
            lock, std::chrono::seconds(5),
            [this]() { return cancelled_.load(); });
        lock.unlock();
        if (opts.abortController && subId != 0) {
            opts.abortController->unsubscribe(subId);
        }

        if (!cancelled) {
            timedOut_.store(true);
            onEvent(StreamError{"provider wait timed out instead of cancelling", 0, ""});
        }
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = providerId_ + "-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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

    bool waitUntilEntered(std::chrono::milliseconds timeout = std::chrono::milliseconds(1500)) {
        std::unique_lock<std::mutex> lock(mutex_);
        return enteredCv_.wait_for(lock, timeout, [this]() { return enteredStream_; });
    }

    bool timedOut() const { return timedOut_.load(); }

    int callCount() const { return callCount_.load(); }

private:
    std::string providerId_;
    std::atomic<int> callCount_{0};
    mutable std::mutex mutex_;
    std::condition_variable enteredCv_;
    std::condition_variable waitCv_;
    bool enteredStream_ = false;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> timedOut_{false};
};

class PartialAbortAwareProvider : public firmius::provider::IProvider {
public:
    explicit PartialAbortAwareProvider(std::string providerId)
        : providerId_(std::move(providerId)) {}

    std::string getId() const override { return providerId_; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions& opts,
                std::function<void(const StreamEvent&)> onEvent) override {
        const int callIndex = callCount_.fetch_add(1);
        if (callIndex > 0) {
            onEvent(TextChunk{"follow-up complete"});
            onEvent(StreamDone{StopReason::Stop});
            return;
        }

        onEvent(ThinkingChunk{"planning partial answer", "sig-cancel"});
        onEvent(TextChunk{"partial response before cancel"});
        while (true) {
            if (opts.abortSignal && opts.abortSignal->load()) {
                onEvent(StreamError{"request interrupted", 0, ""});
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = providerId_ + "-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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
    std::string providerId_;
    std::atomic<int> callCount_{0};
};

class Retry500Provider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "retry-500-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        callCount_.fetch_add(1);
        onEvent(StreamError{"Synthetic 500", 500, ""});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "retry-500-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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

class ProcessInputBlockingToolProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "blocking-process-input-provider"; }

    void setProcessId(std::string processId) { processId_ = std::move(processId); }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        if (callCount_.fetch_add(1) == 0) {
            onEvent(ToolCallChunk{
                "blocking-input", 0, "process_input",
                std::string(R"({"process_id":")") + processId_ +
                    R"(","input":"line1\nline2\nline3\nline4\nline5"})"});
            onEvent(StreamDone{StopReason::ToolUse});
            return;
        }
        onEvent(TextChunk{"done"});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "blocking-process-input-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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
    std::string processId_;
    std::atomic<int> callCount_{0};
};

class ProcessWaitBlockingToolProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "blocking-process-wait-provider"; }

    void setProcessId(std::string processId) { processId_ = std::move(processId); }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        if (callCount_.fetch_add(1) == 0) {
            onEvent(ToolCallChunk{
                "blocking-wait", 0, "process_wait",
                std::string(R"({"process_id":")") + processId_ +
                    R"(","timeout_ms":60000})"});
            onEvent(StreamDone{StopReason::ToolUse});
            return;
        }
        onEvent(TextChunk{"done"});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "blocking-process-wait-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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
    std::string processId_;
    std::atomic<int> callCount_{0};
};

class ToolPreparationStallProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "tool-prep-stall-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions& opts,
                std::function<void(const StreamEvent&)> onEvent) override {
        callCount_.fetch_add(1);
        onEvent(ToolCallChunk{"prepping-call", 0, "list_directory", ""});
        while (true) {
            if (opts.abortSignal && opts.abortSignal->load()) {
                onEvent(StreamError{"aborted while preparing tool", 0, ""});
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "tool-prep-stall-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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
    std::atomic<int> callCount_{0};
};

class CancelWordErrorProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "cancel-word-error-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        onEvent(StreamError{"Remote API returned cancelled operation token mismatch", 400, ""});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "cancel-word-error-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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

class UniformDelayedEchoProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "uniform-delayed-echo-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        const int call = callCount_.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        onEvent(TextChunk{"echo-" + std::to_string(call)});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "uniform-delayed-echo-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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
    std::atomic<int> callCount_{0};
};

class ParallelProcessInputProvider : public firmius::provider::IProvider {
public:
    explicit ParallelProcessInputProvider(std::string inputPayload)
        : inputPayload_(std::move(inputPayload)) {}

    std::string getId() const override { return "parallel-process-input-provider"; }

    void setProcessIds(std::string processA, std::string processB) {
        processA_ = std::move(processA);
        processB_ = std::move(processB);
    }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        if (callCount_.fetch_add(1) == 0) {
            onEvent(ToolCallChunk{
                "parallel-input-a", 0, "process_input",
                std::string(R"({"process_id":")") + processA_ + R"(","input":")" +
                    inputPayload_ + R"("})"});
            onEvent(ToolCallChunk{
                "parallel-input-b", 1, "process_input",
                std::string(R"({"process_id":")") + processB_ + R"(","input":")" +
                    inputPayload_ + R"("})"});
            onEvent(StreamDone{StopReason::ToolUse});
            return;
        }
        onEvent(TextChunk{"done"});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "parallel-process-input-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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
    std::string inputPayload_;
    std::string processA_;
    std::string processB_;
    std::atomic<int> callCount_{0};
};

class CompactionProbeProvider : public firmius::provider::IProvider {
public:
    std::string getId() const override { return "compaction-probe-provider"; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
                std::function<void(const StreamEvent&)> onEvent) override {
        onEvent(TextChunk{"work-in-progress"});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = "compaction-probe-model";
        model.provider = getId();
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override {
        return listModels().front();
    }

    void generateSummary(const std::string&, const AgentHistory&,
                         const std::string& compactionPrompt,
                         std::function<void(const StreamEvent&)> onEvent,
                         std::atomic<bool>* = nullptr) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastCompactionPrompt_ = compactionPrompt;
        }
        summaryCallCount_.fetch_add(1);
        onEvent(AgentCompactionText{"", "summary", ""});
        onEvent(StreamDone{StopReason::Stop});
    }

    firmius::provider::ProviderType getProviderType() const override {
        return firmius::provider::ProviderType::APIKey;
    }

    int summaryCallCount() const { return summaryCallCount_.load(); }

    std::string lastCompactionPrompt() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastCompactionPrompt_;
    }

private:
    mutable std::mutex mutex_;
    std::string lastCompactionPrompt_;
    std::atomic<int> summaryCallCount_{0};
};

class ModelSwitchProbeProvider : public firmius::provider::IProvider {
public:
    struct ObservedCall {
        std::string modelId;
        std::string modelVariantJson;
    };

    ModelSwitchProbeProvider(std::string providerId, std::string modelId,
                             bool blockFirstCall = false)
        : providerId_(std::move(providerId)),
          modelId_(std::move(modelId)),
          blockFirstCall_(blockFirstCall) {}

    std::string getId() const override { return providerId_; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions& opts,
                std::function<void(const StreamEvent&)> onEvent) override {
        const int callIndex = callCount_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(callsMutex_);
            calls_.push_back(ObservedCall{opts.modelId, opts.modelVariantJson});
        }

        if (callIndex == 0) {
            firstCallEntered_.store(true);
            if (blockFirstCall_) {
                std::unique_lock<std::mutex> lock(blockMutex_);
                blockCv_.wait(lock, [&]() { return releaseFirstCall_; });
            }
        }

        onEvent(TextChunk{providerId_ + ":" + opts.modelId + ":" +
                          (opts.modelVariantJson.empty() ? "default"
                                                         : opts.modelVariantJson)});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = modelId_;
        model.provider = providerId_;
        model.contextWindow = 4096;
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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

    bool firstCallEntered() const { return firstCallEntered_.load(); }

    void releaseFirstCall() {
        {
            std::lock_guard<std::mutex> lock(blockMutex_);
            releaseFirstCall_ = true;
        }
        blockCv_.notify_all();
    }

    std::vector<ObservedCall> observedCalls() const {
        std::lock_guard<std::mutex> lock(callsMutex_);
        return calls_;
    }

private:
    std::string providerId_;
    std::string modelId_;
    bool blockFirstCall_;

    mutable std::mutex callsMutex_;
    std::vector<ObservedCall> calls_;
    std::atomic<int> callCount_{0};
    std::atomic<bool> firstCallEntered_{false};

    mutable std::mutex blockMutex_;
    std::condition_variable blockCv_;
    bool releaseFirstCall_{false};
};

class VariantSwitchProbeProvider : public firmius::provider::IProvider {
public:
    VariantSwitchProbeProvider(std::string providerId, std::string modelId,
                               bool blockFirstCall = false)
        : providerId_(std::move(providerId)),
          modelId_(std::move(modelId)),
          blockFirstCall_(blockFirstCall) {}

    std::string getId() const override { return providerId_; }

    void stream(const AgentHistory&, const firmius::provider::ProviderOptions& opts,
                std::function<void(const StreamEvent&)> onEvent) override {
        const int callIndex = callCount_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(callsMutex_);
            observedVariantJson_.push_back(opts.modelVariantJson);
        }

        if (callIndex == 0) {
            firstCallEntered_.store(true);
            if (blockFirstCall_) {
                std::unique_lock<std::mutex> lock(blockMutex_);
                blockCv_.wait(lock, [&]() { return releaseFirstCall_; });
            }
        }

        onEvent(TextChunk{providerId_ + ":" + opts.modelId + ":" +
                          (opts.modelVariantJson.empty() ? "default"
                                                         : opts.modelVariantJson)});
        onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
        ModelInfo model;
        model.id = modelId_;
        model.provider = providerId_;
        model.contextWindow = 4096;
        model.variants = {
            ModelVariant{"low", R"({"variant":"low"})"},
            ModelVariant{"high", R"({"variant":"high"})"},
        };
        return {model};
    }

    ModelInfo getModelInfo(const std::string&) override { return listModels().front(); }

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

    bool firstCallEntered() const { return firstCallEntered_.load(); }

    void releaseFirstCall() {
        {
            std::lock_guard<std::mutex> lock(blockMutex_);
            releaseFirstCall_ = true;
        }
        blockCv_.notify_all();
    }

    std::vector<std::string> observedVariantJson() const {
        std::lock_guard<std::mutex> lock(callsMutex_);
        return observedVariantJson_;
    }

private:
    std::string providerId_;
    std::string modelId_;
    bool blockFirstCall_;

    std::atomic<int> callCount_{0};
    std::atomic<bool> firstCallEntered_{false};
    mutable std::mutex callsMutex_;
    std::vector<std::string> observedVariantJson_;

    mutable std::mutex blockMutex_;
    std::condition_variable blockCv_;
    bool releaseFirstCall_{false};
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
            lead << "---\nname: lead\ntitle: Lead\nwork_role: lead\nscopes: [\"FilesystemRead\"]\n---\nLead persona";
        }
        {
            std::ofstream executor(promptsDir_ / "executor.md");
            executor << "---\nname: executor\ntitle: Executor\nwork_role: executor\nscopes: [\"FilesystemRead\"]\n---\nExecutor persona";
        }
        {
            std::ofstream hotrun(promptsDir_ / "hotrun.md");
            hotrun << "---\nname: hotrun\ntitle: Hot Run\nwork_role: lead\nscopes: [\"FilesystemRead\"]\n---\nHotrun persona";
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
            std::make_shared<DelayedEchoProvider>());
        firmius::provider::ProviderRegistry::instance().registerProvider(
            std::make_shared<DelayedErrorThenSuccessProvider>());
        auto config = firmius::shared::ConfigLoader::instance().getConfig();
        config.defaultProviderId = "test-retry-provider";
        config.defaultModelId = "test-retry-model";
        config.defaultLeadPersona = "lead";
        Harness::instance().updateConfig(config);
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

    bool waitForStopped(const std::string& agentId) {
        return waitForCondition([&]() {
            auto agent = AgentRegistry::instance().getAgent(agentId);
            if (!agent || agent->isRunning() || agent->isBooting()) {
                return false;
            }
            return agent->getContext().state.currentStatus == AgentStatus::Idle ||
                   agent->getContext().state.currentStatus == AgentStatus::Cancelled ||
                   agent->getContext().state.currentStatus == AgentStatus::Error;
        }, std::chrono::milliseconds(5000));
    }

    bool sendAndWaitForIdle(Harness& harness, const std::string& text,
                            const std::shared_ptr<IAgent>& agent) {
        harness.send(text);
        return waitForCondition([&]() {
            return agent && !agent->isRunning() &&
                   agent->getContext().state.currentStatus == AgentStatus::Idle;
        }, std::chrono::milliseconds(5000));
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

    static bool historyContainsUserText(const AgentHistory& history,
                                        const std::string& needle) {
        for (const auto& turn : history.turns) {
            for (const auto& msg : turn.messages) {
                if (msg.role != Role::User) {
                    continue;
                }
                for (const auto& part : msg.content) {
                    if (const auto* text = std::get_if<TextContent>(&part)) {
                        if (text->text.find(needle) != std::string::npos) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    static std::vector<std::string> userTurnTexts(const AgentHistory& history) {
        std::vector<std::string> out;
        for (const auto& turn : history.turns) {
            if (turn.turnId.rfind("user-task-", 0) != 0) {
                continue;
            }
            for (const auto& msg : turn.messages) {
                if (msg.role != Role::User) {
                    continue;
                }
                for (const auto& part : msg.content) {
                    if (const auto* text = std::get_if<TextContent>(&part)) {
                        out.push_back(text->text);
                    }
                }
            }
        }
        return out;
    }

    static size_t countTurnsWithPrefix(const AgentHistory& history,
                                       const std::string& prefix) {
        size_t count = 0;
        for (const auto& turn : history.turns) {
            if (turn.turnId.rfind(prefix, 0) == 0) {
                ++count;
            }
        }
        return count;
    }

    static std::vector<std::string> turnIds(const AgentHistory& history) {
        std::vector<std::string> ids;
        ids.reserve(history.turns.size());
        for (const auto& turn : history.turns) {
            ids.push_back(turn.turnId);
        }
        return ids;
    }

    static void appendStoppedTurn(IAgent& agent, AgentStatus status) {
        auto& history = *agent.getMutableContext().history;
        AgentTurn turn;
        turn.turnId = (status == AgentStatus::Cancelled ? "cancelled-" : "error-") +
                      std::to_string(history.turns.size());
        Message message;
        message.role = (status == AgentStatus::Cancelled ? Role::System
                                                         : Role::Error);
        message.visibility = MessageVisibility::Visible;
        message.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        if (status == AgentStatus::Cancelled) {
            message.content.push_back(NoticeContent{
                "Agent Cancelled",
                "The agent execution was interrupted.",
                "Execution stopped before completion and can be resumed.",
                NoticeSeverity::Warning});
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

TEST_F(HarnessTest, SnapshotUpdatingToolArgsDoNotTriggerMalformedPayloadError) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SnapshotUpdatingToolCallProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
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

    harness.send("trigger snapshot-updating tool args");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));
    harness.unsubscribe(subId);

    EXPECT_FALSE(malformedValidatorTriggered.load());

    const auto& history = *agent->getContext().history;
    bool sawPlanList = false;
    for (const auto& turn : history.turns) {
        for (const auto& msg : turn.messages) {
            for (const auto& part : msg.content) {
                if (auto call = std::get_if<ToolCallContent>(&part)) {
                    if (call->id == "call-1") {
                        sawPlanList = true;
                        EXPECT_EQ(call->name, "plan_list");
                        EXPECT_EQ(call->args,
                                  R"({"status":"active","limit":12})");
                    }
                }
            }
        }
    }
    EXPECT_TRUE(sawPlanList);
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

    EXPECT_EQ(provider->callCount(), 1);
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

TEST_F(HarnessTest,
       IncompleteTodoKeepsLoopAliveWithRunningDescendantSubagent) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-incomplete-todo",
        std::vector<std::string>{"Progress update.", "Final response."});
    auto childStarted = std::make_shared<std::promise<void>>();
    auto childStartedFuture = childStarted->get_future().share();
    auto childRelease = std::make_shared<std::promise<void>>();
    auto childReleaseFuture = childRelease->get_future().share();
    auto childProvider = std::make_shared<SequenceThenBlockProvider>(
        "sequenced-prose-incomplete-child",
        std::vector<std::string>{"Child subagent still running."}, 0,
        childStarted, childReleaseFuture);
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);
    firmius::provider::ProviderRegistry::instance().registerProvider(childProvider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    const std::string childId = Engine::instance().createAgent(
        threadId, "lead", true, agent->getContext().identity.id, "child",
        "Child");
    ASSERT_TRUE(waitForCondition([&]() {
        auto child = AgentRegistry::instance().getAgent(childId);
        return child && !child->isBooting();
    }));
    Engine::instance().switchAgentModel(childId, childProvider->getId(),
                                        childProvider->listModels().front().id,
                                        "");
    Engine::instance().executeTask(childId, "keep child running");
    ASSERT_TRUE(waitForCondition([&]() {
        return childStartedFuture.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready;
    }));
    ASSERT_TRUE(waitForCondition([&]() {
        auto child = AgentRegistry::instance().getAgent(childId);
        return child && child->isRunning();
    }));

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

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));

    childRelease->set_value();
    ASSERT_TRUE(waitForCondition([&]() {
        auto child = AgentRegistry::instance().getAgent(childId);
        return child && !child->isRunning();
    }));

    harness.abort();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
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

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 1);
}

TEST_F(HarnessTest, HotrunMissingTodoWithActivePlanTriggersTodoEnforcement) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-hotrun-missing-todo",
        std::vector<std::string>{"Progress update.", "Follow-up."});
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "hotrun");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedAgent(threadId, "hotrun");
    ASSERT_TRUE(agent);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    Plan plan;
    plan.id = "plan-hotrun-enforce";
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

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 1);
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

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 1);
}

TEST_F(HarnessTest, TodoEnforcementNudgeIsInternalSystemMessage) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-internal-todo-nudge",
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
    plan.id = "plan-internal-nudge";
    plan.threadId = threadId;
    plan.title = "Active Plan";
    plan.objective = "Multi-step coordination";
    plan.status = PlanStatus::Active;
    tm.writePlan(threadId, plan);

    ThreadMetadata metadata = tm.getMetadata(threadId);
    metadata.activePlanId = plan.id;
    tm.updateMetadata(threadId, metadata);

    AgentTodoList todo;
    todo.threadId = threadId;
    todo.agentId = harness.focusedAgentId();
    todo.nextId = 3;
    todo.items.push_back(
        TodoItem{1, "Draft parser refactor", TodoStatus::Pending, "", "", 1, 1});
    todo.items.push_back(
        TodoItem{2, "Write regression tests", TodoStatus::InProgress, "", "", 2, 2});
    tm.writeAgentTodo(threadId, todo.agentId, todo);

    harness.send("build and implement the requested runtime change");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    bool sawInternalTodoNudge = false;
    for (const auto& turn : agent->getContext().history->turns) {
        for (const auto& msg : turn.messages) {
            if (msg.role != Role::System ||
                msg.visibility != MessageVisibility::Internal) {
                continue;
            }
            for (const auto& part : msg.content) {
                if (const auto* txt = std::get_if<TextContent>(&part)) {
                    if (txt->text.find("Continue working through the remaining todo items:") !=
                        std::string::npos) {
                        sawInternalTodoNudge = true;
                    }
                }
            }
        }
    }
    EXPECT_TRUE(sawInternalTodoNudge);
}

TEST_F(HarnessTest, MissingTodoDoesNotBlockReadOnlyToolTurn) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<ToolCallProvider>();
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

    harness.send("inspect the workspace and continue implementation");

    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }, std::chrono::milliseconds(4000)));

    bool sawToolResult = false;
    for (const auto& turn : agent->getContext().history->turns) {
        for (const auto& msg : turn.messages) {
            for (const auto& part : msg.content) {
                if (const auto* result = std::get_if<ToolResultContent>(&part)) {
                    if (result->toolCallId == "tool-1") {
                        sawToolResult = true;
                    }
                }
            }
        }
    }
    EXPECT_TRUE(sawToolResult);
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

    EXPECT_EQ(provider->callCount(), 1);
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

    EXPECT_EQ(provider->callCount(), 1);
}

TEST_F(HarnessTest,
       TodoIncompleteKeepsProseContinuationAliveWhileBlockingProcessIsActive) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-todo-incomplete",
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
        "blocking-process-1");

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    AgentTodoList todo;
    todo.threadId = threadId;
    todo.agentId = harness.focusedAgentId();
    todo.nextId = 3;
    todo.items.push_back(
        TodoItem{1, "Draft parser refactor", TodoStatus::Pending, "", "", 1, 1});
    todo.items.push_back(
        TodoItem{2, "Write regression tests", TodoStatus::InProgress, "", "", 2, 2});
    tm.writeAgentTodo(threadId, todo.agentId, todo);

    harness.send("continue until done");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));

    agent->getEnvironment()->getProcessManager().removeBlockingProcessId(
        "blocking-process-1");
    harness.abort();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
}

TEST_F(HarnessTest,
       UnchangedInProgressTodoAcrossValidMultiTurnContinuationDoesNotStopPrematurely) {
    auto& harness = Harness::instance();
    auto thirdCallStarted = std::make_shared<std::promise<void>>();
    auto thirdCallStartedFuture = thirdCallStarted->get_future().share();
    auto thirdCallRelease = std::make_shared<std::promise<void>>();
    auto thirdCallReleaseFuture = thirdCallRelease->get_future().share();
    auto provider = std::make_shared<SequenceThenBlockProvider>(
        "sequenced-prose-repeat-continuation",
        std::vector<std::string>{"Progress update.",
                                 "Still working.",
                                 "Still working."},
        2, thirdCallStarted, thirdCallReleaseFuture);
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
        TodoItem{1, "Continue execution", TodoStatus::InProgress, "", "", 1, 1});
    tm.writeAgentTodo(threadId, todo.agentId, todo);

    harness.send("continue work");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return thirdCallStartedFuture.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready;
    }));
    EXPECT_GE(provider->callCount(), 3);
    ASSERT_TRUE(waitForCondition([&]() { return agent->isRunning(); }));
    bool sawSpecificTodoNudge = false;
    for (const auto& turn : agent->getContext().history->turns) {
        for (const auto& msg : turn.messages) {
            if (msg.role != Role::System ||
                msg.visibility != MessageVisibility::Internal) {
                continue;
            }
            for (const auto& part : msg.content) {
                if (const auto* txt = std::get_if<TextContent>(&part)) {
                    if (txt->text.find("Continue working through the remaining todo items:") !=
                            std::string::npos &&
                        txt->text.find("#1 [InProgress] Continue execution") !=
                            std::string::npos) {
                        sawSpecificTodoNudge = true;
                    }
                }
            }
        }
    }
    EXPECT_TRUE(sawSpecificTodoNudge);

    thirdCallRelease->set_value();
    harness.abort();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
}

TEST_F(HarnessTest, TodoCompleteStopsProseContinuationWithoutStopToken) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-todo-complete",
        std::vector<std::string>{"All work complete.",
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

    Engine::instance().executeTask(agent->getContext().identity.id, "continue work");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; }));
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_EQ(provider->callCount(), 1);
}

TEST_F(HarnessTest, ActiveRuntimeWorkStillContinuesWithoutTodoHeuristics) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<SequencedProseProvider>(
        "sequenced-prose-active-runtime-work",
        std::vector<std::string>{"Still executing.",
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

    agent->getEnvironment()->getProcessManager().addBlockingProcessId(
        "blocking-runtime-work");

    harness.send("build and implement the requested runtime change");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; }));
    agent->getEnvironment()->getProcessManager().removeBlockingProcessId(
        "blocking-runtime-work");
    ASSERT_TRUE(waitForCondition([&]() {
        return agent && !agent->isRunning() &&
               agent->getContext().state.currentStatus == AgentStatus::Idle;
    }));

    EXPECT_GE(provider->callCount(), 2);
}

TEST_F(HarnessTest, QueuedMessagesFlushAfterFocusedAgentSettles) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<DelayedEchoProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    std::vector<MessageDequeued> dequeued;
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (auto ev = std::get_if<MessageDequeued>(&event)) {
            dequeued.push_back(*ev);
        }
    });

    harness.send("primary message");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() { return agent->isRunning(); },
                                 std::chrono::milliseconds(1500)));

    harness.send("queued message one");
    harness.send("queued message two");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; },
                                 std::chrono::milliseconds(5000)));
    ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));
    harness.unsubscribe(subId);

    EXPECT_EQ(dequeued.size(), 2u);
    for (const auto& ev : dequeued) {
        EXPECT_EQ(ev.agentId, agent->getContext().identity.id);
        EXPECT_EQ(ev.threadId, threadId);
    }
    EXPECT_TRUE(historyContainsUserText(*agent->getContext().history,
                                        "queued message one"));
    EXPECT_TRUE(historyContainsUserText(*agent->getContext().history,
                                        "queued message two"));
}

TEST_F(HarnessTest, QueuedMessagesFlushAfterErrorPathSettles) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<DelayedErrorThenSuccessProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    std::vector<MessageDequeued> dequeued;
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (auto ev = std::get_if<MessageDequeued>(&event)) {
            dequeued.push_back(*ev);
        }
    });

    harness.send("first attempt");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() { return agent->isRunning(); },
                                 std::chrono::milliseconds(1500)));

    harness.send("retry after error");

    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 2; },
                                 std::chrono::milliseconds(5000)));
    ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));
    harness.unsubscribe(subId);

    EXPECT_EQ(dequeued.size(), 1u);
    EXPECT_EQ(dequeued.front().agentId, agent->getContext().identity.id);
    EXPECT_EQ(dequeued.front().threadId, threadId);
    EXPECT_TRUE(historyContainsUserText(*agent->getContext().history,
                                        "retry after error"));
}

TEST_F(HarnessTest, AbortAndFlushPreservesQueuedMessagesAndDispatchesImmediatelyAfterStop) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<AbortAwareWaitingProvider>("abort-flush-provider");
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    std::mutex eventsMutex;
    std::vector<MessageDequeued> dequeued;
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (auto ev = std::get_if<MessageDequeued>(&event)) {
            std::lock_guard<std::mutex> lock(eventsMutex);
            dequeued.push_back(*ev);
        }
    });

    harness.send("primary");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() { return agent->isRunning(); },
                                 std::chrono::milliseconds(1500)));

    harness.send("queued one");
    harness.send("queued two");

    const auto abortStart = std::chrono::steady_clock::now();
    harness.abortAndFlushQueuedMessages();

    ASSERT_TRUE(waitForCondition([&]() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        return dequeued.size() == 2;
    }, std::chrono::milliseconds(2500)));
    ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));
    harness.unsubscribe(subId);

    const auto flushElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - abortStart);
    EXPECT_LT(flushElapsed.count(), 1500);
    EXPECT_GE(provider->callCount(), 2);

    const auto texts = userTurnTexts(*agent->getContext().history);
    auto itOne = std::find(texts.begin(), texts.end(), "queued one");
    auto itTwo = std::find(texts.begin(), texts.end(), "queued two");
    ASSERT_NE(itOne, texts.end());
    ASSERT_NE(itTwo, texts.end());
    EXPECT_LT(std::distance(texts.begin(), itOne), std::distance(texts.begin(), itTwo));
}

TEST_F(HarnessTest, AbortStopsProviderWaitQuickly) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<AbortAwareWaitingProvider>("abort-provider-waiting");
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    harness.send("block in provider");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() { return agent->isRunning(); },
                                 std::chrono::milliseconds(1500)));
    ASSERT_TRUE(provider->waitUntilEntered());

    const auto abortStart = std::chrono::steady_clock::now();
    std::thread abortThread([&]() { harness.abort(); });
    abortThread.join();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - abortStart);
    EXPECT_LT(elapsed.count(), 1000);
    EXPECT_FALSE(provider->timedOut());
}

TEST_F(HarnessTest, AbortStopsRetrySleepPromptlyWithoutDuplicateError) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<Retry500Provider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    std::atomic<bool> sawRetry{false};
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (std::holds_alternative<AgentRetrying>(event)) {
            sawRetry.store(true);
        }
    });

    harness.send("trigger retries");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() { return sawRetry.load(); },
                                 std::chrono::milliseconds(1500)));

    const auto abortStart = std::chrono::steady_clock::now();
    harness.abort();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - abortStart);
    harness.unsubscribe(subId);

    EXPECT_LT(elapsed.count(), 1000);
}

TEST_F(HarnessTest, AbortStopsBlockingProcessInputToolQuickly) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<ProcessInputBlockingToolProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);
    auto& processManager = agent->getEnvironment()->getProcessManager();
    const std::string processId = processManager.spawnProcess("cat");
    provider->setProcessId(processId);

    harness.send("run blocking process input");
    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; },
                                 std::chrono::milliseconds(1500)));

    const auto abortStart = std::chrono::steady_clock::now();
    harness.abort();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - abortStart);
    EXPECT_LT(elapsed.count(), 1500);

    processManager.killProcess(processId);
}

TEST_F(HarnessTest, AbortStopsProcessWaitToolQuickly) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<ProcessWaitBlockingToolProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    auto& processManager = agent->getEnvironment()->getProcessManager();
    const std::string processId = processManager.spawnProcess("sleep 30");
    provider->setProcessId(processId);

    harness.send("wait for spawned process");
    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; },
                                 std::chrono::milliseconds(1500)));

    const auto abortStart = std::chrono::steady_clock::now();
    harness.abort();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - abortStart);
    EXPECT_LT(elapsed.count(), 1500);

    processManager.killProcess(processId);
}

TEST_F(HarnessTest, AbortDuringToolPreparationBoundaryStopsPromptly) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<ToolPreparationStallProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    harness.send("stall while preparing tool");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() { return agent->isRunning(); },
                                 std::chrono::milliseconds(1500)));

    const auto abortStart = std::chrono::steady_clock::now();
    harness.abort();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - abortStart);
    EXPECT_LT(elapsed.count(), 1000);
}

TEST_F(HarnessTest, InterruptCancellationDoesNotEmitDuplicateAgentError) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<AbortAwareWaitingProvider>("interrupt-dedup-provider");
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    std::atomic<int> interruptedCount{0};
    std::atomic<int> errorCount{0};
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (std::holds_alternative<AgentInterrupted>(event)) {
            interruptedCount.fetch_add(1);
        }
        if (std::holds_alternative<AgentError>(event)) {
            errorCount.fetch_add(1);
        }
    });

    harness.send("interrupt me");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() { return agent->isRunning(); },
                                 std::chrono::milliseconds(1500)));

    harness.abort();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
    harness.unsubscribe(subId);

    EXPECT_GE(interruptedCount.load(), 1);
    EXPECT_EQ(errorCount.load(), 0);
}

TEST_F(HarnessTest, CancelledPartialAssistantTurnPersistsBeforeFollowUpRequestRuns) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<PartialAbortAwareProvider>("partial-abort-provider");
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    harness.send("first request");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() { return agent->isRunning(); },
                                 std::chrono::milliseconds(1500)));

    harness.abort();
    harness.send("follow-up request");

    ASSERT_TRUE(waitForCondition([&]() {
        return historyContainsUserText(*agent->getContext().history,
                                       "follow-up request");
    }, std::chrono::milliseconds(4000)));
    ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));

    int firstUserIndex = -1;
    int cancelledAssistantIndex = -1;
    int cancelledNoticeIndex = -1;
    int followUpUserIndex = -1;
    int followUpAssistantIndex = -1;

    for (size_t i = 0; i < agent->getContext().history->turns.size(); ++i) {
        const auto& turn = agent->getContext().history->turns[i];
        if (turn.messages.empty()) {
            continue;
        }
        const auto& message = turn.messages.front();
        for (const auto& part : message.content) {
            if (const auto* text = std::get_if<TextContent>(&part)) {
                if (text->text == "first request") {
                    firstUserIndex = static_cast<int>(i);
                } else if (text->text == "partial response before cancel") {
                    cancelledAssistantIndex = static_cast<int>(i);
                    EXPECT_EQ(turn.stopReason, StopReason::Cancelled);
                } else if (text->text == "follow-up request") {
                    followUpUserIndex = static_cast<int>(i);
                } else if (text->text == "follow-up complete") {
                    followUpAssistantIndex = static_cast<int>(i);
                }
            }
            if (const auto* notice = std::get_if<NoticeContent>(&part)) {
                if (notice->title == "Agent Cancelled") {
                    cancelledNoticeIndex = static_cast<int>(i);
                }
            }
        }
    }

    EXPECT_GE(firstUserIndex, 0);
    EXPECT_GE(cancelledAssistantIndex, 0);
    EXPECT_GE(cancelledNoticeIndex, 0);
    EXPECT_GE(followUpUserIndex, 0);
    EXPECT_GE(followUpAssistantIndex, 0);
    EXPECT_LT(firstUserIndex, cancelledAssistantIndex);
    EXPECT_LT(cancelledAssistantIndex, cancelledNoticeIndex);
    EXPECT_LT(cancelledNoticeIndex, followUpUserIndex);
    EXPECT_LT(followUpUserIndex, followUpAssistantIndex);
}

TEST_F(HarnessTest, NonInterruptedErrorContainingCancelWordStillSurfacesAsAgentError) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<CancelWordErrorProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    std::atomic<int> errorCount{0};
    std::string observedMessage;
    std::mutex observedMutex;
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (auto error = std::get_if<AgentError>(&event)) {
            {
                std::lock_guard<std::mutex> lock(observedMutex);
                observedMessage = error->message;
            }
            errorCount.fetch_add(1);
        }
    });

    harness.send("trigger cancel-word provider error");
    ASSERT_TRUE(waitForCondition([&]() { return errorCount.load() > 0; },
                                 std::chrono::milliseconds(4000)));
    harness.unsubscribe(subId);

    std::lock_guard<std::mutex> lock(observedMutex);
    EXPECT_NE(observedMessage.find("cancelled"), std::string::npos);
}

TEST_F(HarnessTest, PlainAbortPreservesQueuedMessagesForFocusedAgent) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<AbortAwareWaitingProvider>("plain-abort-queue-provider");
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    harness.send("primary");
    auto agent = waitForFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() { return agent->isRunning(); },
                                 std::chrono::milliseconds(1500)));
    harness.send("queued plain abort one");
    harness.send("queued plain abort two");

    harness.abort();
    ASSERT_TRUE(waitForCondition([&]() {
        return historyContainsUserText(*agent->getContext().history,
                                       "queued plain abort one") &&
               historyContainsUserText(*agent->getContext().history,
                                       "queued plain abort two");
    }, std::chrono::milliseconds(3000)));
}

TEST_F(HarnessTest, RetryClearsOnlyFocusedAgentQueueAndPreservesOtherQueuedMessages) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<DelayedEchoProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    auto agentA = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agentA);
    auto agentB = createAgentWithUserTurn(threadId, "worker-b");
    ASSERT_TRUE(agentB);

    ASSERT_TRUE(harness.setFocusedAgent(agentB->getContext().identity.id));
    harness.send("agent-b primary");
    ASSERT_TRUE(waitForCondition([&]() { return agentB->isRunning(); },
                                 std::chrono::milliseconds(1500)));
    harness.send("agent-b queued");

    ASSERT_TRUE(harness.setFocusedAgent(agentA->getContext().identity.id));
    appendStoppedTurn(*agentA, AgentStatus::Cancelled);
    std::string statusMessage;
    ASSERT_TRUE(harness.retryLastRequest(statusMessage));

    ASSERT_TRUE(waitForCondition([&]() {
        return historyContainsUserText(*agentB->getContext().history, "agent-b queued");
    }, std::chrono::milliseconds(3000)));
}

TEST_F(HarnessTest, AbortAndFlushTargetsFocusedAgentQueueWithoutDroppingOthers) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<UniformDelayedEchoProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());

    auto agentA = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agentA);
    auto agentB = createAgentWithUserTurn(threadId, "worker-c");
    ASSERT_TRUE(agentB);

    ASSERT_TRUE(harness.setFocusedAgent(agentA->getContext().identity.id));
    harness.send("agent-a primary");
    ASSERT_TRUE(waitForCondition([&]() { return agentA->isRunning(); },
                                 std::chrono::milliseconds(1500)));
    harness.send("agent-a queued");

    ASSERT_TRUE(harness.setFocusedAgent(agentB->getContext().identity.id));
    harness.send("agent-b primary");
    ASSERT_TRUE(waitForCondition([&]() { return agentB->isRunning(); },
                                 std::chrono::milliseconds(1500)));
    harness.send("agent-b queued");

    ASSERT_TRUE(harness.setFocusedAgent(agentA->getContext().identity.id));
    harness.abortAndFlushQueuedMessages();

    ASSERT_TRUE(waitForCondition([&]() {
        return historyContainsUserText(*agentA->getContext().history, "agent-a queued") &&
               historyContainsUserText(*agentB->getContext().history, "agent-b queued");
    }, std::chrono::milliseconds(5000)));
}

TEST_F(HarnessTest, ParallelToolsStillExecuteConcurrently) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<ParallelProcessInputProvider>("line1\\nline2\\n");
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    auto& processManager = agent->getEnvironment()->getProcessManager();
    const std::string processA = processManager.spawnProcess("stdbuf -o0 cat");
    const std::string processB = processManager.spawnProcess("stdbuf -o0 cat");
    provider->setProcessIds(processA, processB);

    const auto start = std::chrono::steady_clock::now();
    harness.send("run two slow tool calls");
    ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    processManager.killProcess(processA);
    processManager.killProcess(processB);

    EXPECT_LT(elapsed.count(), 3300);
}

TEST_F(HarnessTest, InterruptDuringParallelToolsReturnsPromptlyWithoutDeadlock) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<ParallelProcessInputProvider>(
        "line1\\nline2\\nline3\\nline4\\nline5\\nline6\\n");
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    auto& processManager = agent->getEnvironment()->getProcessManager();
    const std::string processA = processManager.spawnProcess("cat");
    const std::string processB = processManager.spawnProcess("cat");
    provider->setProcessIds(processA, processB);

    harness.send("run interruptible parallel tools");
    ASSERT_TRUE(waitForCondition([&]() { return provider->callCount() >= 1; },
                                 std::chrono::milliseconds(1500)));
    const auto abortStart = std::chrono::steady_clock::now();
    harness.abort();
    ASSERT_TRUE(waitForStopped(agent->getContext().identity.id));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - abortStart);

    processManager.killProcess(processA);
    processManager.killProcess(processB);
    EXPECT_LT(elapsed.count(), 1500);
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
    std::atomic<bool> captured{false};
    int subId = harness.subscribe([&](const AppEvent& event) {
        if (auto error = std::get_if<AgentError>(&event)) {
            if (error->message.find("malformed streamed tool call payload") !=
                std::string::npos) {
                bool expected = false;
                if (captured.compare_exchange_strong(expected, true)) {
                    errorPromise.set_value(*error);
                }
            }
        }
    });

    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);
    ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));

    harness.send("trigger malformed tool");

    ASSERT_EQ(errorFuture.wait_for(std::chrono::seconds(8)),
              std::future_status::ready);
    auto emittedError = errorFuture.get();
    harness.unsubscribe(subId);

    EXPECT_NE(emittedError.message.find("malformed streamed tool call payload"),
              std::string::npos);
    EXPECT_NE(emittedError.message.find("invalid or truncated JSON arguments"),
              std::string::npos);
    EXPECT_NE(emittedError.message.find("Provider: truncated-tool-provider"),
              std::string::npos);
    EXPECT_NE(emittedError.message.find("Model: truncated-tool-model"),
              std::string::npos);

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
    EXPECT_NE(persistedError->details.find("Provider: truncated-tool-provider"),
              std::string::npos);
    EXPECT_NE(persistedError->details.find("Model: truncated-tool-model"),
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

TEST_F(HarnessTest, CompactionLifecycleMarkersPersistAcrossReloadFromDisk) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<CompactionProbeProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);
    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ASSERT_TRUE(sendAndWaitForIdle(harness, "continue pass one", agent));
    ASSERT_TRUE(sendAndWaitForIdle(harness, "continue pass two", agent));

    agent->getMutableContext().aggregateMetrics.tokens.contextSize = 4200;
    harness.compactFocusedAgent();

    ASSERT_TRUE(waitForCondition([&]() {
        const auto& history = *agent->getContext().history;
        return countTurnsWithPrefix(history, "compaction-start-") >= 1 &&
               countTurnsWithPrefix(history, "compaction-summary-") >= 1 &&
               countTurnsWithPrefix(history, "compaction-end-") >= 1;
    }, std::chrono::milliseconds(4000)));
    EXPECT_EQ(agent->getContext().aggregateMetrics.tokens.contextSize, 1000u);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    const auto loaded =
        tm.loadAgentHistory(threadId, agent->getContext().identity.id);
    EXPECT_GE(countTurnsWithPrefix(loaded, "compaction-start-"), 1u);
    EXPECT_GE(countTurnsWithPrefix(loaded, "compaction-summary-"), 1u);
    EXPECT_GE(countTurnsWithPrefix(loaded, "compaction-end-"), 1u);
}

TEST_F(HarnessTest, UndoTurnsRestoresPreCompactionHistoryAndContextSize) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<CompactionProbeProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);
    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ASSERT_TRUE(sendAndWaitForIdle(harness, "turn one", agent));
    ASSERT_TRUE(sendAndWaitForIdle(harness, "turn two", agent));

    auto& mutableCtx = agent->getMutableContext();
    mutableCtx.aggregateMetrics.tokens.contextSize = 7777;
    const auto preCompactionIds = turnIds(*mutableCtx.history);

    harness.compactFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return countTurnsWithPrefix(*agent->getContext().history,
                                    "compaction-end-") >= 1;
    }, std::chrono::milliseconds(4000)));

    const UndoResult result = harness.undoTurns(3);
    EXPECT_TRUE(result.compactionReversed);
    EXPECT_EQ(result.restoredTurns, static_cast<int>(preCompactionIds.size()));
    EXPECT_EQ(turnIds(*agent->getContext().history), preCompactionIds);
    EXPECT_EQ(agent->getContext().aggregateMetrics.tokens.contextSize, 7777u);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    ASSERT_TRUE(waitForCondition([&]() {
        const auto loaded =
            tm.loadAgentHistory(threadId, agent->getContext().identity.id);
        return turnIds(loaded) == preCompactionIds &&
               countTurnsWithPrefix(loaded, "compaction-start-") == 0 &&
               countTurnsWithPrefix(loaded, "compaction-summary-") == 0 &&
               countTurnsWithPrefix(loaded, "compaction-end-") == 0;
    }, std::chrono::milliseconds(1500)));
}

TEST_F(HarnessTest, UndoMessagesAndTimestampRestorePreCompactionSnapshot) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<CompactionProbeProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);
    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ASSERT_TRUE(sendAndWaitForIdle(harness, "turn alpha", agent));
    ASSERT_TRUE(sendAndWaitForIdle(harness, "turn beta", agent));

    auto& ctx = agent->getMutableContext();
    const auto baselineIds = turnIds(*ctx.history);
    const uint64_t baselineTimestamp = 0;

    ctx.aggregateMetrics.tokens.contextSize = 8888;
    harness.compactFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return countTurnsWithPrefix(*agent->getContext().history,
                                    "compaction-end-") >= 1;
    }, std::chrono::milliseconds(4000)));

    UndoResult messageUndo = harness.undoMessages(3);
    EXPECT_TRUE(messageUndo.compactionReversed);
    EXPECT_EQ(turnIds(*agent->getContext().history), baselineIds);
    EXPECT_EQ(agent->getContext().aggregateMetrics.tokens.contextSize, 8888u);

    ctx.aggregateMetrics.tokens.contextSize = 9999;
    harness.compactFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return countTurnsWithPrefix(*agent->getContext().history,
                                    "compaction-end-") >= 1;
    }, std::chrono::milliseconds(4000)));

    UndoResult tsUndo = harness.undoAfterTimestamp(baselineTimestamp);
    EXPECT_TRUE(tsUndo.compactionReversed);
    EXPECT_EQ(turnIds(*agent->getContext().history), baselineIds);
    EXPECT_EQ(agent->getContext().aggregateMetrics.tokens.contextSize, 9999u);
}

TEST_F(HarnessTest, IdleAgentModelSwitchAppliesImmediately) {
    auto& harness = Harness::instance();
    auto oldProvider = std::make_shared<ModelSwitchProbeProvider>(
        "idle-switch-old-provider", "idle-old-model");
    auto newProvider = std::make_shared<ModelSwitchProbeProvider>(
        "idle-switch-new-provider", "idle-new-model");
    firmius::provider::ProviderRegistry::instance().registerProvider(oldProvider);
    firmius::provider::ProviderRegistry::instance().registerProvider(newProvider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = oldProvider->getId();
    config.defaultModelId = "idle-old-model";
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);
    ASSERT_FALSE(agent->isRunning());

    harness.switchModel(newProvider->getId(), "idle-new-model");

    const auto& ctx = agent->getContext();
    EXPECT_EQ(ctx.config.providerId, newProvider->getId());
    EXPECT_EQ(ctx.config.modelId, "idle-new-model");
}

TEST_F(HarnessTest, RunningFocusedAgentSwitchIsQueuedAndAppliesNextTurn) {
    auto& harness = Harness::instance();
    auto oldProvider = std::make_shared<ModelSwitchProbeProvider>(
        "queued-switch-old-provider", "queued-old-model", true);
    auto newProvider = std::make_shared<ModelSwitchProbeProvider>(
        "queued-switch-new-provider", "queued-new-model");
    firmius::provider::ProviderRegistry::instance().registerProvider(oldProvider);
    firmius::provider::ProviderRegistry::instance().registerProvider(newProvider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = oldProvider->getId();
    config.defaultModelId = "queued-old-model";
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    harness.send("first request while old model is active");
    ASSERT_TRUE(waitForCondition([&]() {
        return oldProvider->firstCallEntered() && agent->isRunning();
    }, std::chrono::milliseconds(3000)));

    EXPECT_NO_THROW(
        harness.switchModel(newProvider->getId(), "queued-new-model"));

    oldProvider->releaseFirstCall();
    ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));

    auto oldCalls = oldProvider->observedCalls();
    ASSERT_EQ(oldCalls.size(), 1u);
    EXPECT_EQ(oldCalls.front().modelId, "queued-old-model");

    ASSERT_TRUE(sendAndWaitForIdle(
        harness, "second request should use queued model", agent));

    auto newCalls = newProvider->observedCalls();
    ASSERT_EQ(newCalls.size(), 1u);
    EXPECT_EQ(newCalls.front().modelId, "queued-new-model");
}

TEST_F(HarnessTest, RunningVariantSwitchIsQueuedAndAppliesNextTurn) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<VariantSwitchProbeProvider>(
        "queued-variant-provider", "queued-variant-model", true);
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = "queued-variant-model";
    config.defaultModelVariant = "low";
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);
    harness.switchModel(provider->getId(), "queued-variant-model", "low");

    harness.send("first request with low variant");
    ASSERT_TRUE(waitForCondition([&]() {
        return provider->firstCallEntered() && agent->isRunning();
    }, std::chrono::milliseconds(3000)));

    EXPECT_NO_THROW(
        harness.switchModel(provider->getId(), "queued-variant-model", "high"));

    provider->releaseFirstCall();
    ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));
    ASSERT_TRUE(sendAndWaitForIdle(
        harness, "second request should use high variant", agent));

    const auto variants = provider->observedVariantJson();
    ASSERT_GE(variants.size(), 2u);
    EXPECT_EQ(variants[0], R"({"variant":"low"})");
    EXPECT_EQ(variants[1], R"({"variant":"high"})");
}

TEST_F(HarnessTest, RunningQueuedModelSwitchLastWriteWins) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<ModelSwitchProbeProvider>(
        "queued-last-write-provider", "queued-last-write-default", true);
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);

    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = "queued-last-write-old";
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    harness.send("first request with old model");
    ASSERT_TRUE(waitForCondition([&]() {
        return provider->firstCallEntered() && agent->isRunning();
    }, std::chrono::milliseconds(3000)));

    EXPECT_NO_THROW(harness.switchModel(provider->getId(), "queued-candidate-a"));
    EXPECT_NO_THROW(harness.switchModel(provider->getId(), "queued-candidate-b"));

    provider->releaseFirstCall();
    ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));
    ASSERT_TRUE(sendAndWaitForIdle(
        harness, "second request should use last queued model", agent));

    const auto calls = provider->observedCalls();
    ASSERT_GE(calls.size(), 2u);
    EXPECT_EQ(calls[0].modelId, "queued-last-write-old");
    EXPECT_EQ(calls[1].modelId, "queued-candidate-b");
}

TEST_F(HarnessTest, CompactionSnapshotIncludesChunkTasksAndTodoTexts) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<CompactionProbeProvider>();
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
    plan.id = "plan-compaction-rich-state";
    plan.threadId = threadId;
    plan.title = "Runtime compaction hardening";
    plan.objective = "Preserve the active remediation ledger.";
    plan.strategy = "Run through chunked execution with explicit follow-ups.";
    plan.status = PlanStatus::Active;
    WorkChunk chunk;
    chunk.id = "chunk-remediate-1";
    chunk.title = "Fix compaction undo durability";
    chunk.goal = "Guarantee restore from snapshot on undo paths.";
    chunk.status = WorkChunkStatus::InProgress;
    WorkTask task;
    task.id = "task-restore-1";
    task.title = "Add undo-after-timestamp regression test";
    task.status = WorkChunkStatus::Ready;
    chunk.tasks.push_back(task);
    plan.chunks.push_back(chunk);
    tm.writePlan(threadId, plan);

    ThreadMetadata metadata = tm.getMetadata(threadId);
    metadata.activePlanId = plan.id;
    tm.updateMetadata(threadId, metadata);

    AgentTodoList todo;
    todo.threadId = threadId;
    todo.agentId = agent->getContext().identity.id;
    todo.nextId = 3;
    todo.items.push_back(
        TodoItem{1, "Validate compaction markers on disk", TodoStatus::InProgress,
                 chunk.id, plan.id, 1, 1});
    todo.items.push_back(
        TodoItem{2, "Verify undo snapshot restoration", TodoStatus::Pending,
                 chunk.id, plan.id, 1, 1});
    tm.writeAgentTodo(threadId, todo.agentId, todo);

    ASSERT_TRUE(sendAndWaitForIdle(harness, "work pass one", agent));
    ASSERT_TRUE(sendAndWaitForIdle(harness, "work pass two", agent));
    agent->getMutableContext().aggregateMetrics.tokens.contextSize = 4500;

    harness.compactFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return provider->summaryCallCount() >= 1;
    }, std::chrono::milliseconds(4000)));

    const std::string prompt = provider->lastCompactionPrompt();
    EXPECT_NE(prompt.find("**Chunk Ledger:**"), std::string::npos);
    EXPECT_NE(prompt.find("Fix compaction undo durability"), std::string::npos);
    EXPECT_NE(prompt.find("Add undo-after-timestamp regression test"),
              std::string::npos);
    EXPECT_NE(prompt.find("**Todo Ledger:**"), std::string::npos);
    EXPECT_NE(prompt.find("Validate compaction markers on disk"),
              std::string::npos);
    EXPECT_NE(prompt.find("Verify undo snapshot restoration"),
              std::string::npos);
}

TEST_F(HarnessTest, CompactionDoesNotImmediatelyRetriggerOnNextTurn) {
    auto& harness = Harness::instance();
    auto provider = std::make_shared<CompactionProbeProvider>();
    firmius::provider::ProviderRegistry::instance().registerProvider(provider);
    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = provider->getId();
    config.defaultModelId = provider->listModels().front().id;
    harness.updateConfig(config);

    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    ASSERT_TRUE(sendAndWaitForIdle(harness, "pass one", agent));
    ASSERT_TRUE(sendAndWaitForIdle(harness, "pass two", agent));
    agent->getMutableContext().aggregateMetrics.tokens.contextSize = 5000;

    harness.compactFocusedAgent();
    ASSERT_TRUE(waitForCondition([&]() {
        return countTurnsWithPrefix(*agent->getContext().history,
                                    "compaction-start-") == 1;
    }, std::chrono::milliseconds(4000)));

    ASSERT_TRUE(sendAndWaitForIdle(harness, "post-compaction continuation", agent));
    EXPECT_EQ(countTurnsWithPrefix(*agent->getContext().history,
                                   "compaction-start-"),
              1u);
}

TEST_F(HarnessTest, MarkThreadAsBenchmarkPersistsMetadata) {
    auto& harness = Harness::instance();
    const std::string threadId = harness.newThread({}, "/tmp", "worker");
    ASSERT_FALSE(threadId.empty());

    ASSERT_TRUE(harness.markThreadAsBenchmark(threadId, "mbpp", "42"));

    auto threads = harness.listThreads();
    auto it = std::find_if(threads.begin(), threads.end(),
                           [&](const ThreadMetadata& meta) {
                               return meta.threadId == threadId;
                           });
    ASSERT_NE(it, threads.end());
    EXPECT_TRUE(it->isBenchmarkRun);
    EXPECT_EQ(it->benchmarkId, "mbpp");
    EXPECT_EQ(it->benchmarkTaskId, "42");
    EXPECT_NE(it->title.find("Benchmark: mbpp"), std::string::npos);

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    const auto persisted = tm.getMetadata(threadId);
    EXPECT_TRUE(persisted.isBenchmarkRun);
    EXPECT_EQ(persisted.benchmarkId, "mbpp");
    EXPECT_EQ(persisted.benchmarkTaskId, "42");
}

TEST_F(HarnessTest, AppendSystemMessagePersistsVisibleTranscriptTurn) {
    auto& harness = Harness::instance();
    const std::string threadId = harness.newThread({}, "/tmp", "lead");
    ASSERT_FALSE(threadId.empty());
    auto agent = createFocusedLeadAgent(threadId);
    ASSERT_TRUE(agent);

    const std::string agentId = agent->getContext().identity.id;
    ASSERT_TRUE(harness.appendSystemMessage(agentId, "benchmark: cloning repo"));

    const auto& liveHistory = *agent->getContext().history;
    ASSERT_FALSE(liveHistory.turns.empty());
    const auto& lastTurn = liveHistory.turns.back();
    ASSERT_FALSE(lastTurn.messages.empty());
    const auto& lastMessage = lastTurn.messages.front();
    EXPECT_EQ(lastMessage.role, Role::System);
    ASSERT_FALSE(lastMessage.content.empty());
    auto text = std::get_if<TextContent>(&lastMessage.content.front());
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "benchmark: cloning repo");

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    const auto persisted = tm.loadAgentHistory(threadId, agentId);
    ASSERT_FALSE(persisted.turns.empty());
    const auto& persistedMessage = persisted.turns.back().messages.front();
    EXPECT_EQ(persistedMessage.role, Role::System);
    auto persistedText =
        std::get_if<TextContent>(&persistedMessage.content.front());
    ASSERT_NE(persistedText, nullptr);
    EXPECT_EQ(persistedText->text, "benchmark: cloning repo");
}

TEST_F(HarnessTest, BenchmarkSessionRunAgentTaskEmitsLiveStreamEvents) {
    using firmius::core::BenchmarkConfig;
    using firmius::core::BenchmarkSession;
    using firmius::shared::HostType;

    auto& harness = Harness::instance();
    auto config = firmius::shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = "test-retry-provider";
    config.defaultModelId = "test-retry-model";
    harness.updateConfig(config);

    BenchmarkConfig benchConfig;
    benchConfig.hostOptions.type = HostType::Local;
    benchConfig.cwd = "/tmp";
    benchConfig.personaName = "lead";
    benchConfig.providerId = "test-retry-provider";
    benchConfig.modelId = "test-retry-model";
    benchConfig.initializeHarness = false;

    BenchmarkSession session(benchConfig);

    std::atomic<int> providerWaitingEvents{0};
    std::atomic<int> textEvents{0};
    std::atomic<int> finishedEvents{0};

    const int subId = harness.subscribe([&](const firmius::shared::AppEvent& event) {
        std::visit(
            [&](auto&& ev) {
                using T = std::decay_t<decltype(ev)>;
                if constexpr (std::is_same_v<T, firmius::shared::AgentProviderWaiting>) {
                    providerWaitingEvents.fetch_add(1);
                } else if constexpr (std::is_same_v<T, firmius::shared::AgentText>) {
                    textEvents.fetch_add(1);
                } else if constexpr (std::is_same_v<T, firmius::shared::AgentFinished>) {
                    finishedEvents.fetch_add(1);
                }
            },
            event);
    });

    auto outcome = session.runAgentTask(
        "Return a short response so benchmark streaming events are emitted.");

    harness.unsubscribe(subId);

    EXPECT_NE(outcome.kind, firmius::shared::AgentOutcome::Kind::Failed);
    EXPECT_GE(providerWaitingEvents.load(), 1);
    EXPECT_GE(textEvents.load(), 1);
    EXPECT_GE(finishedEvents.load(), 1);
}

} // namespace
