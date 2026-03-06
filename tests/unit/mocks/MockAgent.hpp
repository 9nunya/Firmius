#ifndef FIRMIUS_TEST_MOCK_AGENT_HPP
#define FIRMIUS_TEST_MOCK_AGENT_HPP

#include "IAgent.hpp"
#include "MockHost.hpp"
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

/**
 * @brief Records a single call to an IAgent method.
 */
struct MockAgentCall {
    std::string method;
    std::map<std::string, std::string> params;
    std::chrono::steady_clock::time_point timestamp;
};

/**
 * @brief Mock implementation of IAgent for unit testing.
 * 
 * Provides controllable agent behavior with configurable context,
 * process tracking, and call recording for verification.
 */
class MockAgent : public IAgent {
public:
    /**
     * @brief Constructs a MockAgent with an optional context.
     * @param context The agent context (will use default if not provided).
     * @param host The host to use (will create MockHost if not provided).
     */
    explicit MockAgent(const AgentContext& context = {},
                       std::shared_ptr<MockHost> host = nullptr)
        : context_(context)
        , host_(host ? host : std::make_shared<MockHost>())
        , interrupted_(false)
        , nextProcessId_(1) {
        if (!context_.history) {
            context_.history = std::make_shared<AgentHistory>();
        }
    }

    /**
     * @brief Resets the agent's history and state.
     */
    void reset() override {
        recordCall("reset", {});
        context_.history->turns.clear();
        context_.state = AgentState();
        interrupted_ = false;
    }

    /**
     * @brief Runs the agent on a specific task.
     * @param task The task description.
     * @param onEvent Callback for real-time stream events.
     */
    void run(const std::string& task, std::function<void(const StreamEvent&)> onEvent) override {
        recordCall("run", {{"task", task}});
        context_.state.currentStatus = AgentStatus::Streaming;
        
        if (onEvent) {
            StreamDone done;
            done.reason = StopReason::Stop;
            onEvent(done);
        }
        
        context_.state.currentStatus = AgentStatus::Idle;
    }

    /**
     * @brief Gets the current agent context (read-only).
     * @return The agent context.
     */
    const AgentContext& getContext() const override {
        return context_;
    }

    /**
     * @brief Gets the current agent context (mutable).
     * @return The agent context.
     */
    AgentContext& getMutableContext() override {
        recordCall("getMutableContext", {});
        return context_;
    }

    /**
     * @brief Resolves a path relative to the agent's CWD.
     * @param path The path to resolve.
     * @return An absolute, normalized path.
     */
    std::string resolvePath(const std::string& path) const override {
        const_cast<MockAgent*>(this)->recordCall("resolvePath", {{"path", path}});
        return FSUtil::resolvePath(path, context_.environment.cwd);
    }

    /**
     * @brief Interrupts the current agent execution.
     */
    void interrupt() override {
        recordCall("interrupt", {});
        interrupted_ = true;
        context_.state.currentStatus = AgentStatus::Cancelled;
    }

    /**
     * @brief Checks if the agent has been interrupted.
     * @return True if interrupt() has been called.
     */
    bool isInterrupted() const override {
        return interrupted_;
    }

    /**
     * @brief Spawns a background process.
     * @param command The command to spawn.
     * @param cwd Optional working directory.
     * @param env Optional environment variables.
     * @return A unique process ID.
     */
    std::string spawnProcess(const std::string& command,
                             const std::string& cwd = "",
                             const std::map<std::string, std::string>& env = {}) override {
        (void)env;
        recordCall("spawnProcess", {
            {"command", command},
            {"cwd", cwd}
        });
        
        std::string id = generateProcessId();
        
        auto it = spawnResults_.find(command);
        if (it != spawnResults_.end()) {
            processResults_[id] = it->second;
        }
        
        context_.state.ownedProcesses.push_back(id);
        return id;
    }

    /**
     * @brief Inspects a background process.
     * @param id The process ID.
     * @return A snapshot of the process state.
     */
    ProcessSnapshot inspectProcess(const std::string& id) override {
        recordCall("inspectProcess", {{"id", id}});
        
        auto it = processResults_.find(id);
        if (it != processResults_.end()) {
            ProcessSnapshot snapshot;
            snapshot.running = false;
            snapshot.exitCode = it->second.exitCode;
            snapshot.stdoutData = it->second.stdoutData;
            snapshot.stderrData = it->second.stderrData;
            snapshot.elapsedMs = 0;
            return snapshot;
        }
        
        ProcessSnapshot snapshot;
        snapshot.running = false;
        snapshot.exitCode = -1;
        snapshot.elapsedMs = 0;
        return snapshot;
    }

    /**
     * @brief Writes data to a background process's stdin.
     * @param id The process ID.
     * @param data The data to write.
     */
    void writeToProcess(const std::string& id, const std::string& data) override {
        recordCall("writeToProcess", {{"id", id}});
        processInputs_[id].push_back(data);
    }

    /**
     * @brief Registers a process ID (for internal tracking).
     * @param id The process ID to register.
     */
    void registerProcessId(const std::string& id) override {
        recordCall("registerProcessId", {{"id", id}});
        registeredProcessIds_.insert(id);
    }

    /**
     * @brief Adds a process ID to the blocking list.
     * @param id The process ID to add.
     */
    void addBlockingProcessId(const std::string& id) override {
        recordCall("addBlockingProcessId", {{"id", id}});
        blockingProcessIds_.push_back(id);
    }

