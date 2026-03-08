#include <gtest/gtest.h>
#include "utils/FSUtil.hpp"
#include <filesystem>

using namespace firmius::shared;

namespace {

class FSUtilTest : public ::testing::Test {
protected:
    void SetUp() override {
        testBaseDir_ = std::filesystem::temp_directory_path() / "fsutil_test";
        std::filesystem::create_directories(testBaseDir_);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(testBaseDir_);
    }
    
    std::filesystem::path testBaseDir_;
};

TEST_F(FSUtilTest, resolvePath_absolute) {
    std::string absolutePath = "/home/user/project/file.txt";
    std::string baseDir = "/tmp";
    
    std::string result = FSUtil::resolvePath(absolutePath, baseDir);
    
    EXPECT_EQ(result, "/home/user/project/file.txt");
}

TEST_F(FSUtilTest, resolvePath_relative) {
    std::string relativePath = "subdir/file.txt";
    std::string baseDir = "/home/user";
    
    std::string result = FSUtil::resolvePath(relativePath, baseDir);
    
    EXPECT_EQ(result, "/home/user/subdir/file.txt");
}

TEST_F(FSUtilTest, resolvePath_withDotDot) {
    std::string relativePath = "../sibling/file.txt";
    std::string baseDir = "/home/user/project";
    
    std::string result = FSUtil::resolvePath(relativePath, baseDir);
    
    EXPECT_EQ(result, "/home/user/sibling/file.txt");
}

TEST_F(FSUtilTest, resolvePath_trailingSlash) {
    std::string relativePath = "file.txt";
    std::string baseDirWithSlash = "/home/user/";
    std::string baseDirWithoutSlash = "/home/user";
    
    std::string resultWithSlash = FSUtil::resolvePath(relativePath, baseDirWithSlash);
    std::string resultWithoutSlash = FSUtil::resolvePath(relativePath, baseDirWithoutSlash);
    
    EXPECT_EQ(resultWithSlash, "/home/user/file.txt");
    EXPECT_EQ(resultWithoutSlash, "/home/user/file.txt");
}

TEST_F(FSUtilTest, isSubpath_exactMatch) {
    std::string path = "/tmp";
    std::string root = "/tmp";
    
    EXPECT_TRUE(FSUtil::isSubpath(path, root));
}

TEST_F(FSUtilTest, isSubpath_child) {
    std::string path = "/tmp/foo/bar";
    std::string root = "/tmp";
    
    EXPECT_TRUE(FSUtil::isSubpath(path, root));
}

TEST_F(FSUtilTest, isSubpath_sibling) {
    std::string path = "/tmp2";
    std::string root = "/tmp";
    
    EXPECT_FALSE(FSUtil::isSubpath(path, root));
}

TEST_F(FSUtilTest, isSubpath_trailingSlash) {
    std::string path = "/tmp/foo";
    std::string rootWithSlash = "/tmp/";
    std::string rootWithoutSlash = "/tmp";
    
    EXPECT_TRUE(FSUtil::isSubpath(path, rootWithSlash));
    EXPECT_TRUE(FSUtil::isSubpath(path, rootWithoutSlash));
}

TEST_F(FSUtilTest, isSubpath_nestedPaths) {
    std::string path = "/home/user/projects/myproject";
    std::string root = "/home/user";
    
    EXPECT_TRUE(FSUtil::isSubpath(path, root));
}

TEST_F(FSUtilTest, isSubpath_pathEscapesRoot) {
    std::string path = "/home/other";
    std::string root = "/home/user";
    
    EXPECT_FALSE(FSUtil::isSubpath(path, root));
}

TEST_F(FSUtilTest, resolvePath_normalizesMultipleSlashes) {
    std::string relativePath = "./subdir//file.txt";
    std::string baseDir = "/home/user";
    
    std::string result = FSUtil::resolvePath(relativePath, baseDir);
    
    EXPECT_EQ(result, "/home/user/subdir/file.txt");
}

TEST_F(FSUtilTest, resolvePath_emptyPath) {
    std::string emptyPath = "";
    std::string baseDir = "/home/user";
    
    std::string result = FSUtil::resolvePath(emptyPath, baseDir);
    
    EXPECT_EQ(result, "/home/user");
}

TEST_F(FSUtilTest, isSubpath_wildcard) {
    std::string path = "/tmp/foo/bar";
    std::string root = "/tmp/**";
    
    EXPECT_TRUE(FSUtil::isSubpath(path, root));
    
    std::string path2 = "/other/path";
    EXPECT_FALSE(FSUtil::isSubpath(path2, root));
}

TEST_F(FSUtilTest, isSubpath_wildcard_nested) {
    std::string path = "/home/user/.agent/skills/git.js";
    std::string root = "/home/user/.agent/skills/**";
    
    EXPECT_TRUE(FSUtil::isSubpath(path, root));
}

} // namespace
