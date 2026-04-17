#include <gtest/gtest.h>
#include "hosts/LocalHost.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <future>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

using namespace firmius::core;
using namespace firmius::shared;

namespace {
int currentProcessId() {
#if defined(_WIN32)
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

std::string pwdCommand() {
#if defined(_WIN32)
    return "cd";
#else
    return "pwd";
#endif
}

std::string sleepCommand(int seconds) {
#if defined(_WIN32)
    return "powershell -NoProfile -Command \"Start-Sleep -Seconds " + std::to_string(seconds) + "\"";
#else
    return "sleep " + std::to_string(seconds);
#endif
}

std::string successCommand() {
#if defined(_WIN32)
    return "exit /b 0";
#else
    return "true";
#endif
}

std::string stdoutStderrCommand() {
#if defined(_WIN32)
    return "echo stdout line && echo stderr line 1>&2";
#else
    return "echo 'stdout line' && echo 'stderr line' >&2";
#endif
}

std::string backgroundGrandchildCommand() {
#if defined(_WIN32)
    return "powershell -NoProfile -Command \"$p = Start-Process powershell -ArgumentList '-NoProfile','-Command','Start-Sleep -Seconds 30' -PassThru; Write-Output ready; Start-Sleep -Seconds 30\"";
#else
    return "sleep 30 & printf 'ready\\n'; sleep 30";
#endif
}
} // namespace

class LocalHostTest : public ::testing::Test {
protected:
    LocalHost host;
    std::filesystem::path tempDir;
    static int testCounter;

    void SetUp() override {
        host.init();
        testCounter++;
        tempDir = std::filesystem::temp_directory_path() /
                  ("firmius_test_" + std::to_string(currentProcessId()) + "_" + std::to_string(testCounter));
        std::filesystem::create_directories(tempDir);
    }

    void TearDown() override {
        if (std::filesystem::exists(tempDir)) {
            std::filesystem::remove_all(tempDir);
        }
        host.destroy();
    }

    void createFile(const std::filesystem::path& path, const std::string& content) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path);
        file << content;
        file.close();
    }

    std::string readFileAsString(const std::filesystem::path& path) {
        std::ifstream file(path);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
};

int LocalHostTest::testCounter = 0;

TEST_F(LocalHostTest, exec_withCwd) {
    auto result = host.exec(pwdCommand(), tempDir.string());

    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.stdoutData.find(tempDir.string()), std::string::npos);
}

TEST_F(LocalHostTest, exec_timeout) {
    auto timeout = std::chrono::milliseconds(100);
    auto result = host.exec(sleepCommand(10), "", {}, timeout);

    EXPECT_EQ(result.finishReason, ProcessFinishReason::Timeout);
    EXPECT_EQ(result.exitCode, -1);
    EXPECT_FALSE(result.backgroundProcessId.empty());

    host.killBackgroundProcess(result.backgroundProcessId);
}

TEST_F(LocalHostTest, exec_commandNotFound) {
    auto result = host.exec("nonexistent_command_xyz_12345");

#if defined(_WIN32)
    EXPECT_NE(result.exitCode, 0);
#else
    EXPECT_EQ(result.exitCode, 127);
#endif
}

TEST_F(LocalHostTest, spawn_lifecycle) {
    auto proc = host.spawn(sleepCommand(30));

    EXPECT_TRUE(proc->isRunning());

    auto snapshot = proc->inspect();
    EXPECT_TRUE(snapshot.running);

    proc->kill();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_FALSE(proc->isRunning());
}

TEST_F(LocalHostTest, spawn_outputCapture) {
    auto proc = host.spawn(stdoutStderrCommand());

    auto result = proc->wait();

    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.stdoutData.find("stdout line"), std::string::npos);
    EXPECT_NE(result.stderrData.find("stderr line"), std::string::npos);
}

