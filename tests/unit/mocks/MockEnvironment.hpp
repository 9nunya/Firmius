#ifndef FIRMIUS_TEST_MOCK_ENVIRONMENT_HPP
#define FIRMIUS_TEST_MOCK_ENVIRONMENT_HPP

#include "IEnvironment.hpp"
#include "MockHost.hpp"
#include "MockPermissions.hpp"
#include <gmock/gmock.h>

namespace firmius::test {

using namespace firmius::shared;

class MockProcessManager : public IProcessManager {
public:
    virtual ~MockProcessManager() = default;
    using EnvMap = std::map<std::string, std::string>;
    MOCK_METHOD(std::string, spawnProcess,
                (const std::string& command, const std::string& toolCallId,
                 const std::string& cwd, const EnvMap& env),
                (override));
    MOCK_METHOD(ProcessSnapshot, inspectProcess, (const std::string& id), (override));
    MOCK_METHOD(void, writeToProcess, (const std::string& id, const std::string& data), (override));
    MOCK_METHOD(void, registerProcessId, (const std::string& id), (override));
    MOCK_METHOD(void, emitProcessSpawned,
                (const std::string& processId, const std::string& toolCallId,
                 const std::string& command),
                (override));
    MOCK_METHOD(void, addBlockingProcessId, (const std::string& id), (override));
    MOCK_METHOD(void, removeBlockingProcessId, (const std::string& id), (override));
    MOCK_METHOD(std::vector<std::string>, getBlockingProcessIds, (), (override));
    MOCK_METHOD(void, killProcess, (const std::string& id), (override));
};

class MockWorkspace : public IWorkspace {
public:
    virtual ~MockWorkspace() = default;
    MOCK_METHOD(std::string, resolvePath, (const std::string& path), (const, override));
    MOCK_METHOD(bool, hasReadFile, (const std::string& path), (const, override));
    MOCK_METHOD(void, markFileAsRead, (const std::string& path), (override));
    MOCK_METHOD(bool, hasFullyReadFile, (const std::string& path), (const, override));
    MOCK_METHOD(void, markFileAsFullyRead, (const std::string& path), (override));
    MOCK_METHOD(std::string, getCurrentWorkingDirectory, (), (const, override));
};

class MockEnvironment : public IEnvironment {
public:
    virtual ~MockEnvironment() = default;
    MockEnvironment(std::shared_ptr<MockHost> host = nullptr)
        : host_(host ? host : std::make_shared<MockHost>())
        , mockProcessManager_(std::make_unique<MockProcessManager>())
        , mockWorkspace_(std::make_unique<MockWorkspace>())
    {}

    std::string getId() const override {
        return "mock-environment";
    }

    IProcessManager& getProcessManager() override {
        return *mockProcessManager_;
    }

    IWorkspace& getWorkspace() override {
        return *mockWorkspace_;
    }

    std::shared_ptr<IHost> getHost() override {
        return host_;
    }

    void cleanup() override {
        cleanedUp_ = true;
    }

    bool isActive() const override {
        return !cleanedUp_;
    }

    MockProcessManager& mockProcessManager() {
        return *mockProcessManager_;
    }

    MockWorkspace& mockWorkspace() {
        return *mockWorkspace_;
    }

private:
    std::shared_ptr<MockHost> host_;
    std::unique_ptr<MockProcessManager> mockProcessManager_;
    std::unique_ptr<MockWorkspace> mockWorkspace_;
    bool cleanedUp_ = false;
};

} // namespace firmius::test

#endif // FIRMIUS_TEST_MOCK_ENVIRONMENT_HPP