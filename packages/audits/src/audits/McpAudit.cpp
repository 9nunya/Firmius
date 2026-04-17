#include "audits/McpAudit.hpp"

#include "ConfigLoader.hpp"
#include "EnvLoader.hpp"
#include "IAgent.hpp"
#include "IEnvironment.hpp"
#include "IPermissions.hpp"
#include "ITool.hpp"
#include "hosts/LocalHost.hpp"
#include "tools/McpCallTool.hpp"
#include "tools/McpListTool.hpp"
#include <iostream>
#include <memory>

namespace firmius::audits {
namespace {

using namespace firmius::core;
using namespace firmius::shared;

class AuditWorkspace final : public IWorkspace {
public:
  explicit AuditWorkspace(std::string cwd) : cwd_(std::move(cwd)) {}

  std::string resolvePath(const std::string &path) const override { return path; }
  bool hasReadFile(const std::string &) const override { return false; }
  void markFileAsRead(const std::string &) override {}
  bool hasFullyReadFile(const std::string &) const override { return false; }
  void markFileAsFullyRead(const std::string &) override {}
  void recordFileEdit(const std::string &) override {}
  bool isLineRead(const std::string &, int) const override { return false; }
  std::string getCurrentWorkingDirectory() const override { return cwd_; }

private:
  std::string cwd_;
};

class AuditProcessManager final : public IProcessManager {
public:
  std::string spawnProcess(const std::string &, const std::string &, const std::string &,
                           const std::map<std::string, std::string> &, bool) override {
    return "";
  }
  ProcessSnapshot inspectProcess(const std::string &) override { return {}; }
  void writeToProcess(const std::string &, const std::string &) override {}
  void registerProcessId(const std::string &) override {}
  void emitProcessSpawned(const std::string &, const std::string &,
                          const std::string &) override {}
  void addBlockingProcessId(const std::string &) override {}
  void removeBlockingProcessId(const std::string &) override {}
  std::vector<std::string> getBlockingProcessIds() override { return {}; }
  void killProcess(const std::string &) override {}
};

class AuditEnvironment final : public IEnvironment {
public:
  AuditEnvironment(std::shared_ptr<IHost> host, std::string cwd)
      : host_(std::move(host)), workspace_(std::move(cwd)) {}

  std::string getId() const override { return host_ ? host_->getId() : "audit-env"; }
  IProcessManager &getProcessManager() override { return processManager_; }
  IWorkspace &getWorkspace() override { return workspace_; }
  std::shared_ptr<IHost> getHost() override { return host_; }
  void cleanup() override {
    if (host_) {
      host_->cleanup();
      host_->destroy();
    }
  }
  bool isActive() const override { return host_ != nullptr; }

private:
  std::shared_ptr<IHost> host_;
  AuditProcessManager processManager_;
  AuditWorkspace workspace_;
};

class AuditPermissions final : public IPermissions {
public:
  PermissionResponse requestCommandApproval(const std::string &,
                                            const CommandIntent &) override {
    return PermissionResponse::AllowOnce;
  }
  PermissionResponse requestEditApproval(const std::string &) override {
    return PermissionResponse::AllowOnce;
  }
  bool checkPathAccess(const std::string &, AccessMode) const override { return true; }
  void validatePathAccess(const std::string &, AccessMode) const override {}
  bool isCommandAllowed(const CommandIntent &) const override { return true; }
  void allowCommandAlways(const std::string &) override {}
  void denyCommandAlways(const std::string &) override {}
  const ICommandIntentAnalyzer &getIntentAnalyzer() const override {
    throw std::runtime_error("AuditPermissions does not expose an intent analyzer");
  }
  void setApprovalMode(ThreadPermissionMode) override {}
};

class AuditAgent final : public IAgent {
public:
  AuditAgent(std::shared_ptr<AuditEnvironment> environment,
             std::shared_ptr<AuditPermissions> permissions)
      : environment_(std::move(environment)), permissions_(std::move(permissions)) {
    context_.history = std::make_shared<AgentHistory>();
    context_.state.currentStatus = AgentStatus::Idle;
  }

