#include "modals/McpModalModel.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::shared::McpServerConfig;
using firmius::tui::McpModalForm;
using firmius::tui::McpSaveResult;
using firmius::tui::McpTemplateKind;

TEST(McpModalModelTest, RejectsBlankNameOnSave) {
  std::map<std::string, McpServerConfig> servers;

  McpModalForm form;
  form.name = "   ";
  form.transport = "stdio";
  form.command = "npx";

  const auto result = firmius::tui::upsertMcpServer(servers, "", form);
  EXPECT_EQ(result, McpSaveResult::EmptyName);
  EXPECT_TRUE(servers.empty());
}

TEST(McpModalModelTest, RejectsRenameCollision) {
  std::map<std::string, McpServerConfig> servers;
  servers["alpha"].transport = "stdio";
  servers["beta"].transport = "http";

  McpModalForm form;
  form.name = "beta";
  form.transport = "stdio";
  form.command = "node";

  const auto result = firmius::tui::upsertMcpServer(servers, "alpha", form);
  EXPECT_EQ(result, McpSaveResult::RenameCollision);
  EXPECT_EQ(servers.size(), 2u);
  EXPECT_TRUE(servers.count("alpha") > 0);
  EXPECT_TRUE(servers.count("beta") > 0);
}

TEST(McpModalModelTest, RenamesMapKeyAndPersistsUpdatedServer) {
  std::map<std::string, McpServerConfig> servers;
  servers["old-name"].transport = "stdio";
  servers["old-name"].command = "npx";

  McpModalForm form;
  form.name = "new-name";
  form.transport = "stdio";
  form.command = "node";
  form.args_text = "server.js, --port, 8080";
  form.env_text = "NODE_ENV=production; LOG_LEVEL=info";
  form.cwd = "/workspace";

  const auto result = firmius::tui::upsertMcpServer(servers, "old-name", form);
  EXPECT_EQ(result, McpSaveResult::Saved);
  EXPECT_EQ(servers.count("old-name"), 0u);
  ASSERT_TRUE(servers.count("new-name") > 0);

  const auto &saved = servers.at("new-name");
  EXPECT_EQ(saved.transport, "stdio");
  EXPECT_EQ(saved.command, "node");
  EXPECT_EQ(saved.args.size(), 3u);
  EXPECT_EQ(saved.args[0], "server.js");
  EXPECT_EQ(saved.args[1], "--port");
  EXPECT_EQ(saved.args[2], "8080");
  EXPECT_EQ(saved.env.at("NODE_ENV"), "production");
  EXPECT_EQ(saved.env.at("LOG_LEVEL"), "info");
  EXPECT_EQ(saved.cwd, "/workspace");
}

TEST(McpModalModelTest, SupportsTransportSwitchAndHttpFieldPersistence) {
  McpModalForm form;
  form.name = "http-server";
  firmius::tui::applyMcpTemplate(form, McpTemplateKind::HttpGeneric);
  form.url = "https://api.example.dev/mcp";
  form.auth_header = "X-API-Key";
  form.auth_bearer_token = "secret-token";
  form.allow_insecure_tls = true;
  form.ca_cert_path = "/etc/certs/ca.pem";

  const auto cfg = firmius::tui::mcpServerFromForm(form);
  EXPECT_EQ(cfg.transport, "http");
  EXPECT_EQ(cfg.url, "https://api.example.dev/mcp");
  EXPECT_EQ(cfg.authHeader, "X-API-Key");
  EXPECT_EQ(cfg.authBearerToken, "secret-token");
  EXPECT_TRUE(cfg.allowInsecureTls);
  EXPECT_EQ(cfg.caCertPath, "/etc/certs/ca.pem");
}

TEST(McpModalModelTest, ToggleEnableDisableAndDeleteBehaviors) {
  std::map<std::string, McpServerConfig> servers;
  servers["demo"].enabled = true;

  EXPECT_TRUE(firmius::tui::toggleMcpServerEnabled(servers, "demo"));
  EXPECT_FALSE(servers.at("demo").enabled);

  EXPECT_TRUE(firmius::tui::toggleMcpServerEnabled(servers, "demo"));
  EXPECT_TRUE(servers.at("demo").enabled);

  EXPECT_TRUE(firmius::tui::deleteMcpServer(servers, "demo"));
  EXPECT_TRUE(servers.empty());
  EXPECT_FALSE(firmius::tui::deleteMcpServer(servers, "missing"));
}

TEST(McpModalModelTest, StdioTemplateProvidesBeginnerFriendlyDefaults) {
  McpModalForm form;
  firmius::tui::applyMcpTemplate(form, McpTemplateKind::StdioFilesystem);

  EXPECT_EQ(form.transport, "stdio");
  EXPECT_EQ(form.command, "npx");
  EXPECT_NE(form.args_text.find("@modelcontextprotocol/server-filesystem"),
            std::string::npos);

  const auto cfg = firmius::tui::mcpServerFromForm(form);
  EXPECT_EQ(cfg.transport, "stdio");
  EXPECT_EQ(cfg.command, "npx");
  EXPECT_FALSE(cfg.args.empty());
}

} // namespace
