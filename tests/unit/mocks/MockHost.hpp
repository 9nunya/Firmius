#ifndef FIRMIUS_TEST_MOCK_HOST_HPP
#define FIRMIUS_TEST_MOCK_HOST_HPP

#include "IHost.hpp"
#include "MockHostProcess.hpp"
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <optional>
#include <chrono>
#include <regex>
#include <stdexcept>

namespace firmius::test {

using namespace firmius::shared;

/**
 * @brief Configuration for controlling MockHost behavior.
 */
struct MockHostConfig {
    std::string hostId = "mock-host-1";
    std::string user = "test-user";
    int defaultExitCode = 0;
    std::string defaultStdout;
    std::string defaultStderr;
    double defaultDurationMs = 0.0;
    std::map<std::string, std::vector<uint8_t>> fileContents;
    std::map<std::string, FileInfo> fileInfos;
    std::vector<std::string> existingPaths;
    std::vector<std::string> pathsToThrowOnRead;
    std::vector<std::string> pathsToThrowOnWrite;
    std::vector<std::string> pathsToThrowOnList;
};

/**
 * @brief Records a single call to an IHost method.
 */
struct MockHostCall {
    std::string method;
    std::map<std::string, std::string> params;
    std::chrono::steady_clock::time_point timestamp;
};

/**
 * @brief Mock implementation of IHost for unit testing.
 * 
 * Provides controllable host behavior with configurable return values,
 * file system simulation, and call recording for verification.
 */
class MockHost : public IHost {
public:
    /**
     * @brief Constructs a MockHost with the given configuration.
     * @param config The configuration controlling mock behavior.
     */
    explicit MockHost(const MockHostConfig& config = {})
        : config_(config)
        , initialized_(false)
        , nextProcessId_(1) {}

    /**
     * @brief Performs environment-specific initialization.
     */
    std::string init() override {
        recordCall("init", {});
        initialized_ = true;
        return config_.hostId;
    }

    /**
     * @brief Performs environment-specific cleanup.
     */
    void destroy() override {
        recordCall("destroy", {});
        initialized_ = false;
    }

    /**
     * @brief Performs full cleanup of all resources.
     */
    void cleanup() override {
        recordCall("cleanup", {});
        backgroundProcesses_.clear();
        completedSnapshots_.clear();
    }

    /**
     * @brief Sets the user context for command execution.
     * @param user Username (e.g., "root", "jack").
     */
    void setUser(const std::string& user) override {
        recordCall("setUser", {{"user", user}});
        config_.user = user;
    }

    /**
     * @brief Safely reads a file from the host filesystem.
     * @param path Absolute path to the file.
     * @return Binary data content.
     * @throws std::runtime_error if read fails or path is configured to throw.
     */
    std::vector<uint8_t> readFile(const std::string& path) override {
        recordCall("readFile", {{"path", path}});
        
        if (shouldThrowOnRead(path)) {
            throw std::runtime_error("Mock readFile error for path: " + path);
        }
        
        auto it = config_.fileContents.find(path);
        if (it != config_.fileContents.end()) {
            return it->second;
        }
        
        throw std::runtime_error("File not found: " + path);
    }

    /**
     * @brief Safely writes a file to the host filesystem.
     * @param path Absolute path to the file.
     * @param data Binary data to write.
     * @throws std::runtime_error if write fails or path is configured to throw.
     */
    void writeFile(const std::string& path, const std::vector<uint8_t>& data) override {
        recordCall("writeFile", {{"path", path}});
        
        if (shouldThrowOnWrite(path)) {
            throw std::runtime_error("Mock writeFile error for path: " + path);
        }
        
        config_.fileContents[path] = data;
        
        // Update file info
        FileInfo info;
        info.name = path.substr(path.find_last_of('/') + 1);
        info.path = path;
        info.size = data.size();
        info.isDirectory = false;
        info.isSymlink = false;
        info.modifiedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        config_.fileInfos[path] = info;
        
        // Mark as existing
        if (std::find(config_.existingPaths.begin(), config_.existingPaths.end(), path) 
            == config_.existingPaths.end()) {
            config_.existingPaths.push_back(path);
        }
    }

    /**
     * @brief Checks if a path exists on the host.
     * @param path The path to check.
     * @return True if it exists.
     */
    bool exists(const std::string& path) override {
        recordCall("exists", {{"path", path}});
        
        return std::find(config_.existingPaths.begin(), config_.existingPaths.end(), path) 
               != config_.existingPaths.end();
    }

