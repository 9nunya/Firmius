#include <gtest/gtest.h>

#include "environment/ProcessManager.hpp"
#include "mocks/MockHost.hpp"
#include "mocks/MockHostProcess.hpp"

#include <chrono>
#include <thread>
#include <variant>
#include <vector>

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

    const auto snapshot = host->inspectBackgroundProcess(processId);
    EXPECT_FALSE(snapshot.running);
    EXPECT_EQ(snapshot.exitCode, 0);
    EXPECT_EQ(manager.getProcessCount(), 0U);
}