  void reset() override {}
  void run(const std::string &, std::function<void(const StreamEvent &)>,
           const std::vector<ImageContent> &) override {}
  void resume(std::function<void(const StreamEvent &)>) override {}
  void interrupt() override { interrupted_ = true; }
  bool isInterrupted() const override { return interrupted_; }
  void clearInterrupt() override { interrupted_ = false; }
  void compactNow(std::function<void(const StreamEvent &)>) override {}
  void setModel(const std::string &providerId, const std::string &modelId) override {
    context_.config.providerId = providerId;
    context_.config.modelId = modelId;
  }
  void setModel(const std::string &providerId, const std::string &modelId,
                const std::string &variantName) override {
    context_.config.providerId = providerId;
    context_.config.modelId = modelId;
    context_.config.modelVariant = variantName;
  }
  ModelChoice getPreferredModel() const override {
    return {context_.config.providerId, context_.config.modelId,
            context_.config.modelVariant.empty()
                ? std::optional<std::string>{}
                : std::optional<std::string>{context_.config.modelVariant}};
  }
  bool isRunning() const override { return false; }
  bool isBooting() const override { return false; }
  void setBooting(bool) override {}
  const AgentContext &getContext() const override { return context_; }
  AgentContext &getMutableContext() override { return context_; }
  void saveHistory() override {}
  void appendHistoryTurn(const AgentTurn &turn) override {
    if (context_.history) {
      context_.history->turns.push_back(turn);
    }
  }
  std::shared_ptr<IEnvironment> getEnvironment() const override { return environment_; }
  std::shared_ptr<IPermissions> getPermissions() const override { return permissions_; }
  std::shared_ptr<IHost> getHost() override { return environment_->getHost(); }

private:
  AgentContext context_;
  std::shared_ptr<AuditEnvironment> environment_;
  std::shared_ptr<AuditPermissions> permissions_;
  bool interrupted_ = false;
};

} // namespace

using namespace firmius::core;
using namespace firmius::shared;

std::string McpAudit::getId() const { return "mcp"; }

std::string McpAudit::getDescription() const {
  return "List, call, and inspect MCP servers";
}

AuditResult McpAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();

  EnvLoader::load(".env.local");

