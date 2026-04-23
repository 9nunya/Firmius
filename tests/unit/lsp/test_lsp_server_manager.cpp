#include <gtest/gtest.h>

#include "lsp/LspServerManager.hpp"
#include "lsp/LspServerSpec.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <signal.h>
#endif

namespace fs = std::filesystem;

using firmius::core::LspClient;
using firmius::core::LspServerManager;
using firmius::core::LspServerSpec;

namespace {

std::string uniqueSuffix() {
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

class ScopedTempDir {
public:
    ScopedTempDir() {
        path_ = fs::temp_directory_path() / ("firmius_lsp_manager_test_" + uniqueSuffix());
        fs::create_directories(path_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string lspStubServerCommand() {
#ifndef FIRMIUS_LSP_TEST_STUB_SERVER_PATH
#error "FIRMIUS_LSP_TEST_STUB_SERVER_PATH must be defined for test_lsp_server_manager"
#endif
    const fs::path exe = fs::path(FIRMIUS_LSP_TEST_STUB_SERVER_PATH);
    if (!fs::exists(exe)) {
        throw std::runtime_error("Missing lsp_test_stub_server test helper: " + exe.string());
    }
    return exe.string();
}

LspServerSpec makeSpec(const std::string& id, const std::string& command) {
    LspServerSpec spec;
    spec.id = id;
    spec.commands = {{command}};
    spec.defaultLanguageId = "plaintext";
    spec.extensions = {".txt"};
    return spec;
}

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout,
               std::chrono::milliseconds pollInterval = std::chrono::milliseconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(pollInterval);
    }
    return predicate();
}

class LspServerManagerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        manager_.shutdownAll();
        manager_.setHostForTesting(nullptr);
        ASSERT_EQ(manager_.activeServerCount(), 0U);
#if !defined(_WIN32)
        previousSigPipeHandler_ = ::signal(SIGPIPE, SIG_IGN);
#endif
    }

    void TearDown() override {
        manager_.shutdownAll();
        manager_.setHostForTesting(nullptr);
#if !defined(_WIN32)
        if (previousSigPipeHandler_ != SIG_ERR) {
            ::signal(SIGPIPE, previousSigPipeHandler_);
        }
#endif
    }

    LspServerManager& manager_ = LspServerManager::instance();

#if !defined(_WIN32)
    using SigHandler = void (*)(int);
    SigHandler previousSigPipeHandler_ = SIG_ERR;
#endif
};

TEST_F(LspServerManagerFixture, StartsServerAndReportsHealthyWithExpectedPoolKey) {
    ScopedTempDir temp;
    const fs::path root = temp.path() / "workspace";
    fs::create_directories(root);

    const LspServerSpec spec = makeSpec("stub-start-" + uniqueSuffix(), lspStubServerCommand());

    LspClient* client = manager_.getOrCreateServer(spec, root.string(), 1500);
    ASSERT_NE(client, nullptr);

    EXPECT_TRUE(manager_.isServerHealthy(spec.id, root.string()));
    EXPECT_EQ(manager_.activeServerCount(), 1U);

    const std::string canonicalRoot = fs::weakly_canonical(root).string();
    const std::string expectedKey = spec.id + ":" + canonicalRoot;
    const std::vector<std::string> ids = manager_.activeServerIds();
    ASSERT_EQ(ids.size(), 1U);
    EXPECT_EQ(ids.front(), expectedKey);

    std::vector<std::string> stderrLines;
    ASSERT_TRUE(waitUntil(
        [&]() {
            stderrLines = manager_.getServerStderr(spec.id, root.string(), 20);
            return !stderrLines.empty();
        },
        std::chrono::milliseconds(1200)));
    EXPECT_NE(stderrLines.back().find("stub-started"), std::string::npos);
}

TEST_F(LspServerManagerFixture, ReusesServerForEquivalentCanonicalProjectRoots) {
    ScopedTempDir temp;
    const fs::path root = temp.path() / "workspace" / "a";
    fs::create_directories(root);

    const LspServerSpec spec = makeSpec("stub-reuse-" + uniqueSuffix(), lspStubServerCommand());

    LspClient* first = manager_.getOrCreateServer(spec, root.string(), 1500);
    ASSERT_NE(first, nullptr);

    const std::string equivalent = (root / "." / "b" / "..").string();
    fs::create_directories(root / "b");
    LspClient* second = manager_.getOrCreateServer(spec, equivalent, 1500);

    EXPECT_EQ(first, second);
    EXPECT_EQ(manager_.activeServerCount(), 1U);
}

TEST_F(LspServerManagerFixture, ReleaseKeepsHealthyServerAndShutdownServerRemovesOnlyTarget) {
    ScopedTempDir temp;
    const fs::path rootA = temp.path() / "workspace_a";
    const fs::path rootB = temp.path() / "workspace_b";
    fs::create_directories(rootA);
    fs::create_directories(rootB);

    const LspServerSpec specA = makeSpec("stub-release-a-" + uniqueSuffix(), lspStubServerCommand());
    const LspServerSpec specB = makeSpec("stub-release-b-" + uniqueSuffix(), lspStubServerCommand());

    ASSERT_NE(manager_.getOrCreateServer(specA, rootA.string(), 1500), nullptr);
    ASSERT_NE(manager_.getOrCreateServer(specB, rootB.string(), 1500), nullptr);
    ASSERT_EQ(manager_.activeServerCount(), 2U);

    manager_.releaseServer(specA.id, rootA.string());
    EXPECT_EQ(manager_.activeServerCount(), 2U);
    EXPECT_TRUE(manager_.isServerHealthy(specA.id, rootA.string()));

    manager_.shutdownServer(specA.id, rootA.string());
    EXPECT_EQ(manager_.activeServerCount(), 1U);
    EXPECT_FALSE(manager_.isServerHealthy(specA.id, rootA.string()));
    EXPECT_TRUE(manager_.isServerHealthy(specB.id, rootB.string()));
}

TEST_F(LspServerManagerFixture, ShutdownServerMarksEntryUnhealthyAndRemovesPoolEntry) {
    ScopedTempDir temp;
    const fs::path root = temp.path() / "workspace";
    fs::create_directories(root);

    const LspServerSpec spec = makeSpec("stub-dead-" + uniqueSuffix(), lspStubServerCommand());

    ASSERT_NE(manager_.getOrCreateServer(spec, root.string(), 1500), nullptr);
    ASSERT_TRUE(manager_.isServerHealthy(spec.id, root.string()));

    manager_.shutdownServer(spec.id, root.string());
    EXPECT_FALSE(manager_.isServerHealthy(spec.id, root.string()));
    EXPECT_EQ(manager_.activeServerCount(), 0U);
}

TEST_F(LspServerManagerFixture, ThrowsWhenCommandCannotBeResolvedAndDoesNotLeakPool) {
    ScopedTempDir temp;
    const fs::path root = temp.path() / "workspace";
    fs::create_directories(root);

    const LspServerSpec spec = makeSpec("stub-missing-" + uniqueSuffix(),
                                        "definitely_missing_firmius_lsp_binary_for_test");

    EXPECT_THROW((void)manager_.getOrCreateServer(spec, root.string(), 500), std::runtime_error);
    EXPECT_EQ(manager_.activeServerCount(), 0U);
    EXPECT_FALSE(manager_.isServerHealthy(spec.id, root.string()));
}

} // namespace
