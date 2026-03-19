#ifndef FIRMIUS_TEST_MOCK_AGENT_HPP
#define FIRMIUS_TEST_MOCK_AGENT_HPP

#include "IAgent.hpp"
#include "MockEnvironment.hpp"
#include "MockPermissions.hpp"
#include "utils/FSUtil.hpp"
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <set>
#include <chrono>

namespace firmius::test {

using namespace firmius::shared;

struct MockAgentCall {
    std::string method;
    std::map<std::string, std::string> params;
    std::chrono::steady_clock::time_point timestamp;
};

class MockAgent : public IAgent {
public:
    explicit MockAgent(const AgentContext& context = {},
                       std::shared_ptr<MockEnvironment> env = nullptr,
                       std::shared_ptr<MockPermissions> perms = nullptr)
        : context_(context)
        , environment_(env ? env : std::make_shared<MockEnvironment>())
        , permissions_(perms ? perms : std::make_shared<MockPermissions>())
        , interrupted_(false)
        , booting_(false) {
        if (!context_.history) {
            context_.history = std::make_shared<AgentHistory>();
        }
    }

    void reset() override {
        recordCall("reset", {});
        context_.history->turns.clear();
        context_.state = AgentState();
        interrupted_ = false;
        booting_ = false;
    }

    void run(const std::string& task, std::function<void(const StreamEvent&)> onEvent,
             const std::vector<ImageContent>& /*images*/ = {}) override {
        recordCall("run", {{"task", task}});
        context_.state.currentStatus = AgentStatus::Streaming;
        
        if (onEvent) {
            StreamDone done;
            done.reason = StopReason::Stop;
            onEvent(done);
        }
        
        context_.state.currentStatus = AgentStatus::Idle;
    }

    void resume(std::function<void(const StreamEvent&)> onEvent) override {
        recordCall("resume", {});
        context_.state.currentStatus = AgentStatus::Streaming;

        if (onEvent) {
            StreamDone done;
            done.reason = StopReason::Stop;
            onEvent(done);
        }

        context_.state.currentStatus = AgentStatus::Idle;
    }

    void interrupt() override {
        recordCall("interrupt", {});
        interrupted_ = true;
        context_.state.currentStatus = AgentStatus::Cancelled;
    }

    bool isInterrupted() const override {
        return interrupted_;
    }

    void clearInterrupt() override {
        interrupted_ = false;
    }

    void setModel(const std::string& providerId, const std::string& modelId) override {
        setModel(providerId, modelId, "");
    }

    void setModel(const std::string& providerId, const std::string& modelId,
                  const std::string& variantName) override {
        (void)variantName;
        recordCall("setModel", {{"providerId", providerId}, {"modelId", modelId}});
        context_.config.providerId = providerId;
        context_.config.modelId = modelId;
    }

    bool isRunning() const override {
        return context_.state.currentStatus != AgentStatus::Idle;
    }

    bool isBooting() const override {
        return booting_;
    }

    void setBooting(bool b) override {
        booting_ = b;
    }

    const AgentContext& getContext() const override {
        return context_;
    }

    AgentContext& getMutableContext() override {
        recordCall("getMutableContext", {});
        return context_;
    }

    void compactNow(std::function<void(const StreamEvent&)> /*onEvent*/) override {
        recordCall("compactNow", {});
    }

    void saveHistory() override {
        recordCall("saveHistory", {});
    }

    std::shared_ptr<IEnvironment> getEnvironment() const override {
        return environment_;
    }

    std::shared_ptr<IPermissions> getPermissions() const override {
        return permissions_;
    }

    std::shared_ptr<IHost> getHost() override {
        return environment_->getHost();
    }

    MockEnvironment& mockEnvironment() {
        return *environment_;
    }

    MockPermissions& mockPermissions() {
        return *permissions_;
    }

    const std::vector<MockAgentCall>& getCalls() const {
        return calls_;
    }

    void clearCalls() {
        calls_.clear();
    }

    bool wasMethodCalled(const std::string& method) const {
        return std::any_of(calls_.begin(), calls_.end(),
            [&method](const MockAgentCall& call) { return call.method == method; });
    }

    size_t getCallCount(const std::string& method) const {
        return std::count_if(calls_.begin(), calls_.end(),
            [&method](const MockAgentCall& call) { return call.method == method; });
    }

private:
    AgentContext context_;
    std::shared_ptr<MockEnvironment> environment_;
    std::shared_ptr<MockPermissions> permissions_;
    bool interrupted_;
    bool booting_;
    std::vector<MockAgentCall> calls_;

    void recordCall(const std::string& method, const std::map<std::string, std::string>& params) {
        MockAgentCall call;
        call.method = method;
        call.params = params;
        call.timestamp = std::chrono::steady_clock::now();
        calls_.push_back(call);
    }
};

} // namespace firmius::test

#endif // FIRMIUS_TEST_MOCK_AGENT_HPP