    /**
     * @brief Lists entries in a directory.
     * @param path Absolute path to the directory.
     * @return Vector of FileInfo for each entry.
     * @throws std::runtime_error if path is not a directory or access fails.
     */
    std::vector<FileInfo> listDir(const std::string& path) override {
        recordCall("listDir", {{"path", path}});
        
        if (shouldThrowOnList(path)) {
            throw std::runtime_error("Mock listDir error for path: " + path);
        }
        
        std::vector<FileInfo> entries;
        
        for (const auto& [filePath, info] : config_.fileInfos) {
            // Check if this file is directly inside the given directory
            if (filePath.find(path) == 0 && filePath != path) {
                std::string relative = filePath.substr(path.length());
                if (relative.front() == '/') {
                    relative = relative.substr(1);
                }
                // Only direct children (no slashes in relative path)
                if (relative.find('/') == std::string::npos) {
                    entries.push_back(info);
                }
            }
        }
        
        return entries;
    }

    /**
     * @brief Gets metadata about a single file or directory.
     * @param path Absolute path.
     * @return FileInfo for the entry.
     * @throws std::runtime_error if path does not exist or access fails.
     */
    FileInfo stat(const std::string& path) override {
        recordCall("stat", {{"path", path}});
        
        auto it = config_.fileInfos.find(path);
        if (it != config_.fileInfos.end()) {
            return it->second;
        }
        
        throw std::runtime_error("Path not found: " + path);
    }

    /**
     * @brief Gets a unique identifier for this host.
     */
    std::string getId() const override {
        return config_.hostId;
    }

    /**
     * @brief Executes a command synchronously.
     * @param command The shell command string.
     * @param cwd Optional working directory.
     * @param env Optional environment variables.
     * @param timeout Optional timeout for the execution.
     * @return Result of the execution.
     */
    ProcessResult exec(const std::string& command,
                       const std::string& cwd = "",
                       const std::map<std::string, std::string>& /*env*/ = {},
                       std::optional<std::chrono::milliseconds> /*timeout*/ = std::nullopt) override {
        recordCall("exec", {
            {"command", command},
            {"cwd", cwd}
        });
        
        // Look for a configured result for this command pattern
        auto it = execResults_.find(command);
        if (it != execResults_.end()) {
            return it->second;
        }
        
        // Check regex patterns
        for (const auto& [pattern, result] : execPatternResults_) {
            std::regex re(pattern);
            if (std::regex_search(command, re)) {
                return result;
            }
        }
        
        // Return default
        ProcessResult result;
        result.exitCode = config_.defaultExitCode;
        result.stdoutData = config_.defaultStdout;
        result.stderrData = config_.defaultStderr;
        result.durationMs = config_.defaultDurationMs;
        result.finishReason = ProcessFinishReason::Natural;
        return result;
    }

    /**
     * @brief Spawns a command asynchronously.
     * @param command The shell command string.
     * @param cwd Optional working directory.
     * @param env Optional environment variables.
     * @return A handle to the spawned process.
     */
    std::unique_ptr<IHostProcess> spawn(const std::string& command,
                                          const std::string& cwd = "",
                                          const std::map<std::string, std::string>& /*env*/ = {}) override {
        recordCall("spawn", {
            {"command", command},
            {"cwd", cwd}
        });
        
        // Look for a configured mock process
        auto it = spawnConfigs_.find(command);
        if (it != spawnConfigs_.end()) {
            return std::make_unique<MockHostProcess>(it->second);
        }
        
        // Create a default mock process
        MockHostProcessConfig defaultConfig;
        defaultConfig.systemId = "mock-process-" + std::to_string(nextProcessId_++);
        defaultConfig.running = true;
        return std::make_unique<MockHostProcess>(defaultConfig);
    }

    /**
     * @brief Registers a process in the host's background process tracking.
     * @param id The unique process ID to assign.
     * @param proc The process handle to register.
     */
    void registerBackgroundProcess(const std::string& id, std::unique_ptr<IHostProcess> proc) override {
        recordCall("registerBackgroundProcess", {{"id", id}});
        completedSnapshots_.erase(id);
        backgroundProcesses_[id] = std::move(proc);
    }

