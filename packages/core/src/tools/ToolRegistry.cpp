#include "tools/ToolRegistry.hpp"
#include "agents/Agent.hpp"
#include "utils/FSUtil.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace firmius::core {
using namespace firmius::shared;

namespace {

std::string normalizeObjectKey(const rapidjson::Value &key) {
  std::string normalized = key.GetString();
  if (normalized.size() >= 2 && normalized.front() == '"' &&
      normalized.back() == '"') {
    return normalized.substr(1, normalized.size() - 2);
  }
  return normalized;
}

void normalizeToolArgumentValue(const rapidjson::Value &input,
                                rapidjson::Value &output,
                                rapidjson::Document::AllocatorType &alloc) {
  if (input.IsObject()) {
    output.SetObject();
    for (auto it = input.MemberBegin(); it != input.MemberEnd(); ++it) {
      const std::string normalizedKey = normalizeObjectKey(it->name);
      rapidjson::Value key(normalizedKey.c_str(), alloc);
      rapidjson::Value normalizedValue;
      normalizeToolArgumentValue(it->value, normalizedValue, alloc);
      output.AddMember(key.Move(), normalizedValue.Move(), alloc);
    }
    return;
  }

  if (input.IsArray()) {
    output.SetArray();
    for (const auto &element : input.GetArray()) {
      rapidjson::Value normalizedElement;
      normalizeToolArgumentValue(element, normalizedElement, alloc);
      output.PushBack(normalizedElement.Move(), alloc);
    }
    return;
  }

  output.CopyFrom(input, alloc);
}

rapidjson::Document normalizeToolArguments(const rapidjson::Value &input) {
  rapidjson::Document normalized;
  rapidjson::Value normalizedRoot;
  normalizeToolArgumentValue(input, normalizedRoot, normalized.GetAllocator());
  normalized.CopyFrom(normalizedRoot, normalized.GetAllocator());
  return normalized;
}

bool isAllowedCompactWorkTool(const AgentPermissions &perms) {
  const auto &allowed = perms.allowedScopes;
  using firmius::shared::ToolScope;
  const ToolScope required[] = {
      ToolScope::PlanRead, ToolScope::PlanWrite, ToolScope::ChunkRead,
      ToolScope::ChunkWrite, ToolScope::ChunkAssign, ToolScope::ChunkReview};
  return std::any_of(std::begin(required), std::end(required),
                     [&](ToolScope scope) {
                       return std::find(allowed.begin(), allowed.end(), scope) !=
                              allowed.end();
                     });
}

} // namespace

void ToolRegistry::registerTool(std::unique_ptr<shared::ITool> tool) {
  auto meta = tool->getMetadata();
  std::lock_guard<std::mutex> lock(mutex_);
  tools[meta.name] = std::move(tool);
}

void ToolRegistry::registerToolFactory(const std::string &name,
                                       ToolFactory factory) {
  if (!factory)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  factories[name] = std::move(factory);
}

shared::ITool *ToolRegistry::getTool(const std::string &name) const {
  ToolFactory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools.find(name);
    if (it != tools.end()) {
      return it->second.get();
    }

    auto factoryIt = factories.find(name);
    if (factoryIt == factories.end()) {
      return nullptr;
    }
    factory = factoryIt->second;
  }

  auto tool = factory ? factory() : nullptr;
  if (!tool) {
    return nullptr;
  }

  auto meta = tool->getMetadata();
  std::lock_guard<std::mutex> lock(mutex_);
  auto existing = tools.find(name);
  if (existing != tools.end()) {
    return existing->second.get();
  }

  auto [insertedIt, inserted] = tools.emplace(meta.name, std::move(tool));
  return insertedIt != tools.end() ? insertedIt->second.get() : nullptr;
}

std::vector<shared::ToolMetadata> ToolRegistry::listToolMetadata() const {
  std::vector<shared::ToolMetadata> metas;
  std::vector<ToolFactory> pendingFactories;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &[name, tool] : tools) {
      metas.push_back(tool->getMetadata());
    }

    for (const auto &[name, factory] : factories) {
      if (tools.find(name) == tools.end()) {
        pendingFactories.push_back(factory);
      }
    }
  }

  for (const auto &factory : pendingFactories) {
    auto tool = factory ? factory() : nullptr;
    if (tool) {
      metas.push_back(tool->getMetadata());
    }
  }

  return metas;
}

