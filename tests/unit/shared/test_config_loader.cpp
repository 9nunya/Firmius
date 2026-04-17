#include "ConfigLoader.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

class ConfigLoaderRouterTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char *existing_home = std::getenv("HOME");
    if (existing_home) {
      original_home_ = existing_home;
    }
    auto unique = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
    temp_home_ = std::filesystem::temp_directory_path() /
                 ("firmius_config_router_test_" + unique);
    std::filesystem::create_directories(temp_home_);
    setenv("HOME", temp_home_.c_str(), 1);
  }

  void TearDown() override {
    std::filesystem::remove_all(temp_home_);
    if (original_home_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", original_home_.c_str(), 1);
    }
  }

  std::filesystem::path temp_home_;
  std::string original_home_;
};

TEST_F(ConfigLoaderRouterTest, SavesAndLoadsRouterCategoriesAndPurposeRoutes) {
  auto &loader = firmius::shared::ConfigLoader::instance();

  firmius::shared::UserConfig cfg;
  cfg.defaultProviderId = "openai";
  cfg.defaultModelId = "gpt-5";
  cfg.defaultModelVariant = "thinking";
  cfg.modelRouterCategories["code"].models = {{"openai", "gpt-5-codex", "thinking"}};
  cfg.modelRouterCategories["research"].models = {{"openrouter", "qwen-omni", ""}};
  cfg.purposeRoutes["executor"] = "code";
  cfg.defaultRouteCategory = "research";
  cfg.enableSubagentRouteFallback = true;
  cfg.subagentRouteFallbackOrder = {"research", "code"};
  cfg.rollingMemory.enabled = true;
  cfg.rollingMemory.mode = "rolling_forever";
  cfg.rollingMemory.preset = "balanced";
  cfg.rollingMemory.targetOccupancyRatio = 0.57f;
  cfg.rollingMemory.bufferOccupancyRatio = 0.47f;
  cfg.rollingMemory.emergencyOccupancyRatio = 0.66f;
  cfg.rollingMemory.observer = {true, "openai", "gpt-5.4-mini", "fast"};
  cfg.rollingMemory.reflector = {true, "openrouter", "qwen3", ""};

  loader.updateConfig(cfg);
  loader.save();

  loader.updateConfig(firmius::shared::UserConfig{});
  loader.load();
  const auto loaded = loader.getConfig();

  ASSERT_EQ(loaded.modelRouterCategories.size(), 2u);
  ASSERT_EQ(loaded.purposeRoutes.size(), 1u);
  ASSERT_EQ(loaded.modelRouterCategories.at("code").models.size(), 1u);
  EXPECT_EQ(loaded.modelRouterCategories.at("code").models[0].providerId, "openai");
  EXPECT_EQ(loaded.modelRouterCategories.at("code").models[0].modelId, "gpt-5-codex");
  EXPECT_EQ(loaded.modelRouterCategories.at("code").models[0].variantName, "thinking");
  EXPECT_EQ(loaded.purposeRoutes.at("executor"), "code");
  EXPECT_EQ(loaded.defaultRouteCategory, "research");
  EXPECT_TRUE(loaded.enableSubagentRouteFallback);
  ASSERT_EQ(loaded.subagentRouteFallbackOrder.size(), 2u);
  EXPECT_EQ(loaded.subagentRouteFallbackOrder[0], "research");
  EXPECT_EQ(loaded.subagentRouteFallbackOrder[1], "code");
  EXPECT_TRUE(loaded.rollingMemory.enabled);
  EXPECT_EQ(loaded.rollingMemory.mode, "rolling_forever");
  EXPECT_EQ(loaded.rollingMemory.preset, "balanced");
  EXPECT_FLOAT_EQ(loaded.rollingMemory.targetOccupancyRatio, 0.57f);
  EXPECT_EQ(loaded.rollingMemory.observer.providerId, "openai");
  EXPECT_EQ(loaded.rollingMemory.observer.modelId, "gpt-5.4-mini");
  EXPECT_EQ(loaded.rollingMemory.reflector.providerId, "openrouter");
}

TEST_F(ConfigLoaderRouterTest, MissingRouterFieldsRemainBackwardCompatible) {
  auto &loader = firmius::shared::ConfigLoader::instance();
  const auto config_path = loader.getConfigPath();
  std::filesystem::create_directories(
      std::filesystem::path(config_path).parent_path());

  std::ofstream out(config_path);
  out << R"({
  "defaultProviderId": "openai",
  "defaultModelId": "gpt-5",
  "defaultModelVariant": "thinking",
  "defaultLeadPersona": "lead"
})";
  out.close();

  loader.updateConfig(firmius::shared::UserConfig{});
  loader.load();
  const auto loaded = loader.getConfig();

  EXPECT_TRUE(loaded.modelRouterCategories.empty());
  EXPECT_TRUE(loaded.purposeRoutes.empty());
  EXPECT_TRUE(loaded.defaultRouteCategory.empty());
  EXPECT_TRUE(loaded.enableSubagentRouteFallback);
  EXPECT_TRUE(loaded.subagentRouteFallbackOrder.empty());
  EXPECT_EQ(loaded.defaultProviderId, "openai");
}

