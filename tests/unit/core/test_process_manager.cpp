#include <gtest/gtest.h>

#include "environment/ProcessManager.hpp"
#include "mocks/MockHost.hpp"
#include "mocks/MockHostProcess.hpp"
#include "hosts/LocalHost.hpp"

#include <chrono>
#include <thread>
#include <variant>
#include <vector>
#include <map>
#include <optional>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::test;

namespace {

const ProcessOutputDelta* findFinishedDelta(const std::vector<StreamEvent>& events,
                                            const std::string& processId) {
    for (const auto& event : events) {
        if (const auto* delta = std::get_if<ProcessOutputDelta>(&event)) {
            if (delta->processId == processId && delta->finished) {
                return delta;
            }
        }
    }
    return nullptr;
}

class ImmediateOutputProcess final : public IHostProcess {
public:
    void onOutput(std::function<void(const std::string&, bool)> callback) override {
        callback_ = std::move(callback);
        if (callback_) {
            callback_("booting\n", false);
        }
    }

    ProcessResult wait() override {
        running_ = false;
        ProcessResult result;
        result.exitCode = 0;
        result.finishReason = ProcessFinishReason::Natural;
        return result;
    }

    ProcessSnapshot inspect() const override {
        ProcessSnapshot snapshot;
        snapshot.running = running_;
        snapshot.exitCode = 0;
        return snapshot;
    }

    void kill() override { running_ = false; }
    void write(const std::string&) override {}
    bool isRunning() override { return running_; }
    std::string getSystemId() const override { return "immediate"; }

private:
    bool running_ = true;
    std::function<void(const std::string&, bool)> callback_;
};

class ImmediateOutputHost final : public MockHost {
public:
    std::unique_ptr<IHostProcess> spawn(
        const std::string& command, const std::string& cwd = "",
        const std::map<std::string, std::string>& env = {}) override {
        (void)command;
        (void)cwd;
        (void)env;
        return std::make_unique<ImmediateOutputProcess>();
    }
};

} // namespace

TEST(ProcessManagerTest, monitorUsesHostInspectionWithoutDestroyingFinishedProcess) {
    auto host = std::make_shared<MockHost>();
    MockHostProcessConfig config;
    config.systemId = "mock-fast-process";
    config.running = false;
    config.exitCode = 0;
    host->setSpawnResult("mkdir -p /tmp/fast", config);

    std::vector<StreamEvent> events;
    std::mutex eventsMutex;
    ProcessManager manager(host, [&](const StreamEvent& event) {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.push_back(event);
    });

    const std::string processId =
        manager.spawnProcess("mkdir -p /tmp/fast", "tool-1", "/tmp", {}, true);

    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard<std::mutex> lock(eventsMutex);
            if (findFinishedDelta(events, processId) != nullptr) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        const auto* finished = findFinishedDelta(events, processId);
        ASSERT_NE(finished, nullptr);
        EXPECT_EQ(finished->exitCode, 0);
    }

    EXPECT_TRUE(host->wasCalledWith("releaseBackgroundProcess", {{"id", processId}}));
    const auto snapshot = host->inspectBackgroundProcess(processId);
    EXPECT_FALSE(snapshot.running);
    EXPECT_EQ(snapshot.exitCode, 0);
    EXPECT_EQ(manager.getProcessCount(), 0U);
}

TEST(ProcessManagerTest, EmitsSpawnBeforeImmediateOutputDelta) {
    auto host = std::make_shared<ImmediateOutputHost>();
    std::vector<StreamEvent> events;
    ProcessManager manager(host, [&](const StreamEvent& event) {
        events.push_back(event);
    });

    const std::string processId =
        manager.spawnProcess("echo hi", "tool-1", "/tmp", {}, false);

    ASSERT_GE(events.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<AgentProcessSpawned>(events[0]));
    ASSERT_TRUE(std::holds_alternative<ProcessOutputDelta>(events[1]));

    const auto& spawned = std::get<AgentProcessSpawned>(events[0]);
    const auto& delta = std::get<ProcessOutputDelta>(events[1]);

    EXPECT_EQ(spawned.processId, processId);
    EXPECT_EQ(spawned.toolCallId, "tool-1");
    EXPECT_EQ(delta.processId, processId);
    EXPECT_EQ(delta.output, "booting\n");
    EXPECT_FALSE(delta.finished);
}

TEST(ProcessManagerTest, LocalHostCompletedSnapshotPreservesStdoutAndStderr) {
    auto host = std::make_shared<LocalHost>();
    std::vector<StreamEvent> events;
    std::mutex eventsMutex;
    ProcessManager manager(host, [&](const StreamEvent& event) {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.push_back(event);
    });

    const std::string processId = manager.spawnProcess(
        "printf 'stdout-visible\\n'; printf 'stderr-visible\\n' >&2",
        "tool-1", "/tmp", {}, true);

    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(eventsMutex);
            if (findFinishedDelta(events, processId) != nullptr) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::vector<ProcessOutputDelta> deltas;
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        for (const auto& event : events) {
            if (const auto* delta = std::get_if<ProcessOutputDelta>(&event)) {
                if (delta->processId == processId) {
                    deltas.push_back(*delta);
                }
            }
        }
    }

    ASSERT_FALSE(deltas.empty());
    bool sawStdout = false;
    bool sawStderr = false;
    for (const auto& delta : deltas) {
        if (!delta.isStderr && delta.output.find("stdout-visible") != std::string::npos) {
            sawStdout = true;
        }
        if (delta.isStderr && delta.output.find("stderr-visible") != std::string::npos) {
            sawStderr = true;
        }
    }
    EXPECT_TRUE(sawStdout);
    EXPECT_TRUE(sawStderr);

    const auto snapshot = host->inspectBackgroundProcess(processId);
    EXPECT_FALSE(snapshot.running);
    EXPECT_EQ(snapshot.exitCode, 0);
    EXPECT_NE(snapshot.stdoutData.find("stdout-visible"), std::string::npos);
    EXPECT_NE(snapshot.stderrData.find("stderr-visible"), std::string::npos);
}
