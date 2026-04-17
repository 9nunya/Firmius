#include "CliOptions.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<char *> makeArgv(std::vector<std::string> &args) {
  std::vector<char *> argv;
  argv.reserve(args.size());
  for (auto &arg : args) {
    argv.push_back(arg.data());
  }
  return argv;
}

} // namespace

TEST(CliOptionsTest, ParsesDirectPromptStartupFlags) {
  std::vector<std::string> args = {"firmius",
                                   "--prompt",
                                   "build a full project",
                                   "--cwd",
                                   "/mnt/SHIT/Projects",
                                   "--quit-when-idle"};
  auto argv = makeArgv(args);

  const auto options =
      firmius::cli::parseCliOptions(static_cast<int>(argv.size()), argv.data());

  EXPECT_EQ(options.initialPrompt, "build a full project");
  EXPECT_EQ(options.initialCwd, "/mnt/SHIT/Projects");
  EXPECT_TRUE(options.quitWhenIdle);
  EXPECT_EQ(options.permissionMode,
            firmius::shared::ThreadPermissionMode::Request);
}

TEST(CliOptionsTest, LoadsPromptFromFile) {
  const auto promptPath = std::filesystem::temp_directory_path() /
                          "firmius-cli-options-prompt.txt";
  {
    std::ofstream out(promptPath);
    ASSERT_TRUE(out.is_open());
    out << "ForgeLang style soak prompt";
  }

  std::vector<std::string> args = {"firmius", "--prompt-file",
                                   promptPath.string()};
  auto argv = makeArgv(args);

  const auto options =
      firmius::cli::parseCliOptions(static_cast<int>(argv.size()), argv.data());

  EXPECT_EQ(options.initialPrompt, "ForgeLang style soak prompt");
  EXPECT_FALSE(options.quitWhenIdle);

  std::filesystem::remove(promptPath);
}

TEST(CliOptionsTest, ParsesPermissionMode) {
  std::vector<std::string> args = {"firmius", "--permission-mode",
                                   "always-allow"};
  auto argv = makeArgv(args);

  const auto options =
      firmius::cli::parseCliOptions(static_cast<int>(argv.size()), argv.data());

  EXPECT_EQ(options.permissionMode,
            firmius::shared::ThreadPermissionMode::AlwaysAllow);
}

TEST(CliOptionsTest, MissingPromptFileThrows) {
  std::vector<std::string> args = {"firmius", "--prompt-file",
                                   "/tmp/firmius-cli-options-missing.txt"};
  auto argv = makeArgv(args);

  EXPECT_THROW(
      firmius::cli::parseCliOptions(static_cast<int>(argv.size()), argv.data()),
      std::runtime_error);
}