TEST_F(ConfigLoaderRouterTest, SavesAndLoadsMcpServersRoundTrip) {
  auto &loader = firmius::shared::ConfigLoader::instance();

  firmius::shared::UserConfig cfg;
  firmius::shared::McpStdioServerConfig primary;
  primary.command = "npx";
  primary.args = {"-y", "@modelcontextprotocol/server-filesystem", "/tmp"};
  primary.env = {{"NODE_ENV", "production"}, {"LOG_LEVEL", "debug"}};
  primary.cwd = "/workspace";
  primary.enabled = false;

  firmius::shared::McpServerConfig remote;
  remote.transport = "http";
  remote.url = "https://mcp.example.com/v1";
  remote.authHeader = "X-API-Key";
  remote.authBearerToken = "secret-token";
  remote.allowInsecureTls = true;
  remote.caCertPath = "/etc/ssl/custom-ca.pem";
  remote.enabled = true;

  cfg.mcpServers["filesystem"] = primary;
  cfg.mcpServers["remote"] = remote;

  loader.updateConfig(cfg);
  loader.save();

  const auto config_path = loader.getConfigPath();
  std::ifstream saved(config_path);
  ASSERT_TRUE(saved.is_open());
  const std::string saved_json((std::istreambuf_iterator<char>(saved)),
                               std::istreambuf_iterator<char>());
  EXPECT_NE(saved_json.find("\"mcpServers\""), std::string::npos);
  EXPECT_NE(saved_json.find("\"transport\""), std::string::npos);
  EXPECT_NE(saved_json.find("\"stdio\""), std::string::npos);
  EXPECT_NE(saved_json.find("\"http\""), std::string::npos);
  EXPECT_NE(saved_json.find("\"url\""), std::string::npos);

  loader.updateConfig(firmius::shared::UserConfig{});
  loader.load();
  const auto loaded = loader.getConfig();

  ASSERT_EQ(loaded.mcpServers.size(), 2u);

  const auto &loadedPrimary = loaded.mcpServers.at("filesystem");
  EXPECT_EQ(loadedPrimary.transport, "stdio");
  EXPECT_EQ(loadedPrimary.command, "npx");
  EXPECT_EQ(loadedPrimary.args.size(), 3u);
  EXPECT_EQ(loadedPrimary.args[0], "-y");
  EXPECT_EQ(loadedPrimary.args[1], "@modelcontextprotocol/server-filesystem");
  EXPECT_EQ(loadedPrimary.args[2], "/tmp");
  ASSERT_EQ(loadedPrimary.env.size(), 2u);
  EXPECT_EQ(loadedPrimary.env.at("NODE_ENV"), "production");
  EXPECT_EQ(loadedPrimary.env.at("LOG_LEVEL"), "debug");
  EXPECT_EQ(loadedPrimary.cwd, "/workspace");
  EXPECT_FALSE(loadedPrimary.enabled);

  const auto &loadedRemote = loaded.mcpServers.at("remote");
  EXPECT_EQ(loadedRemote.transport, "http");
  EXPECT_EQ(loadedRemote.url, "https://mcp.example.com/v1");
  EXPECT_EQ(loadedRemote.authHeader, "X-API-Key");
  EXPECT_EQ(loadedRemote.authBearerToken, "secret-token");
  EXPECT_TRUE(loadedRemote.allowInsecureTls);
  EXPECT_EQ(loadedRemote.caCertPath, "/etc/ssl/custom-ca.pem");
  EXPECT_TRUE(loadedRemote.enabled);
}