    /**
     * @brief Removes a process ID from the blocking list.
     * @param id The process ID to remove.
     */
    void removeBlockingProcessId(const std::string& id) override {
        recordCall("removeBlockingProcessId", {{"id", id}});
        auto it = std::find(blockingProcessIds_.begin(), blockingProcessIds_.end(), id);
        if (it != blockingProcessIds_.end()) {
            blockingProcessIds_.erase(it);
        }
    }

    /**
     * @brief Gets all blocking process IDs.
     * @return Vector of blocking process IDs.
     */
    std::vector<std::string> getBlockingProcessIds() override {
        return blockingProcessIds_;
    }

    /**
     * @brief Checks if a file has been read in the current session.
     * @param path The absolute path to the file.
     * @return True if the file has been read.
     */
    bool hasReadFile(const std::string& path) const override {
        return readFiles_.find(path) != readFiles_.end();
    }

    /**
     * @brief Marks a file as having been read in the current session.
     * @param path The absolute path to the file.
     */
    void markFileAsRead(const std::string& path) override {
        recordCall("markFileAsRead", {{"path", path}});
        readFiles_.insert(path);
        context_.state.readFiles.push_back(path);
    }

    /**
     * @brief Returns the agent's host.
     */
    std::shared_ptr<IHost> getHost() override {
        return host_;
    }

    /**
     * @brief Sets a field in the agent context.
     * @param field The field name (identity.id, environment.cwd, etc.).
     * @param value The value to set.
     */
    void setContextField(const std::string& field, const std::string& value) {
        if (field == "identity.id") {
            context_.identity.id = value;
        } else if (field == "identity.name") {
            context_.identity.name = value;
        } else if (field == "identity.role") {
            context_.identity.role = value;
        } else if (field == "identity.goal") {
            context_.identity.goal = value;
        } else if (field == "identity.systemPrompt") {
            context_.identity.systemPrompt = value;
        } else if (field == "identity.parentId") {
            context_.identity.parentId = value;
        } else if (field == "environment.cwd") {
            context_.environment.cwd = value;
        } else if (field == "environment.identifier") {
            context_.environment.identifier = value;
        } else if (field == "history.threadId") {
            context_.history->threadId = value;
        }
    }

    /**
     * @brief Sets the agent context directly.
     * @param context The new context.
     */
    void setContext(const AgentContext& context) {
        context_ = context;
    }

    /**
     * @brief Configures the result for a spawned process.
     * @param command The command pattern to match.
     * @param exitCode The exit code.
     * @param stdout The stdout output.
     * @param stderr The stderr output.
     */
    void setSpawnResult(const std::string& command,
                        int exitCode,
                        const std::string& stdout = "",
                        const std::string& stderr = "") {
        ProcessResult result;
        result.exitCode = exitCode;
        result.stdoutData = stdout;
        result.stderrData = stderr;
        spawnResults_[command] = result;
    }

    /**
     * @brief Sets the host for the agent.
     * @param host The host to use.
     */
    void setHost(std::shared_ptr<MockHost> host) {
        host_ = host;
    }

    /**
     * @brief Gets all recorded calls.
     * @return Vector of recorded calls.
     */
    const std::vector<MockAgentCall>& getCalls() const {
        return calls_;
    }

    /**
     * @brief Clears all recorded calls.
     */
    void clearCalls() {
        calls_.clear();
    }

    /**
     * @brief Checks if a method was called.
     * @param method The method name.
     * @return True if the method was called at least once.
     */
    bool wasMethodCalled(const std::string& method) const {
        return std::any_of(calls_.begin(), calls_.end(),
            [&method](const MockAgentCall& call) { return call.method == method; });
    }

    /**
     * @brief Checks if a method was called with specific parameters.
     * @param method The method name.
     * @param params The parameters to match.
     * @return True if a matching call was found.
     */
    bool wasCalledWith(const std::string& method,
                       const std::map<std::string, std::string>& params) const {
        for (const auto& call : calls_) {
            if (call.method == method) {
                bool matches = true;
                for (const auto& [key, value] : params) {
                    auto it = call.params.find(key);
                    if (it == call.params.end() || it->second != value) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * @brief Gets the number of times a method was called.
     * @param method The method name.
     * @return The call count.
     */
    size_t getCallCount(const std::string& method) const {
        return std::count_if(calls_.begin(), calls_.end(),
            [&method](const MockAgentCall& call) { return call.method == method; });
    }

    /**
     * @brief Gets the mock host for direct manipulation.
     * @return The mock host.
     */
    std::shared_ptr<MockHost> getMockHost() const {
        return host_;
    }

    /**
     * @brief Gets all data written to a process.
     * @param id The process ID.
     * @return Vector of written data strings.
     */
    const std::vector<std::string>& getProcessInputs(const std::string& id) const {
        static const std::vector<std::string> empty;
        auto it = processInputs_.find(id);
        if (it != processInputs_.end()) {
            return it->second;
        }
        return empty;
    }

private:
    AgentContext context_;
    std::shared_ptr<MockHost> host_;
    bool interrupted_;
    int nextProcessId_;
    std::vector<MockAgentCall> calls_;
    std::map<std::string, ProcessResult> spawnResults_;
    std::map<std::string, ProcessResult> processResults_;
    std::map<std::string, std::vector<std::string>> processInputs_;
    std::set<std::string> readFiles_;
    std::set<std::string> registeredProcessIds_;
    std::vector<std::string> blockingProcessIds_;

    std::string generateProcessId() {
        return "mock-process-" + std::to_string(nextProcessId_++);
    }

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
