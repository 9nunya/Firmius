#include <gtest/gtest.h>

#include "lsp/LspServerManager.hpp"
#include "lsp/LspServerSpec.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

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

std::string makeStubServerScript(const fs::path& scriptPath) {
    const std::string script = R"SCRIPT(#!/usr/bin/env bash
set -euo pipefail

pid_file="${1:-}"
if [[ -n "${pid_file}" ]]; then
  echo "$$" > "${pid_file}"
fi

echo "stub-started $$" >&2

while true; do
  content_length=""
  while IFS= read -r line; do
    line="${line%$'\r'}"
    [[ -z "${line}" ]] && break
    if [[ "${line}" == Content-Length:* ]]; then
      content_length="${line#Content-Length: }"
    fi
  done || exit 0

  [[ -z "${content_length}" ]] && continue

  IFS= read -r -N "${content_length}" body || exit 0

  method=""
  req_id="null"
  if [[ "${body}" =~ \"method\":\"([^\"]+)\" ]]; then
    method="${BASH_REMATCH[1]}"
  fi
  if [[ "${body}" =~ \"id\":([0-9]+) ]]; then
    req_id="${BASH_REMATCH[1]}"
  fi

  if [[ "${method}" == "initialize" ]]; then
    response="{\"jsonrpc\":\"2.0\",\"id\":${req_id},\"result\":{\"capabilities\":{}}}"
    printf 'Content-Length: %d\r\n\r\n%s' "${#response}" "${response}"
  elif [[ "${method}" == "shutdown" ]]; then
    response="{\"jsonrpc\":\"2.0\",\"id\":${req_id},\"result\":null}"
    printf 'Content-Length: %d\r\n\r\n%s' "${#response}" "${response}"
  elif [[ "${method}" == "exit" ]]; then
    exit 0
  fi
done
)SCRIPT";

    std::ofstream out(scriptPath);
    out << script;
    out.close();

    fs::permissions(scriptPath,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                        fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);

    return scriptPath.string();
}

LspServerSpec makeSpec(const std::string& id, const std::vector<std::string>& command) {
    LspServerSpec spec;
    spec.id = id;
    spec.commands = {command};
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

std::optional<pid_t> waitForPidFile(const fs::path& pidFile, std::chrono::milliseconds timeout) {
    pid_t pid = -1;
    const bool found = waitUntil(
        [&]() {
            if (!fs::exists(pidFile)) {
                return false;
            }
            std::ifstream in(pidFile);
            in >> pid;
            return !in.fail() && pid > 0;
        },
        timeout);

    if (!found) {
        return std::nullopt;
    }
    return pid;
}

class LspServerManagerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        manager_.shutdownAll();
        ASSERT_EQ(manager_.activeServerCount(), 0U);
        previousSigPipeHandler_ = ::signal(SIGPIPE, SIG_IGN);
    }

    void TearDown() override {
        manager_.shutdownAll();
        if (previousSigPipeHandler_ != SIG_ERR) {
            ::signal(SIGPIPE, previousSigPipeHandler_);
        }
    }

    LspServerManager& manager_ = LspServerManager::instance();
    using SigHandler = void (*)(int);
    SigHandler previousSigPipeHandler_ = SIG_ERR;
};

TEST_F(LspServerManagerFixture, StartsServerAndReportsHealthyWithExpectedPoolKey) {
    ScopedTempDir temp;
    const fs::path scriptPath = temp.path() / "stub_lsp_server.sh";
    const fs::path root = temp.path() / "workspace";
    fs::create_directories(root);

    const LspServerSpec spec = makeSpec("stub-start-" + uniqueSuffix(), {makeStubServerScript(scriptPath)});

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
    const fs::path scriptPath = temp.path() / "stub_lsp_server.sh";
    const fs::path root = temp.path() / "workspace" / "a";
    fs::create_directories(root);

    const LspServerSpec spec = makeSpec("stub-reuse-" + uniqueSuffix(), {makeStubServerScript(scriptPath)});

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
    const fs::path scriptA = temp.path() / "stub_a.sh";
    const fs::path scriptB = temp.path() / "stub_b.sh";
    const fs::path rootA = temp.path() / "workspace_a";
    const fs::path rootB = temp.path() / "workspace_b";
    fs::create_directories(rootA);
    fs::create_directories(rootB);

    const LspServerSpec specA = makeSpec("stub-release-a-" + uniqueSuffix(), {makeStubServerScript(scriptA)});
    const LspServerSpec specB = makeSpec("stub-release-b-" + uniqueSuffix(), {makeStubServerScript(scriptB)});

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

TEST_F(LspServerManagerFixture, DetectsDeadServerAndCleansPoolEntry) {
    ScopedTempDir temp;
    const fs::path scriptPath = temp.path() / "stub_killable.sh";
    const fs::path pidFile = temp.path() / "stub.pid";
    const fs::path root = temp.path() / "workspace";
    fs::create_directories(root);

    const LspServerSpec spec = makeSpec("stub-dead-" + uniqueSuffix(),
                                        {makeStubServerScript(scriptPath), pidFile.string()});

    ASSERT_NE(manager_.getOrCreateServer(spec, root.string(), 1500), nullptr);
    ASSERT_TRUE(manager_.isServerHealthy(spec.id, root.string()));

    const auto pidOpt = waitForPidFile(pidFile, std::chrono::milliseconds(1000));
    ASSERT_TRUE(pidOpt.has_value());

    ASSERT_EQ(::kill(*pidOpt, SIGKILL), 0);

    const bool becameUnhealthy = waitUntil(
        [&]() { return !manager_.isServerHealthy(spec.id, root.string()); },
        std::chrono::milliseconds(2000));

    EXPECT_TRUE(becameUnhealthy);
    EXPECT_EQ(manager_.activeServerCount(), 0U);
}

TEST_F(LspServerManagerFixture, ThrowsWhenCommandCannotBeResolvedAndDoesNotLeakPool) {
    ScopedTempDir temp;
    const fs::path root = temp.path() / "workspace";
    fs::create_directories(root);

    const LspServerSpec spec = makeSpec("stub-missing-" + uniqueSuffix(),
                                        {"definitely_missing_firmius_lsp_binary_for_test"});

    EXPECT_THROW((void)manager_.getOrCreateServer(spec, root.string(), 500), std::runtime_error);
    EXPECT_EQ(manager_.activeServerCount(), 0U);
    EXPECT_FALSE(manager_.isServerHealthy(spec.id, root.string()));
}

} // namespace