    /**
     * @brief Inspects a background process.
     * @param id The process ID.
     * @return A snapshot of the process state.
     */
    ProcessSnapshot inspectBackgroundProcess(const std::string& id) override {
        recordCall("inspectBackgroundProcess", {{"id", id}});
        
        auto it = backgroundProcesses_.find(id);
        if (it != backgroundProcesses_.end()) {
            auto snapshot = it->second->inspect();
            if (!snapshot.running) {
                completedSnapshots_[id] = snapshot;
            }
            return snapshot;
        }
        
        auto completedIt = completedSnapshots_.find(id);
        if (completedIt != completedSnapshots_.end()) {
            return completedIt->second;
        }
        
        ProcessSnapshot snapshot;
        snapshot.running = false;
        snapshot.exitCode = -1;
        snapshot.elapsedMs = 0;
        return snapshot;
    }

    void releaseBackgroundProcess(const std::string& id) override {
        recordCall("releaseBackgroundProcess", {{"id", id}});
        auto it = backgroundProcesses_.find(id);
        if (it != backgroundProcesses_.end()) {
            completedSnapshots_[id] = it->second->inspect();
            backgroundProcesses_.erase(it);
        }
    }

    /**
     * @brief Writes data to a background process's stdin.
     * @param id The process ID.
     * @param data The data to write.
     */
    void writeToBackgroundProcess(const std::string& id, const std::string& data) override {
        recordCall("writeToBackgroundProcess", {{"id", id}});
        
        auto it = backgroundProcesses_.find(id);
        if (it != backgroundProcesses_.end()) {
            it->second->write(data);
        }
    }

    /**
     * @brief Forcefully kills a background process.
     * @param id The process ID.
     */
    void killBackgroundProcess(const std::string& id) override {
        recordCall("killBackgroundProcess", {{"id", id}});
        
        auto it = backgroundProcesses_.find(id);
        if (it != backgroundProcesses_.end()) {
            it->second->kill();
        }
    }

    /**
     * @brief Configures the result for a specific command.
     * @param command The exact command to match.
     * @param exitCode The exit code to return.
     * @param stdout The stdout data.
     * @param stderr The stderr data.
     */
    void setExecResult(const std::string& command, 
                       int exitCode, 
                       const std::string& stdout = "",
                       const std::string& stderr = "") {
        ProcessResult result;
        result.exitCode = exitCode;
        result.stdoutData = stdout;
        result.stderrData = stderr;
        result.durationMs = 0;
        result.finishReason = ProcessFinishReason::Natural;
        execResults_[command] = result;
    }

    /**
     * @brief Configures a default exec result for any unmatched command.
     * @param exitCode The exit code to return.
     * @param stdout The stdout data.
     * @param stderr The stderr data.
     */
    void setDefaultExecResult(int exitCode,
                              const std::string& stdout = "",
                              const std::string& stderr = "") {
        config_.defaultExitCode = exitCode;
        config_.defaultStdout = stdout;
        config_.defaultStderr = stderr;
    }

    /**
     * @brief Configures a result for commands matching a regex pattern.
     * @param pattern The regex pattern to match.
     * @param exitCode The exit code to return.
     * @param stdout The stdout data.
     * @param stderr The stderr data.
     */
    void setExecResultPattern(const std::string& pattern,
                              int exitCode,
                              const std::string& stdout = "",
                              const std::string& stderr = "") {
        ProcessResult result;
        result.exitCode = exitCode;
        result.stdoutData = stdout;
        result.stderrData = stderr;
        result.durationMs = 0;
        result.finishReason = ProcessFinishReason::Natural;
        execPatternResults_[pattern] = result;
    }

    /**
     * @brief Configures the mock process returned by spawn().
     * @param command The exact command to match.
     * @param processConfig The configuration for the returned MockHostProcess.
     */
    void setSpawnResult(const std::string& command, const MockHostProcessConfig& processConfig) {
        spawnConfigs_[command] = processConfig;
    }

    /**
     * @brief Adds a file to the mock filesystem.
     * @param path The file path.
     * @param content The file content.
     * @param isDirectory Whether this is a directory.
     */
    void addMockFile(const std::string& path, 
                     const std::vector<uint8_t>& content,
                     bool isDirectory = false) {
        config_.fileContents[path] = content;
        config_.existingPaths.push_back(path);
        
        FileInfo info;
        info.name = path.substr(path.find_last_of('/') + 1);
        info.path = path;
        info.size = content.size();
        info.isDirectory = isDirectory;
        info.isSymlink = false;
        info.modifiedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        config_.fileInfos[path] = info;
    }

