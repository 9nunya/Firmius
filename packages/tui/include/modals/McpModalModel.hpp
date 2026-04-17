#pragma once

#include "ConfigLoader.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace firmius::tui {

struct McpModalForm {
  std::string name;
  std::string transport = "stdio";
  bool enabled = true;

  std::string command;
  std::string args_text;
  std::string env_text;
  std::string cwd;

  std::string url;
  std::string auth_header = "Authorization";
  std::string auth_bearer_token;
  bool allow_insecure_tls = false;
  std::string ca_cert_path;
};

enum class McpTemplateKind {
  StdioFilesystem,
  StdioCustom,
  HttpGeneric,
};

enum class McpSaveResult {
  Saved,
  EmptyName,
  RenameCollision,
};

inline std::string trimMcpText(std::string value) {
  auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), notSpace));
  value.erase(
      std::find_if(value.rbegin(), value.rend(), notSpace).base(),
      value.end());
  return value;
}

inline std::vector<std::string> parseMcpArgs(const std::string &text) {
  std::vector<std::string> args;
  std::string token;
  for (char ch : text) {
    if (ch == ',') {
      token = trimMcpText(token);
      if (!token.empty()) {
        args.push_back(token);
      }
      token.clear();
      continue;
    }
    token.push_back(ch);
  }
  token = trimMcpText(token);
  if (!token.empty()) {
    args.push_back(token);
  }
  return args;
}

inline std::string joinMcpArgs(const std::vector<std::string> &args) {
  std::string out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += args[i];
  }
  return out;
}

inline std::map<std::string, std::string> parseMcpEnv(const std::string &text) {
  std::map<std::string, std::string> env;
  std::string token;
  auto flush = [&env](std::string pairText) {
    pairText = trimMcpText(pairText);
    if (pairText.empty()) {
      return;
    }
    const auto eq = pairText.find('=');
    if (eq == std::string::npos) {
      return;
    }
    std::string key = trimMcpText(pairText.substr(0, eq));
    std::string value = trimMcpText(pairText.substr(eq + 1));
    if (key.empty()) {
      return;
    }
    env[key] = value;
  };

  for (char ch : text) {
    if (ch == ';') {
      flush(token);
      token.clear();
      continue;
    }
    token.push_back(ch);
  }
  flush(token);
  return env;
}

inline std::string joinMcpEnv(const std::map<std::string, std::string> &env) {
  std::string out;
  size_t i = 0;
  for (const auto &[key, value] : env) {
    if (i++ > 0) {
      out += "; ";
    }
    out += key + "=" + value;
  }
  return out;
}

inline void applyMcpTemplate(McpModalForm &form, McpTemplateKind kind) {
  switch (kind) {
  case McpTemplateKind::StdioFilesystem:
    form.transport = "stdio";
    form.command = "npx";
    form.args_text = "-y, @modelcontextprotocol/server-filesystem, <directory-path>";
    form.env_text = "";
    form.cwd = "";
    break;
  case McpTemplateKind::StdioCustom:
    form.transport = "stdio";
    form.command = "";
    form.args_text = "";
    form.env_text = "";
    form.cwd = "";
    break;
  case McpTemplateKind::HttpGeneric:
    form.transport = "http";
    form.url = "https://mcp.example.com/v1";
    form.auth_header = "Authorization";
    form.auth_bearer_token = "";
    form.allow_insecure_tls = false;
    form.ca_cert_path = "";
    break;
  }
}

inline McpModalForm mcpFormFromServer(const std::string &name,
                                      const shared::McpServerConfig &server) {
  McpModalForm form;
  form.name = name;
  form.transport = server.transport.empty() ? "stdio" : server.transport;
  form.enabled = server.enabled;
  form.command = server.command;
  form.args_text = joinMcpArgs(server.args);
  form.env_text = joinMcpEnv(server.env);
  form.cwd = server.cwd;
  form.url = server.url;
  form.auth_header = server.authHeader.empty() ? "Authorization" : server.authHeader;
  form.auth_bearer_token = server.authBearerToken;
  form.allow_insecure_tls = server.allowInsecureTls;
  form.ca_cert_path = server.caCertPath;
  return form;
}

inline shared::McpServerConfig mcpServerFromForm(const McpModalForm &form) {
  shared::McpServerConfig cfg;
  cfg.transport = form.transport.empty() ? "stdio" : form.transport;
  cfg.enabled = form.enabled;
  cfg.command = trimMcpText(form.command);
  cfg.args = parseMcpArgs(form.args_text);
  cfg.env = parseMcpEnv(form.env_text);
  cfg.cwd = trimMcpText(form.cwd);
  cfg.url = trimMcpText(form.url);
  cfg.authHeader = trimMcpText(form.auth_header);
  if (cfg.authHeader.empty()) {
    cfg.authHeader = "Authorization";
  }
  cfg.authBearerToken = form.auth_bearer_token;
  cfg.allowInsecureTls = form.allow_insecure_tls;
  cfg.caCertPath = trimMcpText(form.ca_cert_path);
  return cfg;
}

inline McpSaveResult upsertMcpServer(std::map<std::string, shared::McpServerConfig> &servers,
                                     const std::string &original_name,
                                     const McpModalForm &form) {
  const std::string target_name = trimMcpText(form.name);
  if (target_name.empty()) {
    return McpSaveResult::EmptyName;
  }

  if (!original_name.empty() && original_name != target_name) {
    if (servers.count(target_name) > 0) {
      return McpSaveResult::RenameCollision;
    }
    servers.erase(original_name);
  } else if (original_name.empty() && servers.count(target_name) > 0) {
    return McpSaveResult::RenameCollision;
  }

  servers[target_name] = mcpServerFromForm(form);
  return McpSaveResult::Saved;
}

inline bool deleteMcpServer(std::map<std::string, shared::McpServerConfig> &servers,
                            const std::string &name) {
  return servers.erase(name) > 0;
}

inline bool toggleMcpServerEnabled(
    std::map<std::string, shared::McpServerConfig> &servers,
    const std::string &name) {
  auto it = servers.find(name);
  if (it == servers.end()) {
    return false;
  }
  it->second.enabled = !it->second.enabled;
  return true;
}

} // namespace firmius::tui