std::vector<firmius::provider::ToolDefinition>
ToolRegistry::getAvailableToolDefinitions(const AgentPermissions &perms) const {
  std::vector<firmius::provider::ToolDefinition> defs;
  std::vector<ToolFactory> pendingFactories;
  auto &allowed = perms.allowedScopes;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &[name, tool] : tools) {
      auto meta = tool->getMetadata();
      const bool permitted =
          meta.name == "Work" ? isAllowedCompactWorkTool(perms)
                               : std::find(allowed.begin(), allowed.end(),
                                           meta.scope) != allowed.end();
      if (permitted) {
        defs.push_back({meta.name, meta.description,
                        tool->getSchema()->toString()});
      }
    }

    for (const auto &[name, factory] : factories) {
      if (tools.find(name) == tools.end()) {
        pendingFactories.push_back(factory);
      }
    }
  }

  for (const auto &factory : pendingFactories) {
    auto tool = factory ? factory() : nullptr;
    if (!tool) {
      continue;
    }
    auto meta = tool->getMetadata();
    const bool permitted =
        meta.name == "Work" ? isAllowedCompactWorkTool(perms)
                             : std::find(allowed.begin(), allowed.end(),
                                         meta.scope) != allowed.end();
    if (permitted) {
      defs.push_back({meta.name, meta.description, tool->getSchema()->toString()});
    }
  }

  return defs;
}

std::string ToolRegistry::getSchema(const std::string &name) const {
  auto *tool = getTool(name);
  if (tool) {
    return tool->getSchema()->toString();
  }
  return "";
}

shared::ToolResult ToolRegistry::execute(const std::string &name,
                                         const rapidjson::Value &input,
                                         shared::ToolContext &ctx) {
  auto *tool = getTool(name);
  if (!tool) {
    return shared::ToolResult::fail("Tool not found: " + name);
  }

  const auto &meta = tool->getMetadata();

  // Security check
  auto &perms = ctx.agent.getContext().permissions;
  const bool permitted =
      meta.name == "Work" ? isAllowedCompactWorkTool(perms)
                           : std::find(perms.allowedScopes.begin(),
                                       perms.allowedScopes.end(),
                                       meta.scope) != perms.allowedScopes.end();
  if (!permitted) {
    return shared::ToolResult::fail(
        "Permission denied: tool scope not allowed for " + name);
  }

  const rapidjson::Document normalizedInput = normalizeToolArguments(input);

  // Validation with breadcrumbs
  auto validation = tool->getSchema()->validate(normalizedInput);
  if (!validation.success) {
    return shared::ToolResult::fail(validation.violationToPretty());
  }

  shared::ToolResult result = tool->execute(normalizedInput, ctx);
  return truncateIfNecessary(result, ctx);
}

shared::ToolResult ToolRegistry::truncateIfNecessary(shared::ToolResult result,
                                                     shared::ToolContext &ctx) {
  const size_t threshold = 1024 * 512; // 512KB
  if (!result.success || result.data.size() <= threshold) {
    return result;
  }

  size_t originalSize = result.data.size();
  int lineCount = 0;
  for (char ch : result.data) {
    if (ch == '\n')
      lineCount++;
  }
  if (!result.data.empty() && result.data.back() != '\n') {
    lineCount++;
  }

  // Generate a unique filename in the uploads directory
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch())
                       .count();
  std::string filename = "tool_output_" + ctx.agent.getContext().identity.id +
                         "_" + std::to_string(timestamp) + ".json";
  std::string uploadsDir = "/mnt/SHIT/Projects/Firmius/uploads";
  std::filesystem::path fullPath = std::filesystem::path(uploadsDir) / filename;

  try {
    std::filesystem::create_directories(uploadsDir);
    std::ofstream ofs(fullPath);
    if (!ofs.is_open()) {
      throw std::runtime_error("Could not open file for writing: " +
                               fullPath.string());
    }
    ofs << result.data;
    ofs.close();

    // Create a summary result
    std::string peek;
    const size_t peekSize = 1024; // 1KB peek
    if (result.data.size() > peekSize) {
      peek = result.data.substr(0, peekSize) + "\n... [TRUNCATED]";
    } else {
      peek = result.data;
    }

    rapidjson::Document summaryDoc;
    summaryDoc.SetObject();
    auto &alloc = summaryDoc.GetAllocator();

    summaryDoc.AddMember(
        "info",
        rapidjson::Value("Tool result was too large and has been truncated.",
                         alloc)
            .Move(),
        alloc);
    summaryDoc.AddMember("original_byte_size", (uint64_t)originalSize, alloc);
    summaryDoc.AddMember("line_count", lineCount, alloc);
    summaryDoc.AddMember("full_output_path",
                         rapidjson::Value(fullPath.c_str(), alloc).Move(),
                         alloc);
    summaryDoc.AddMember("peek", rapidjson::Value(peek.c_str(), alloc).Move(),
                         alloc);

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    summaryDoc.Accept(writer);

    result.data = sb.GetString();
  } catch (const std::exception &e) {
    // If saving fails, we still truncate but inform about the failure
    result.error =
        "Output truncation failed to save full file: " + std::string(e.what());
    result.data = "{\"error\": \"Truncation failed to save full output.\"}";
  }

  return result;
}

} // namespace firmius::core
