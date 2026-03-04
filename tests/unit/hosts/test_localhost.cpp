#include <gtest/gtest.h>
#include "hosts/LocalHost.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <signal.h>

using namespace firmius::core;
using namespace firmius::shared;

class LocalHostTest : public ::testing::Test {
protected:
    LocalHost host;
    std::filesystem::path tempDir;
    static int testCounter;

    void SetUp() override {
        host.init();
        testCounter++;
        tempDir = std::filesystem::temp_directory_path() / 
                  ("firmius_test_" + std::to_string(getpid()) + "_" + std::to_string(testCounter));
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
    auto result = host.exec("pwd", tempDir.string());
    
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.stdoutData.find(tempDir.string()), std::string::npos);
}

TEST_F(LocalHostTest, exec_timeout) {
    auto timeout = std::chrono::milliseconds(100);
    auto result = host.exec("sleep 10", "", {}, timeout);
    
    EXPECT_EQ(result.finishReason, ProcessFinishReason::Timeout);
    EXPECT_EQ(result.exitCode, -1);
    EXPECT_FALSE(result.backgroundProcessId.empty());
    
    host.killBackgroundProcess(result.backgroundProcessId);
}

TEST_F(LocalHostTest, exec_commandNotFound) {
    auto result = host.exec("nonexistent_command_xyz_12345");
    
    EXPECT_EQ(result.exitCode, 127);
}

TEST_F(LocalHostTest, spawn_lifecycle) {
    auto proc = host.spawn("sleep 30");
    
    EXPECT_TRUE(proc->isRunning());
    
    auto snapshot = proc->inspect();
    EXPECT_TRUE(snapshot.running);
    
    proc->kill();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    EXPECT_FALSE(proc->isRunning());
}

TEST_F(LocalHostTest, spawn_outputCapture) {
    auto proc = host.spawn("echo 'stdout line' && echo 'stderr line' >&2");
    
    auto result = proc->wait();
    
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.stdoutData.find("stdout line"), std::string::npos);
    EXPECT_NE(result.stderrData.find("stderr line"), std::string::npos);
}

TEST_F(LocalHostTest, inspectBackgroundProcess_finished) {
    auto proc = host.spawn("echo 'done'");
    std::string processId = "test_process_" + std::to_string(getpid());
    host.registerBackgroundProcess(processId, std::move(proc));
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    auto snapshot = host.inspectBackgroundProcess(processId);
    EXPECT_FALSE(snapshot.running);
    
    EXPECT_THROW(host.inspectBackgroundProcess(processId), std::runtime_error);
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