TEST_F(ConfigLoaderRouterTest, MissingMcpServerFieldsRemainBackwardCompatible) {
  auto &loader = firmius::shared::ConfigLoader::instance();
  const auto config_path = loader.getConfigPath();
  std::filesystem::create_directories(
      std::filesystem::path(config_path).parent_path());

  {
    std::ofstream out(config_path);
    out << R"({
  "defaultProviderId": "openai",
  "defaultModelId": "gpt-5",
  "defaultLeadPersona": "lead"
})";
  }

  loader.updateConfig(firmius::shared::UserConfig{});
  loader.load();
  auto loaded = loader.getConfig();
  EXPECT_TRUE(loaded.mcpServers.empty());

  {
    std::ofstream out(config_path);
    out << R"({
  "defaultProviderId": "openai",
  "defaultModelId": "gpt-5",
  "defaultLeadPersona": "lead",
  "mcpServers": {
    "empty": {},
    "partial": {
      "command": "npx"
    }
  }
})";
  }

  loader.updateConfig(firmius::shared::UserConfig{});
  loader.load();
  loaded = loader.getConfig();

  ASSERT_EQ(loaded.mcpServers.size(), 2u);

  const auto &emptyServer = loaded.mcpServers.at("empty");
  EXPECT_TRUE(emptyServer.command.empty());
  EXPECT_TRUE(emptyServer.args.empty());
  EXPECT_TRUE(emptyServer.env.empty());
  EXPECT_TRUE(emptyServer.cwd.empty());
  EXPECT_TRUE(emptyServer.enabled);
  EXPECT_EQ(emptyServer.transport, "stdio");

  const auto &partialServer = loaded.mcpServers.at("partial");
  EXPECT_EQ(partialServer.command, "npx");
  EXPECT_TRUE(partialServer.args.empty());
  EXPECT_TRUE(partialServer.env.empty());
  EXPECT_TRUE(partialServer.cwd.empty());
  EXPECT_TRUE(partialServer.enabled);
  EXPECT_EQ(partialServer.transport, "stdio");
}

TEST_F(ConfigLoaderRouterTest, LoadsTransportAwareMcpServerSchema) {
  auto &loader = firmius::shared::ConfigLoader::instance();
  const auto config_path = loader.getConfigPath();
  std::filesystem::create_directories(
      std::filesystem::path(config_path).parent_path());

  std::ofstream out(config_path);
  out << R"({
  "defaultProviderId": "openai",
  "defaultModelId": "gpt-5",
  "defaultLeadPersona": "lead",
  "mcpServers": {
    "filesystem": {
      "transport": "stdio",
      "stdio": {
        "command": "npx",
        "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"],
        "env": {"NODE_ENV": "production"},
        "cwd": "/workspace",
        "enabled": false
      }
    }
  }
})";
  out.close();

  loader.updateConfig(firmius::shared::UserConfig{});
  loader.load();
  const auto loaded = loader.getConfig();

  ASSERT_EQ(loaded.mcpServers.size(), 1u);
  const auto &server = loaded.mcpServers.at("filesystem");
  EXPECT_EQ(server.transport, "stdio");
  EXPECT_EQ(server.command, "npx");
  ASSERT_EQ(server.args.size(), 3u);
  EXPECT_EQ(server.args[0], "-y");
  EXPECT_EQ(server.args[1], "@modelcontextprotocol/server-filesystem");
  EXPECT_EQ(server.args[2], "/tmp");
  ASSERT_EQ(server.env.size(), 1u);
  EXPECT_EQ(server.env.at("NODE_ENV"), "production");
  EXPECT_EQ(server.cwd, "/workspace");
  EXPECT_FALSE(server.enabled);
}

TEST_F(ConfigLoaderRouterTest, LoadsLegacyFlatStdioMcpServerSchema) {
  auto &loader = firmius::shared::ConfigLoader::instance();
  const auto config_path = loader.getConfigPath();
  std::filesystem::create_directories(
      std::filesystem::path(config_path).parent_path());

  std::ofstream out(config_path);
  out << R"({
  "defaultProviderId": "openai",
  "defaultModelId": "gpt-5",
  "defaultLeadPersona": "lead",
  "mcpServers": {
    "legacy_stdio": {
      "command": "python3",
      "args": ["-m", "legacy_mcp"],
      "env": {"PYTHONUNBUFFERED": "1"},
      "cwd": "/legacy",
      "enabled": false
    }
  }
})";
  out.close();

  loader.updateConfig(firmius::shared::UserConfig{});
  loader.load();
  const auto loaded = loader.getConfig();

  ASSERT_EQ(loaded.mcpServers.size(), 1u);
  const auto &legacy = loaded.mcpServers.at("legacy_stdio");
  EXPECT_EQ(legacy.transport, "stdio");
  EXPECT_EQ(legacy.command, "python3");
  ASSERT_EQ(legacy.args.size(), 2u);
  EXPECT_EQ(legacy.args[0], "-m");
  EXPECT_EQ(legacy.args[1], "legacy_mcp");
  ASSERT_EQ(legacy.env.size(), 1u);
  EXPECT_EQ(legacy.env.at("PYTHONUNBUFFERED"), "1");
  EXPECT_EQ(legacy.cwd, "/legacy");
  EXPECT_FALSE(legacy.enabled);
}

} // namespace