  std::string serverName;
  std::string toolName;
  std::string toolArgs;
  bool listAll = false;
  bool showPrompts = false;
  bool showTools = true;
  bool showResources = false;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: firmius_audit --audit mcp [options]\n"
                << "\nOptions:\n"
                << "  --server <name>     MCP server name (from config)\n"
                << "  --call <tool>       Call a tool on the server\n"
                << "  --args <json>       Arguments for the tool call (JSON)\n"
                << "  --list              List all configured MCP servers\n"
                << "  --prompts           Show prompts for server\n"
                << "  --tools             Show tools for server (default)\n"
                << "  --resources         Show resources for server\n"
                << "\nExamples:\n"
                << "  firmius_audit --audit mcp --list\n"
                << "  firmius_audit --audit mcp --server filesystem --list\n"
                << "  firmius_audit --audit mcp --server filesystem --call "
                   "read_file --args '{\"path\":\"/tmp/test.txt\"}'\n";
      result.exitCode = 0;
      result.passed = true;
      return result;
    } else if (arg == "--server" && i + 1 < args.size()) {
      serverName = args[++i];
    } else if (arg == "--call" && i + 1 < args.size()) {
      toolName = args[++i];
    } else if (arg == "--args" && i + 1 < args.size()) {
      toolArgs = args[++i];
    } else if (arg == "--list") {
      if (serverName.empty()) {
        listAll = true;
      }
    } else if (arg == "--prompts") {
      showPrompts = true;
    } else if (arg == "--tools") {
      showTools = true;
    } else if (arg == "--resources") {
      showResources = true;
    }
  }

  const auto &config = ConfigLoader::instance().getConfig();

  if (listAll) {
    std::cout << "=== Configured MCP Servers ===" << std::endl;
    for (const auto &[name, server] : config.mcpServers) {
      std::cout << "\nServer: " << name << std::endl;
      std::cout << "  Transport: " << server.transport << std::endl;
      if (server.transport == "stdio") {
        std::cout << "  Command: " << server.command << std::endl;
      } else if (server.transport == "http") {
        std::cout << "  URL: " << server.url << std::endl;
      }
      std::cout << "  Enabled: " << (server.enabled ? "yes" : "no")
                << std::endl;
    }
    result.exitCode = 0;
    result.passed = true;
    return result;
  }

  if (serverName.empty()) {
    std::cerr << "Error: --server is required (or use --list to see all)\n";
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  auto it = config.mcpServers.find(serverName);
  if (it == config.mcpServers.end()) {
    std::cerr << "Error: Unknown MCP server: " << serverName << std::endl;
    std::cerr << "Available servers: ";
    for (const auto &s : config.mcpServers) {
      std::cerr << s.first << " ";
    }
    std::cerr << std::endl;
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  const auto &server = it->second;
  if (!server.enabled) {
    std::cerr << "Error: MCP server '" << serverName << "' is disabled\n";
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  auto host = std::make_shared<LocalHost>();
  host->init();
  auto environment =
      std::make_shared<AuditEnvironment>(host, server.cwd.empty() ? "/work" : server.cwd);
  auto permissions = std::make_shared<AuditPermissions>();
  AuditAgent agent(environment, permissions);
  ToolContext toolCtx{*host, agent, "", nullptr};

  const int timeoutMs = 30000;

  try {
    if (!toolName.empty()) {
      std::cout << "=== Calling tool: " << toolName << " ===" << std::endl;

      rapidjson::Document argsDoc;
      if (!toolArgs.empty()) {
        argsDoc.Parse(toolArgs.c_str());
        if (argsDoc.HasParseError()) {
          std::cerr << "Error: Invalid JSON in --args\n";
          result.exitCode = 1;
          result.passed = false;
          environment->cleanup();
          return result;
        }
      } else {
        argsDoc.SetObject();
      }

      McpCallTool callTool;
      McpCallInput callInput;
      callInput.server_name = serverName;
      callInput.tool_name = toolName;
      callInput.arguments.CopyFrom(argsDoc, callInput.arguments.GetAllocator());
      callInput.timeout_ms = timeoutMs;

      auto toolResult = callTool.execute(callInput, toolCtx);
      if (toolResult.success) {
        rapidjson::Document resultDoc;
        resultDoc.Parse(toolResult.data.c_str());
        if (!resultDoc.HasParseError() && resultDoc.HasMember("remote_result")) {
          rapidjson::StringBuffer buffer;
          rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
          resultDoc["remote_result"].Accept(writer);
          std::cout << "Result:\n" << buffer.GetString() << std::endl;
        } else {
          std::cout << "Result:\n" << toolResult.data << std::endl;
        }
      } else {
        std::cerr << "Error from MCP server: " << toolResult.error << std::endl;
        result.exitCode = 1;
        result.passed = false;
        environment->cleanup();
        return result;
      }
    } else {
      McpListTool listTool;
      McpListInput listInput;
      listInput.server_name = serverName;
      listInput.timeout_ms = timeoutMs;

      auto toolResult = listTool.execute(listInput, toolCtx);
      if (toolResult.success) {
        rapidjson::Document resultDoc;
        resultDoc.Parse(toolResult.data.c_str());
        if (!resultDoc.HasParseError() && resultDoc.HasMember("servers") &&
            resultDoc["servers"].IsArray()) {
          for (const auto &s : resultDoc["servers"].GetArray()) {
            if (showTools || (!showPrompts && !showResources)) {
              std::cout << "=== Tools ===" << std::endl;
              if (s.HasMember("tools") && s["tools"].IsArray()) {
                for (const auto &t : s["tools"].GetArray()) {
                  std::cout << "  - " << t.GetString() << std::endl;
                }
              }
              std::cout << std::endl;
            }

            if (showResources) {
              std::cout << "=== Resources ===" << std::endl;
              if (s.HasMember("resources") && s["resources"].IsArray()) {
                for (const auto &r : s["resources"].GetArray()) {
                  std::cout << "  - " << r.GetString() << std::endl;
                }
              }
              std::cout << std::endl;
            }

            if (showPrompts) {
              std::cout << "=== Prompts ===" << std::endl;
              if (s.HasMember("prompts") && s["prompts"].IsArray()) {
                for (const auto &p : s["prompts"].GetArray()) {
                  std::cout << "  - " << p.GetString() << std::endl;
                }
              }
              std::cout << std::endl;
            }
          }
        }
      } else {
        std::cerr << "Error listing capabilities: " << toolResult.error << std::endl;
        result.exitCode = 1;
        result.passed = false;
        environment->cleanup();
        return result;
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    result.exitCode = 1;
    result.passed = false;
    environment->cleanup();
    return result;
  }

  environment->cleanup();
  result.exitCode = 0;
  result.passed = true;
  return result;
}

} // namespace firmius::audits
