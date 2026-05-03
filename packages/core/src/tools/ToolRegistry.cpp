#include "tools/ToolRegistry.hpp"
#include "agents/Agent.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "workflow/WorkflowLoader.hpp"
#include "utils/FSUtil.hpp"
#include "utils/JSONSchemaFromJson.hpp"
#include "utils/StringUtil.hpp"
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

bool hasWrappedJsonKeys(const rapidjson::Value &v) {
  if (v.IsObject()) {
    for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
      const char *str = it->name.GetString();
      const auto len = it->name.GetStringLength();
      if (len >= 2 && str[0] == '"' && str[len - 1] == '"') {
        return true;
      }
      if (hasWrappedJsonKeys(it->value)) {
        return true;
      }
    }
    return false;
  }
  if (v.IsArray()) {
    for (const auto &element : v.GetArray()) {
      if (hasWrappedJsonKeys(element)) {
        return true;
      }
    }
  }
  return false;
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

std::string workActionForLegacyToolName(const std::string &name) {
  if (name == "plan_create") return "CreatePlan";
  if (name == "plan_list") return "ListPlans";
  if (name == "plan_get") return "GetPlan";
  if (name == "plan_update") return "UpdatePlan";
  if (name == "plan_set_active") return "ActivatePlan";
  if (name == "chunk_add") return "AddChunk";
  if (name == "chunk_list") return "ListChunks";
  if (name == "chunk_get") return "GetChunk";
  if (name == "chunk_update") return "UpdateChunk";
  if (name == "chunk_ready_for_execution") return "ReadyChunk";
  return "";
}

std::string canonicalWorkAction(const std::string &action) {
  if (action == "create_plan" || action == "plan_create") return "CreatePlan";
  if (action == "list_plans" || action == "plan_list") return "ListPlans";
  if (action == "get_plan" || action == "plan_get") return "GetPlan";
  if (action == "update_plan" || action == "plan_update") return "UpdatePlan";
  if (action == "activate_plan" || action == "set_active_plan" ||
      action == "plan_set_active") return "ActivatePlan";
  if (action == "add_chunk" || action == "chunk_add") return "AddChunk";
  if (action == "list_chunks" || action == "chunk_list") return "ListChunks";
  if (action == "get_chunk" || action == "chunk_get") return "GetChunk";
  if (action == "update_chunk" || action == "chunk_update") return "UpdateChunk";
  if (action == "ready_chunk" || action == "ready_chunks" ||
      action == "chunk_ready_for_execution") return "ReadyChunk";
  return action;
}

std::optional<ToolScope> toolScopeFromString(const std::string &scope) {
  const std::string lowered = shared::StringUtil::toLower(shared::StringUtil::trim(scope));
  if (lowered == "filesystemread") return ToolScope::FilesystemRead;
  if (lowered == "filesystemwrite") return ToolScope::FilesystemWrite;
  if (lowered == "process") return ToolScope::Process;
  if (lowered == "semantic") return ToolScope::Semantic;
  if (lowered == "delegation") return ToolScope::Delegation;
  if (lowered == "web") return ToolScope::Web;
  if (lowered == "planread") return ToolScope::PlanRead;
  if (lowered == "planwrite") return ToolScope::PlanWrite;
  if (lowered == "chunkread") return ToolScope::ChunkRead;
  if (lowered == "chunkwrite") return ToolScope::ChunkWrite;
  if (lowered == "chunkassign") return ToolScope::ChunkAssign;
  if (lowered == "chunkreview") return ToolScope::ChunkReview;
  return std::nullopt;
}

class WorkflowDefinedTool final : public shared::ITool {
public:
  explicit WorkflowDefinedTool(const Workflow &workflow) : workflow_(workflow) {}

  ToolMetadata getMetadata() const override {
    ToolMetadata meta;
    meta.name = workflow_.definesTool ? workflow_.definesTool->name : workflow_.id;
    meta.description = workflow_.definesTool ? workflow_.definesTool->description
                                             : workflow_.description;
    meta.scope = workflow_.definesTool && toolScopeFromString(workflow_.definesTool->requiredScope).has_value()
                     ? *toolScopeFromString(workflow_.definesTool->requiredScope)
                     : ToolScope::Semantic;
    return meta;
  }

  std::shared_ptr<JSONSchema> getSchema() const override {
    if (workflow_.definesTool.has_value() &&
        !workflow_.definesTool->schemaJson.empty()) {
      auto parsed = firmius::shared::jsonSchemaFromString(workflow_.definesTool->schemaJson);
      if (parsed) {
        return parsed;
      }
    }
    return zObject({
        {"payload", zString()->setOptional()->describe("Opaque payload for workflow-defined tool invocation")},
        {"path", zString()->setOptional()},
        {"brief", zString()->setOptional()},
    });
  }