    /**
     * @brief Adds a text file to the mock filesystem.
     * @param path The file path.
     * @param content The text content.
     * @param isDirectory Whether this is a directory.
     */
    void addMockFile(const std::string& path, 
                     const std::string& content,
                     bool isDirectory = false) {
        std::vector<uint8_t> data(content.begin(), content.end());
        addMockFile(path, data, isDirectory);
    }

    /**
     * @brief Configures a path to throw an exception on readFile().
     * @param path The path to throw on.
     */
    void throwOnRead(const std::string& path) {
        config_.pathsToThrowOnRead.push_back(path);
    }

    /**
     * @brief Configures a path to throw an exception on writeFile().
     * @param path The path to throw on.
     */
    void throwOnWrite(const std::string& path) {
        config_.pathsToThrowOnWrite.push_back(path);
    }

    /**
     * @brief Configures a path to throw an exception on listDir().
     * @param path The path to throw on.
     */
    void throwOnList(const std::string& path) {
        config_.pathsToThrowOnList.push_back(path);
    }

    /**
     * @brief Gets all recorded calls.
     * @return Vector of recorded calls.
     */
    const std::vector<MockHostCall>& getCalls() const {
        return calls_;
    }

    /**
     * @brief Clears all recorded calls.
     */
    void clearCalls() {
        calls_.clear();
    }

    /**
     * @brief Checks if a method was called with parameters matching a pattern.
     * @param method The method name to check.
     * @param paramPattern A map of parameter key-value pairs to match.
     * @return True if a matching call was found.
     */
    bool wasCalledWith(const std::string& method,
                       const std::map<std::string, std::string>& paramPattern) const {
        for (const auto& call : calls_) {
            if (call.method == method) {
                bool matches = true;
                for (const auto& [key, value] : paramPattern) {
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
     * @brief Checks if exec() was called with a command matching a pattern.
     * @param commandPattern The regex pattern to match against commands.
     * @return True if a matching call was found.
     */
    bool expectExecCalledWith(const std::string& commandPattern) const {
        std::regex re(commandPattern);
        for (const auto& call : calls_) {
            if (call.method == "exec") {
                auto it = call.params.find("command");
                if (it != call.params.end() && std::regex_search(it->second, re)) {
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
            [&method](const MockHostCall& call) { return call.method == method; });
    }

    /**
     * @brief Checks if the host has been initialized.
     * @return True if init() was called.
     */
    bool isInitialized() const {
        return initialized_;
    }

private:
    MockHostConfig config_;
    bool initialized_;
    int nextProcessId_;
    std::vector<MockHostCall> calls_;
    std::map<std::string, ProcessResult> execResults_;
    std::map<std::string, ProcessResult> execPatternResults_;
    std::map<std::string, MockHostProcessConfig> spawnConfigs_;
    std::map<std::string, std::unique_ptr<IHostProcess>> backgroundProcesses_;
    std::map<std::string, ProcessSnapshot> completedSnapshots_;

    void recordCall(const std::string& method, const std::map<std::string, std::string>& params) {
        MockHostCall call;
        call.method = method;
        call.params = params;
        call.timestamp = std::chrono::steady_clock::now();
        calls_.push_back(call);
    }

    bool shouldThrowOnRead(const std::string& path) const {
        return std::find(config_.pathsToThrowOnRead.begin(), 
                        config_.pathsToThrowOnRead.end(), path) 
               != config_.pathsToThrowOnRead.end();
    }

    bool shouldThrowOnWrite(const std::string& path) const {
        return std::find(config_.pathsToThrowOnWrite.begin(), 
                        config_.pathsToThrowOnWrite.end(), path) 
               != config_.pathsToThrowOnWrite.end();
    }

    bool shouldThrowOnList(const std::string& path) const {
        return std::find(config_.pathsToThrowOnList.begin(), 
                        config_.pathsToThrowOnList.end(), path) 
               != config_.pathsToThrowOnList.end();
    }
};

} // namespace firmius::test

#endif // FIRMIUS_TEST_MOCK_HOST_HPP