#if !defined(_WIN32)
TEST_F(LocalHostTest, killTerminatesProcessGroupAndUnblocksInspect) {
    auto proc = host.spawn(backgroundGrandchildCommand());

    for (int i = 0; i < 100; ++i) {
        auto snapshot = proc->inspect();
        if (snapshot.stdoutData.find("ready") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    proc->kill();

    auto inspectFuture = std::async(std::launch::async, [&]() {
        return proc->inspect();
    });

    ASSERT_EQ(inspectFuture.wait_for(std::chrono::seconds(2)),
              std::future_status::ready)
        << "inspect() stayed blocked after kill(), likely due to surviving child keeping pipes open";

    const auto snapshot = inspectFuture.get();
    EXPECT_FALSE(snapshot.running);
}
#endif

TEST_F(LocalHostTest, inspectBackgroundProcess_finished) {
    auto proc = host.spawn("echo done");
    std::string processId = "test_process_" + std::to_string(currentProcessId());
    host.registerBackgroundProcess(processId, std::move(proc));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto snapshot = host.inspectBackgroundProcess(processId);
    EXPECT_FALSE(snapshot.running);
    EXPECT_EQ(snapshot.exitCode, 0);

    auto secondSnapshot = host.inspectBackgroundProcess(processId);
    EXPECT_FALSE(secondSnapshot.running);
    EXPECT_EQ(secondSnapshot.exitCode, 0);
}

TEST_F(LocalHostTest, spawn_fastProcessReportsRealExitCodeBeforeFinished) {
    auto proc = host.spawn(successCommand());

    for (int i = 0; i < 100; ++i) {
        auto snapshot = proc->inspect();
        if (!snapshot.running) {
            EXPECT_EQ(snapshot.exitCode, 0);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    FAIL() << "process did not finish in time";
}

TEST_F(LocalHostTest, inspectBackgroundProcess_immediateExitKeepsFinishedSnapshot) {
    auto proc = host.spawn(successCommand());
    std::string processId = "immediate_exit_" + std::to_string(currentProcessId());
    host.registerBackgroundProcess(processId, std::move(proc));

    for (int i = 0; i < 100; ++i) {
        auto snapshot = host.inspectBackgroundProcess(processId);
        if (!snapshot.running) {
            EXPECT_EQ(snapshot.exitCode, 0);

            auto repeatedSnapshot = host.inspectBackgroundProcess(processId);
            EXPECT_FALSE(repeatedSnapshot.running);
            EXPECT_EQ(repeatedSnapshot.exitCode, 0);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    FAIL() << "background process did not finish in time";
}

TEST_F(LocalHostTest, listDir_basic) {
    createFile(tempDir / "file1.txt", "content1");
    createFile(tempDir / "file2.txt", "content2");
    std::filesystem::create_directories(tempDir / "subdir");

    auto entries = host.listDir(tempDir.string());

    EXPECT_EQ(entries.size(), 3);

    bool foundFile1 = false, foundFile2 = false, foundSubdir = false;
    for (const auto& entry : entries) {
        if (entry.name == "file1.txt") {
            foundFile1 = true;
            EXPECT_FALSE(entry.isDirectory);
            EXPECT_EQ(entry.size, 8);
        } else if (entry.name == "file2.txt") {
            foundFile2 = true;
            EXPECT_FALSE(entry.isDirectory);
        } else if (entry.name == "subdir") {
            foundSubdir = true;
            EXPECT_TRUE(entry.isDirectory);
        }
    }

    EXPECT_TRUE(foundFile1);
    EXPECT_TRUE(foundFile2);
    EXPECT_TRUE(foundSubdir);
}

TEST_F(LocalHostTest, listDir_missingPathReportsNotFound) {
    const auto missing = (tempDir / "does-not-exist").string();
    EXPECT_THROW({
        try {
            host.listDir(missing);
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find("Path not found"), std::string::npos);
            throw;
        }
    }, std::runtime_error);
}

TEST_F(LocalHostTest, listDir_filePathReportsNotDirectory) {
    const auto filePath = tempDir / "single-file.txt";
    createFile(filePath, "content");
    EXPECT_THROW({
        try {
            host.listDir(filePath.string());
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find("Not a directory"), std::string::npos);
            throw;
        }
    }, std::runtime_error);
}

TEST_F(LocalHostTest, readFile_success) {
    std::string expectedContent = "Hello, World!\nThis is a test file.\n";
    auto filePath = tempDir / "testfile.txt";
    createFile(filePath, expectedContent);

    auto data = host.readFile(filePath.string());
    std::string actualContent(data.begin(), data.end());

    EXPECT_EQ(actualContent, expectedContent);
}

TEST_F(LocalHostTest, writeFile_success) {
    auto filePath = tempDir / "writetest.txt";
    std::string content = "Test content for write operation";
    std::vector<uint8_t> data(content.begin(), content.end());

    host.writeFile(filePath.string(), data);

    EXPECT_TRUE(std::filesystem::exists(filePath));
    std::string actualContent = readFileAsString(filePath);
    EXPECT_EQ(actualContent, content);
}

TEST_F(LocalHostTest, stat_file) {
    auto filePath = tempDir / "stattest.txt";
    std::string content = "Content for stat test";
    createFile(filePath, content);

    auto info = host.stat(filePath.string());

    EXPECT_EQ(info.name, "stattest.txt");
    EXPECT_EQ(info.path, filePath.string());
    EXPECT_EQ(info.size, content.length());
    EXPECT_FALSE(info.isDirectory);
    EXPECT_FALSE(info.isSymlink);
    EXPECT_GT(info.modifiedMs, 0);
}

TEST_F(LocalHostTest, stat_directory) {
    auto dirPath = tempDir / "testsubdir";
    std::filesystem::create_directories(dirPath);

    auto info = host.stat(dirPath.string());

    EXPECT_EQ(info.name, "testsubdir");
    EXPECT_TRUE(info.isDirectory);
}