  ToolResult execute(const rapidjson::Value &input, ToolContext &ctx) override {
    // Workflow-defined tools execute the same hook workflow action chain,
    // but in a "manual" tool-call context rather than an event trigger.

    // Bind hook state to the current thread so thread-scoped writes persist.
    hooks::HookState::instance().bindThread(ctx.agent.getContext().history->threadId);

    hooks::EventPayload payload;
    payload.threadId = ctx.agent.getContext().history->threadId;
    payload.agentId = ctx.agent.getContext().identity.id;
    payload.persona = ctx.agent.getContext().config.personaName;
    payload.activeMode = ctx.agent.getContext().state.activeMode;
    payload.toolName = getMetadata().name;
    payload.completedWorkflowId = workflow_.id;

    // Tool args: use the resolved input as JSON string.
    {
      rapidjson::StringBuffer sb;
      rapidjson::Writer<rapidjson::StringBuffer> w(sb);
      input.Accept(w);
      payload.toolArgsJson = std::string(sb.GetString(), sb.GetSize());
    }

    hooks::HookOutcome outcome = hooks::HookDispatcher::runAction(workflow_, payload);

    // Settle side-effects (state writes) regardless of return.
    hooks::HookDispatcher::settleOutcome(workflow_, outcome);

    // Structured tool return payload.
    rapidjson::Document out;
    out.SetObject();
    auto &alloc = out.GetAllocator();

    out.AddMember("tool_name",
                  rapidjson::Value(getMetadata().name.c_str(), alloc).Move(), alloc);
    out.AddMember("workflow_id",
                  rapidjson::Value(workflow_.id.c_str(), alloc).Move(), alloc);
    rapidjson::Value argsCopy;
    argsCopy.CopyFrom(input, alloc);
    out.AddMember("args", argsCopy, alloc);
    if (!payload.completedWorkflowId.empty()) {
      out.AddMember("completed_workflow",
                    rapidjson::Value(payload.completedWorkflowId.c_str(), alloc).Move(), alloc);
    }

    if (!outcome.outcomeLabel.empty()) {
      out.AddMember("outcome",
                    rapidjson::Value(outcome.outcomeLabel.c_str(), alloc).Move(), alloc);
    }

    if (!outcome.toolReturnPayloadJson.empty()) {
      rapidjson::Document ret;
      if (!ret.Parse(outcome.toolReturnPayloadJson.c_str()).HasParseError() &&
          ret.IsObject()) {
        out.AddMember("return", ret.Move(), alloc);
      }
    }

    // Also include a compact effect report for observability/tests.
    rapidjson::Value effects(rapidjson::kObjectType);
    effects.AddMember("state_writes", static_cast<uint64_t>(outcome.stateWrites.size()), alloc);
    out.AddMember("effects", effects, alloc);

    return ToolResult::ok(out);
  }

private:
  Workflow workflow_;
};

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

void ToolRegistry::registerWorkflowDefinedTools() {
  const auto workflows = WorkflowLoader::instance().getAllWorkflows();
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &workflow : workflows) {
    if (!workflow.definesTool.has_value()) {
      continue;
    }
    const auto toolName = workflow.definesTool->name;
    if (toolName.empty()) {
      continue;
    }
    factories[toolName] = [workflow]() {
      return std::make_unique<WorkflowDefinedTool>(workflow);
    };
  }
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

std::optional<shared::ToolMetadata>
ToolRegistry::getMetadataFor(const std::string &name) const {
  const std::string legacyWorkAction = workActionForLegacyToolName(name);
  const std::string resolvedName = legacyWorkAction.empty() ? name : "Work";
  auto *tool = getTool(resolvedName);
  if (!tool) {
    return std::nullopt;
  }
  return tool->getMetadata();
}

shared::ToolResult ToolRegistry::execute(const std::string &name,
                                         const rapidjson::Value &input,
                                         shared::ToolContext &ctx) {
  const std::string legacyWorkAction = workActionForLegacyToolName(name);
  const std::string resolvedName = legacyWorkAction.empty() ? name : "Work";
  auto *tool = getTool(resolvedName);
  if (!tool) {
    return shared::ToolResult::fail("Tool not found: " + name);
  }

  const auto &meta = tool->getMetadata();

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

  rapidjson::Document normalizedDoc;
  const rapidjson::Value *resolvedInput = &input;
  if (hasWrappedJsonKeys(input)) {
    normalizedDoc = normalizeToolArguments(input);
    resolvedInput = &normalizedDoc;
  }

  rapidjson::Document legacyWorkDoc;
  if (!legacyWorkAction.empty()) {
    legacyWorkDoc.CopyFrom(*resolvedInput, legacyWorkDoc.GetAllocator());
    if (!legacyWorkDoc.IsObject()) {
      return shared::ToolResult::fail("Work input must be an object");
    }
    if (!legacyWorkDoc.HasMember("action")) {
      legacyWorkDoc.AddMember(
          "action",
          rapidjson::Value(legacyWorkAction.c_str(), legacyWorkDoc.GetAllocator()),
          legacyWorkDoc.GetAllocator());
    }
    resolvedInput = &legacyWorkDoc;
  }

  rapidjson::Document canonicalWorkDoc;
  if (meta.name == "Work" && resolvedInput->IsObject() &&
      resolvedInput->HasMember("action") && (*resolvedInput)["action"].IsString()) {
    const std::string canonical =
        canonicalWorkAction((*resolvedInput)["action"].GetString());
    if (canonical != (*resolvedInput)["action"].GetString()) {
      canonicalWorkDoc.CopyFrom(*resolvedInput, canonicalWorkDoc.GetAllocator());
      canonicalWorkDoc["action"].SetString(
          canonical.c_str(), static_cast<rapidjson::SizeType>(canonical.size()),
          canonicalWorkDoc.GetAllocator());
      resolvedInput = &canonicalWorkDoc;
    }
  }

  auto validation = tool->getSchema()->validate(*resolvedInput);
  if (!validation.success) {
    return shared::ToolResult::fail(validation.violationToPretty());
  }

  shared::ToolResult result = tool->execute(*resolvedInput, ctx);
  return truncateIfNecessary(result, ctx);
}

shared::ToolResult ToolRegistry::truncateIfNecessary(shared::ToolResult result,
                                                     shared::ToolContext &ctx) {
  const size_t threshold = 1024 * 512;
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

    std::string peek;
    const size_t peekSize = 1024;
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
    result.error =
        "Output truncation failed to save full file: " + std::string(e.what());
    result.data = "{\"error\": \"Truncation failed to save full output.\"}";
  }

  return result;
}

} // namespace firmius::core
