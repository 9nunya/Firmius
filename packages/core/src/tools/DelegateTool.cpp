#include "tools/DelegateTool.hpp"

#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "Engine.hpp"
#include "environment/PermissionSuggestionEngine.hpp"
#include "harness/Harness.hpp"
#include "Events.hpp"
#include "Serialization.hpp"
#include "agents/PurposeLoader.hpp"
#include "artifacts/ReferenceExpansion.hpp"
#include "utils/PlatformPaths.hpp"
#include "utils/StringUtil.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <unordered_set>

namespace firmius::core {
using namespace firmius::shared;

namespace {

static constexpr int kDelegatePollIntervalMs = 50;

enum class DelegationPersonaKind { General, Coder, Reviewer, Explorer };

DelegationPersonaKind personaKindForName(const std::string &persona) {
  const std::string lowered = shared::StringUtil::toLower(shared::StringUtil::trim(persona));
  if (lowered == "coder") {
    return DelegationPersonaKind::Coder;
  }
  if (lowered == "reviewer" || lowered == "shrike") {
    return DelegationPersonaKind::Reviewer;
  }
  if (lowered == "explorer") {
    return DelegationPersonaKind::Explorer;
  }
  return DelegationPersonaKind::General;
}

std::optional<std::string> legacyPersonaSuggestion(const std::string &persona) {
  const std::string lowered = shared::StringUtil::toLower(shared::StringUtil::trim(persona));
  if (lowered == "implementer") return "legacy role 'implementer'; use 'coder'";
  if (lowered == "researcher") return "legacy role 'researcher'; use 'explorer'";
  if (lowered == "executor") return "legacy role 'executor'; use 'coder'";
  if (lowered == "worker") return "legacy role 'worker'; use 'coder'";
  if (lowered == "auditor") return "legacy role 'auditor'; use 'reviewer'";
  if (lowered == "scout") return "legacy role 'scout'; use 'explorer'";
  return std::nullopt;
}

bool isExplorerLikeKind(DelegationPersonaKind kind) { return kind == DelegationPersonaKind::Explorer; }

std::optional<std::string> normalizeOptionalString(const std::optional<std::string> &value) {
  if (!value.has_value()) return std::nullopt;
  const std::string trimmed = shared::StringUtil::trim(*value);
  if (trimmed.empty()) return std::nullopt;
  return trimmed;
}

bool callerMayUseDreamMode(const shared::ToolContext &ctx) {
  const std::string lowered = shared::StringUtil::toLower(shared::StringUtil::trim(ctx.agent.getContext().config.personaName));
  return lowered == "aster" || lowered == "fast" || lowered == "harbor" || lowered == "lead" || lowered == "hotrun";
}

bool isRetryableWaitOutcome(const AgentOutcome &outcome) {
  return outcome.kind == AgentOutcome::Kind::NoSummary || outcome.kind == AgentOutcome::Kind::Failed;
}

std::string normalizeRouteToken(const std::string& value) { return shared::StringUtil::toLower(shared::StringUtil::trim(value)); }

bool userExplicitlyRequestedCategory(const shared::ToolContext& ctx, const std::string& category) {
  const auto& agentContext = ctx.agent.getContext();
  if (!agentContext.history) return false;
  const std::string normalizedCategory = shared::StringUtil::toLower(shared::StringUtil::trim(category));
  if (normalizedCategory.empty()) return false;
  for (auto turnIt = agentContext.history->turns.rbegin(); turnIt != agentContext.history->turns.rend(); ++turnIt) {
    for (auto msgIt = turnIt->messages.rbegin(); msgIt != turnIt->messages.rend(); ++msgIt) {
      if (msgIt->role != shared::Role::User) continue;
      for (const auto& part : msgIt->content) {
        if (const auto* txt = std::get_if<shared::TextContent>(&part)) {
          const std::string lowered = normalizeRouteToken(txt->text);
          if (lowered.find(normalizedCategory) != std::string::npos) return true;
        }
      }
    }
  }
  return false;
}

std::string buildWorkerTask(const std::string &task, const std::optional<shared::WorkTask> &workTask) {
  std::ostringstream prompt;
  prompt << "You are a helper supporting your parent coder on a bounded subtask.\n\nBoundaries\n- You do not own the full route.\n- You are not responsible for the whole task.\n- Complete only the bounded subtask below and return useful results to the coder.\n\n";
  if (workTask.has_value()) {
    prompt << "Assigned Task\nTask ID: " << workTask->id << "\nTask Title: " << workTask->title << "\nTask Goal: " << workTask->goal << "\n";
    if (!workTask->notes.empty()) prompt << "Task Notes: " << workTask->notes << "\n";
    if (!workTask->verificationCondition.empty()) prompt << "Verification: " << workTask->verificationCondition << "\n";
    prompt << "\n";
  }
  prompt << "Subtask\n" << task << "\n";
  return prompt.str();
}




std::string buildDelegationTask(const std::string &/*persona*/, const std::string &task, const std::optional<std::string> &plan_id, const std::optional<std::string> &chunk_id, const std::optional<std::string> &task_id, const std::string &threadId, DelegationPersonaKind kind) {
  (void)plan_id;
  (void)chunk_id;
  (void)task_id;
  (void)threadId;
  if (isExplorerLikeKind(kind)) return buildWorkerTask(task, std::nullopt);
  return task;
}

std::string buildDreamerTask(const std::string &task, const std::optional<std::string> &plan_id, const std::string &threadId, const shared::ToolContext &ctx, const std::string &memoryRoot) {
  std::ostringstream prompt;
  prompt << "You are being summoned in restricted dream mode by a lead agent.\n\nDream Sandbox\n- Working directory: " << memoryRoot << "\n- Read/write only under that directory.\n- Do not modify the project repository itself.\n- Prefer USER.md, BEHAVIOR.md, and project-specific notes under projects/.\n\n";
  const auto &parentCtx = ctx.agent.getContext();
  prompt << "Lead Context\n- Parent persona: " << parentCtx.config.personaName << "\n- Source workspace: " << parentCtx.environment.cwd << "\n- Thread ID: " << threadId << "\n\n";
  (void)plan_id;
  prompt << "Lead Dream Request\n" << task << "\n";
  return prompt.str();
}

struct ResolvedRoute {
  std::string providerId;
  std::string modelId;
  std::string variantName;
  std::string categoryName;
  std::string warning;
};

ResolvedRoute resolveModelRoute(const std::string &persona, const std::optional<std::string>& explicitCategoryOverride = std::nullopt, const std::string& explicitCategoryWarning = "") {
  const auto &config = shared::ConfigLoader::instance().getConfig();
  auto useDefaultRoute = [&config]() { return ResolvedRoute{config.defaultProviderId, config.defaultModelId, config.defaultModelVariant, "", ""}; };
  auto findCategory = [&config](const std::string &name) -> const shared::ModelRouteCategory * {
    auto it = config.modelRouterCategories.find(name);
    return it == config.modelRouterCategories.end() ? nullptr : &it->second;
  };
  if (explicitCategoryOverride.has_value() && !explicitCategoryOverride->empty()) {
    if (const auto *category = findCategory(*explicitCategoryOverride)) {
      if (!category->models.empty()) {
        const auto &opt = category->models.front();
        return {opt.providerId, opt.modelId, opt.variantName, *explicitCategoryOverride, explicitCategoryWarning};
      }
    }
    auto route = useDefaultRoute();
    route.warning = "Category '" + *explicitCategoryOverride + "' not found; using default model route.";
    return route;
  }
  auto it_purpose = config.purposeRoutes.find(persona);
  if (it_purpose != config.purposeRoutes.end() && !it_purpose->second.empty()) {
    const std::string mapped_category = it_purpose->second;
    if (const auto *category = findCategory(mapped_category)) {
      if (!category->models.empty()) {
        const auto &opt = category->models.front();
        return {opt.providerId, opt.modelId, opt.variantName, mapped_category, ""};
      }
    }
    auto route = useDefaultRoute();
    route.warning = "Purpose route for '" + persona + "' points to missing category '" + mapped_category + "'; using default model route.";
    return route;
  }
  if (!config.defaultRouteCategory.empty()) {
    if (const auto *category = findCategory(config.defaultRouteCategory)) {
      if (!category->models.empty()) {
        const auto &opt = category->models.front();
        return {opt.providerId, opt.modelId, opt.variantName, config.defaultRouteCategory, ""};
      }
    }
  }
  return useDefaultRoute();
}

std::vector<ResolvedRoute> buildRouteCandidates(const std::string &persona, const std::optional<std::string>& explicitCategoryOverride = std::nullopt, const std::string& explicitCategoryWarning = "") {
  const auto &config = shared::ConfigLoader::instance().getConfig();
  std::vector<ResolvedRoute> routes;
  std::unordered_set<std::string> seen;
  auto pushUnique = [&](const ResolvedRoute &route) {
    std::string key = route.providerId + "|" + route.modelId + "|" + route.variantName + "|" + route.categoryName;
    if (seen.insert(key).second) routes.push_back(route);
  };
  ResolvedRoute primary = resolveModelRoute(persona, explicitCategoryOverride, "");
  if (!explicitCategoryWarning.empty()) primary.warning = explicitCategoryWarning + (primary.warning.empty() ? "" : " " + primary.warning);
  pushUnique(primary);
  if (!config.enableSubagentRouteFallback) return routes;
  std::vector<std::string> fallbackCategories = config.subagentRouteFallbackOrder;
  if (fallbackCategories.empty()) { for (const auto &[name, _] : config.modelRouterCategories) fallbackCategories.push_back(name); }
  for (const auto &name : fallbackCategories) {
    auto it = config.modelRouterCategories.find(name);
    if (it == config.modelRouterCategories.end() || it->second.models.empty()) continue;
    const auto &opt = it->second.models.front();
    pushUnique(ResolvedRoute{opt.providerId, opt.modelId, opt.variantName, name, ""});
  }
  return routes;
}

// Token-waste pass 5: routing metadata is debug breadcrumbs in the
// success path; only surface it when something interesting happened
// (a warning was set, or fallback was used). attempted_categories was
// always emitted as an array; now we collapse to a single
// `routing_fallback` flag when relevant.
void appendRoutingMetadata(rapidjson::Document &d, const ResolvedRoute &route, const std::vector<std::string> &attemptedCategories, bool fallbackUsed) {
  auto &a = d.GetAllocator();
  if (!route.categoryName.empty()) d.AddMember("category", rapidjson::Value(route.categoryName.c_str(), a).Move(), a);
  if (fallbackUsed) {
    d.AddMember("routing_fallback", true, a);
    // Include attempted_categories ONLY when fallback was used — this is
    // when the model needs to know the chain of categories tried.
    rapidjson::Value attempted(rapidjson::kArrayType);
    for (const auto &category : attemptedCategories) attempted.PushBack(rapidjson::Value(category.c_str(), a).Move(), a);
    d.AddMember("attempted_categories", attempted, a);
  }
  if (!route.warning.empty()) d.AddMember("routing_warning", rapidjson::Value(route.warning.c_str(), a).Move(), a);
}

// Token-waste pass 5: outcome artifacts are emitted as bare reference
// strings instead of full ThreadArtifactMetadata blocks. The model uses
// these references to fetch the artifact later; the metadata struct is
// rebuilt from disk if/when needed.
void appendOutcomeArtifacts(rapidjson::Document &d, const shared::AgentOutcome &outcome) {
  auto &a = d.GetAllocator();
  auto appendArray = [&](const char *key, const std::vector<shared::ThreadArtifactMetadata> &items) {
    if (items.empty()) return;  // omit empty arrays entirely
    rapidjson::Value array(rapidjson::kArrayType);
    for (const auto &artifact : items) {
      const std::string owner = artifact.ownerFriendlyName.empty() ? artifact.ownerAgentId : artifact.ownerFriendlyName;
      const std::string reference = "@artifact:" + owner + "/" + artifact.filename;
      array.PushBack(rapidjson::Value(reference.c_str(), a).Move(), a);
    }
    d.AddMember(rapidjson::Value(key, a).Move(), array, a);
  };
  appendArray("artifacts_created", outcome.artifacts_created);
  appendArray("artifacts_updated", outcome.artifacts_updated);
}

shared::ToolResult failWithStructuredData(const rapidjson::Document &d, const std::string &error) {
  shared::ToolResult result = shared::ToolResult::fail(error);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  result.data = buffer.GetString();
  return result;
}

shared::ToolResult executeSpawn(const rapidjson::Value &input, shared::ToolContext &ctx) {
  bool dream = false;
  if (input.HasMember("dream") && input["dream"].IsBool()) dream = input["dream"].GetBool();

  std::string persona;
  if (dream) {
    if (!callerMayUseDreamMode(ctx)) return shared::ToolResult::fail("dream summon mode is restricted to lead, fast, or harbor agents");
    persona = "dreamer";
  } else if (input.HasMember("persona") && input["persona"].IsString()) {
    persona = shared::StringUtil::trim(std::string(input["persona"].GetString()));
  } else {
    return shared::ToolResult::fail("Missing required field: persona");
  }

  if (!PurposeLoader::isValid(persona)) {
    if (auto suggestion = legacyPersonaSuggestion(persona)) return shared::ToolResult::fail("Invalid persona: '" + persona + "'. " + *suggestion + ".");
    return shared::ToolResult::fail("Invalid persona: '" + persona + "'. Check available personas in base.md or prompts/ directory.");
  }

  std::string task;
  if (input.HasMember("task") && input["task"].IsString()) task = input["task"].GetString();
  else return shared::ToolResult::fail("Missing required field: task");

  std::string name;
  if (input.HasMember("name") && input["name"].IsString()) name = input["name"].GetString();
  else return shared::ToolResult::fail("Missing required field: name");

  std::string title;
  if (input.HasMember("title") && input["title"].IsString()) title = input["title"].GetString();
  else return shared::ToolResult::fail("Missing required field: title");

  std::optional<std::string> agent_id;
  if (input.HasMember("agent_id") && input["agent_id"].IsString()) agent_id = normalizeOptionalString(input["agent_id"].GetString());

  std::optional<std::string> category;
  if (input.HasMember("category") && input["category"].IsString()) category = normalizeOptionalString(input["category"].GetString());

  bool isAsync = false;
  if (input.HasMember("async") && input["async"].IsBool()) isAsync = input["async"].GetBool();

  DelegationPersonaKind personaKind = personaKindForName(persona);
  std::string threadId = ctx.agent.getContext().history->threadId;

  // ── agent.spawn policy gate ───────────────────────────────────────
  // Subagent spawns can fan out work the user didn't intend. The gate
  // lets users pin "executor only from lead" or block paranoid configs
  // entirely. Defaults to allow (built-in category default) so existing
  // workflows aren't broken until the user opts into stricter rules.
  {
    PolicyRequest sreq;
    sreq.category = kCatAgentSpawn;
    sreq.persona = persona;
    sreq.parentPersona = ctx.agent.getContext().identity.role;
    sreq.toolName = "Delegate";
    auto eval = Harness::instance().policyEngine().evaluate(sreq);
    if (eval.decision == PolicyDecision::Deny) {
      return shared::ToolResult::fail(
          "Subagent spawn denied by policy: persona=" + persona);
    }
    if (eval.decision == PolicyDecision::Ask) {
      shared::PermissionEscalationRequest esc;
      esc.threadId = threadId;
      esc.agentId = ctx.agent.getContext().identity.id;
      esc.toolName = "Delegate";
      esc.requestType = shared::PermissionRequestType::Read;
      esc.title = "Spawn subagent " + persona + "?";
      esc.message = "Approve spawning persona '" + persona +
                    "' from agent '" + sreq.parentPersona + "'.";
      esc.severity = shared::CommandSeverity::LOW;
      esc.allowAlways = true;
      esc.category = sreq.category;
      esc.persona = persona;
      esc.parentPersona = sreq.parentPersona;
      shared::CommandIntent dummy;
      auto suggestions =
          PermissionSuggestionEngine::generate(sreq, dummy);
      auto response =
          Harness::instance()
              .requestPermissionEscalationWithSuggestions(
                  std::move(esc), std::move(suggestions));
      if (response == shared::PermissionResponse::Deny) {
        return shared::ToolResult::fail(
            "Subagent spawn denied: persona=" + persona);
      }
    }
  }

  std::string delegatedTask;
  std::optional<SummonAgentOverrides> summonOverrides;
  try {
    std::string builtTask;
    if (dream) {
      const std::string memoryRoot = (firmius::shared::PlatformPaths::firmiusHomeDir() / "user").string();
      std::filesystem::create_directories(memoryRoot);
      builtTask = buildDreamerTask(task, std::nullopt, threadId, ctx, memoryRoot);
      summonOverrides = SummonAgentOverrides{.cwdOverride = memoryRoot, .allowedPathsOverride = std::vector<std::string>{memoryRoot, memoryRoot + "/**"}};
    } else {
      builtTask = buildDelegationTask(persona, task, std::nullopt, std::nullopt, std::nullopt, threadId, personaKind);
    }
    const std::string cwd = ctx.agent.getContext().environment.cwd;
    delegatedTask = firmius::core::artifacts::expandInboundReferences(threadId, cwd, builtTask);
  } catch (const std::exception &e) { return shared::ToolResult::fail("Reference expansion failed: " + std::string(e.what())); }

  auto existingAgents = AgentRegistry::instance().listAll();
  for (const auto &aid : existingAgents) {
    auto ag = AgentRegistry::instance().getAgent(aid);
    if (ag && ag->getContext().identity.friendlyName == name && (!agent_id.has_value() || aid != *agent_id)) return shared::ToolResult::fail("Agent name '" + name + "' already exists in this thread");
  }

  std::optional<std::string> explicitCategoryOverride;
  std::string explicitCategoryWarning;
  if (category.has_value() && !category->empty()) {
    if (userExplicitlyRequestedCategory(ctx, *category)) explicitCategoryOverride = category;
    else explicitCategoryWarning = "Ignored explicit category '" + *category + "' because only user-specified route-category overrides are honored; using configured purpose/default routing.";
  }

  const std::vector<ResolvedRoute> routes = buildRouteCandidates(persona, explicitCategoryOverride, explicitCategoryWarning);
  std::vector<std::string> attemptedCategories;
  const bool isRetaskingExistingAgent = agent_id.has_value() && !agent_id->empty();
  std::string reusableAgentId = isRetaskingExistingAgent ? *agent_id : shared::StringUtil::generateUuid();
  bool agentExists = isRetaskingExistingAgent;

  auto waitForOutcome = [&](const std::string &aid) -> std::optional<AgentOutcome> {
    while (true) {
      auto outcome = Engine::instance().waitForAgentOutcome(aid, std::chrono::milliseconds(20));
      if (outcome.has_value()) return outcome;
      if (ctx.cancelRequested()) { Engine::instance().cancelAgent(aid); return AgentOutcome{AgentOutcome::Kind::Cancelled, "Cancelled by parent."}; }
    }
  };

  auto waitForOutcomeWithTimeout = [&](const std::string &aid, std::chrono::milliseconds timeout) -> std::optional<AgentOutcome> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto outcome = Engine::instance().peekAgentOutcome(aid, std::chrono::milliseconds(20));
      if (outcome.has_value()) return outcome;
      if (ctx.cancelRequested()) { Engine::instance().cancelAgent(aid); return AgentOutcome{AgentOutcome::Kind::Cancelled, "Cancelled by parent."}; }
    }
    return std::nullopt;
  };

  auto buildWaitResult = [&](const std::string &aid, const ResolvedRoute &route, const AgentOutcome &outcome, bool fallbackUsed) -> shared::ToolResult {
    rapidjson::Document d; d.SetObject(); auto &a = d.GetAllocator();
    d.AddMember("agentId", rapidjson::Value(aid.c_str(), a).Move(), a);
    appendRoutingMetadata(d, route, attemptedCategories, fallbackUsed);
    appendOutcomeArtifacts(d, outcome);
    if (outcome.kind == AgentOutcome::Kind::Cancelled) {
      d.AddMember("status", "cancelled", a); d.AddMember("result", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
      return shared::ToolResult::ok(d);
    }
    if (outcome.kind == AgentOutcome::Kind::Failed) {
      d.AddMember("status", "failed", a); d.AddMember("error", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
      return failWithStructuredData(d, outcome.text);
    }
    if (outcome.kind == AgentOutcome::Kind::NoSummary) {
      d.AddMember("status", "completed_no_summary", a); d.AddMember("result", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
      return shared::ToolResult::ok(d);
    }
    d.AddMember("status", "completed", a); d.AddMember("result", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
    return shared::ToolResult::ok(d);
  };

  for (std::size_t i = 0; i < routes.size(); ++i) {
    const auto &route = routes[i];
    attemptedCategories.push_back(route.categoryName.empty() ? "default" : route.categoryName);
    if (agentExists) {
      auto ag = AgentRegistry::instance().getAgent(reusableAgentId);
      if (!ag) return shared::ToolResult::fail("Agent not found: " + reusableAgentId);
      if (ag->getContext().state.currentStatus != AgentStatus::Idle) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (ag->getContext().state.currentStatus != AgentStatus::Idle && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(kDelegatePollIntervalMs));
        if (ag->getContext().state.currentStatus != AgentStatus::Idle) return shared::ToolResult::fail("Agent is busy (status: " + std::to_string(static_cast<int>(ag->getContext().state.currentStatus)) + ")");
      }
      try {
        Engine::instance().switchAgentModel(reusableAgentId, route.providerId, route.modelId, route.variantName);
        Engine::instance().executeTask(reusableAgentId, delegatedTask);
      } catch (const std::exception &) { if (i + 1 < routes.size()) continue; return shared::ToolResult::fail("Failed to launch subagent run on all configured routes."); }
      if (isAsync) {
        auto immediate = waitForOutcomeWithTimeout(reusableAgentId, std::chrono::milliseconds(250));
        if (immediate.has_value()) {
          if (isRetryableWaitOutcome(*immediate) && i + 1 < routes.size()) continue;
          if (immediate->kind == AgentOutcome::Kind::Cancelled) return buildWaitResult(reusableAgentId, route, *immediate, i > 0);
        }
        rapidjson::Document d; d.SetObject(); auto &a = d.GetAllocator();
        d.AddMember("agentId", rapidjson::Value(reusableAgentId.c_str(), a).Move(), a);
        d.AddMember("status", "re-tasked", a);
        appendRoutingMetadata(d, route, attemptedCategories, i > 0);
        return shared::ToolResult::ok(d);
      }
      auto outcome = waitForOutcome(reusableAgentId);
      if (!outcome.has_value()) return shared::ToolResult::fail("Parent agent interrupted while waiting for subagent.");
      if (isRetryableWaitOutcome(*outcome) && i + 1 < routes.size()) continue;
      return buildWaitResult(reusableAgentId, route, *outcome, i > 0);
    }
    try {
      reusableAgentId = Engine::instance().summonAgent(threadId, persona, delegatedTask, true, ctx.agent.getContext().identity.id, name, title, reusableAgentId, route.providerId, route.modelId, route.variantName, {}, summonOverrides);
      agentExists = true;
    } catch (const std::exception &) { if (i + 1 < routes.size()) continue; return shared::ToolResult::fail("Failed to summon subagent on all configured routes."); }
    if (isAsync) {
      auto immediate = waitForOutcomeWithTimeout(reusableAgentId, std::chrono::milliseconds(250));
      if (immediate.has_value()) {
        if (isRetryableWaitOutcome(*immediate) && i + 1 < routes.size()) { agentExists = false; continue; }
        if (immediate->kind == AgentOutcome::Kind::Cancelled) return buildWaitResult(reusableAgentId, route, *immediate, i > 0);
      }
      rapidjson::Document d; d.SetObject(); auto &a = d.GetAllocator();
      d.AddMember("agentId", rapidjson::Value(reusableAgentId.c_str(), a).Move(), a);
      d.AddMember("status", "spawned", a);
      appendRoutingMetadata(d, route, attemptedCategories, i > 0);
      return shared::ToolResult::ok(d);
    }
    auto outcome = waitForOutcome(reusableAgentId);
    if (!outcome.has_value()) return shared::ToolResult::fail("Parent agent interrupted while waiting for subagent.");
    if (isRetryableWaitOutcome(*outcome) && i + 1 < routes.size()) { agentExists = false; continue; }
    return buildWaitResult(reusableAgentId, route, *outcome, i > 0);
  }
  return shared::ToolResult::fail("Subagent run failed or returned no usable summary on all routes.");
}

shared::ToolResult executeWait(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string agent_id;
  if (input.HasMember("agent_id") && input["agent_id"].IsString()) agent_id = input["agent_id"].GetString();
  else return shared::ToolResult::fail("Missing required field: agent_id");

  std::optional<AgentOutcome> outcome;
  while (true) {
    outcome = Engine::instance().waitForAgentOutcome(agent_id, std::chrono::milliseconds(25));
    if (outcome.has_value()) break;
    if (ctx.cancelRequested()) { Engine::instance().cancelAgent(agent_id); return shared::ToolResult::fail("Parent agent interrupted while waiting for subagent."); }
  }
  rapidjson::Document d; d.SetObject(); auto& a = d.GetAllocator();
  d.AddMember("agentId", rapidjson::Value(agent_id.c_str(), a).Move(), a);
  {
    auto agent = AgentRegistry::instance().getAgent(agent_id);
    const std::string friendlyName = agent && !agent->getContext().identity.friendlyName.empty() ? agent->getContext().identity.friendlyName : "";
    d.AddMember("friendlyName", rapidjson::Value(friendlyName.c_str(), a).Move(), a);
  }
  appendOutcomeArtifacts(d, *outcome);
  if (outcome->kind == AgentOutcome::Kind::Cancelled) {
    d.AddMember("status", "cancelled", a); d.AddMember("result", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
    return shared::ToolResult::ok(d);
  }
  if (outcome->kind == AgentOutcome::Kind::Failed) {
    d.AddMember("status", "failed", a); d.AddMember("error", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
    return failWithStructuredData(d, outcome->text);
  }
  if (outcome->kind == AgentOutcome::Kind::NoSummary) {
    d.AddMember("status", "completed_no_summary", a); d.AddMember("result", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
    return shared::ToolResult::ok(d);
  }
  d.AddMember("status", "completed", a); d.AddMember("result", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
  return shared::ToolResult::ok(d);
}

shared::ToolResult executeStop(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string agent_id;
  if (input.HasMember("agent_id") && input["agent_id"].IsString()) agent_id = input["agent_id"].GetString();
  else return shared::ToolResult::fail("Missing required field: agent_id");

  (void)ctx;
  Engine::instance().terminateAgent(agent_id);
  rapidjson::Document d; d.SetObject(); auto& a = d.GetAllocator();
  d.AddMember("agent_id", rapidjson::Value(agent_id.c_str(), a).Move(), a);
  d.AddMember("status", "terminated", a);
  return shared::ToolResult::ok(d);
}

} // namespace

shared::ToolMetadata DelegateTool::getMetadata() const {
  return {"Delegate",
          R"(Subagent operations for spawning, waiting on, and stopping child agents.

USAGE GUIDANCE:
- Use Spawn to start a bounded subtask in another agent/persona.
- Use Wait to collect the result of a previously spawned child.
- Use Stop to terminate a child that is stale, blocked, or no longer needed.
- Delegate only when parallelization or specialization materially helps; do not use it for tiny direct edits.
- A good spawn request should include concrete bearing, scope bounds, anchors, success criteria, and expected return shape in the task text.

ACTIONS:
- Spawn: launch a subagent.
- Wait: wait for a child result.
- Stop: terminate a child agent.
)",
          shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> DelegateTool::getSchema() const {
  return shared::zObject({
      {"action", shared::zEnum({"Spawn", "Wait", "Stop"})->describe(
          "Delegation action.\n\n"
          "- Spawn: create a child agent for a bounded task\n"
          "- Wait: wait for a child to finish and return its result\n"
          "- Stop: terminate a child agent")},
      {"agent_id", shared::zString()->setOptional()->describe(
          "Child agent id for Wait or Stop. Usually returned by Spawn.")},
      {"persona", shared::zString()->setOptional()->describe(
          "Persona/purpose to use for Spawn (for example: lead, coder, explorer, reviewer, shrike, titler).")},
      {"task", shared::zString()->setOptional()->describe(
          "Task text for Spawn. This should carry the real handoff: bearing, charge, bounds, anchors, unknowns, success test, return shape, recovery guidance.")},
      {"title", shared::zString()->setOptional()->describe(
          "Short human-readable title for the delegated cut.")},
      {"name", shared::zString()->setOptional()->describe(
          "Friendly agent label or purpose name for Spawn.")},
      {"category", shared::zString()->setOptional()->describe(
          "Optional route category / model routing hint for Spawn.")},
      {"async", shared::zBoolean()->setOptional()->describe(
          "When true, return immediately after Spawn instead of waiting inline.")},
      {"dream", shared::zBoolean()->setOptional()->describe(
          "When true, request a memory/dream-oriented run rather than a normal execution lane.")},
  });
}

shared::ToolResult DelegateTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  if (!input.IsObject() || !input.HasMember("action") || !input["action"].IsString()) {
    return shared::ToolResult::fail("Delegate.action must be a string (Spawn, Wait, or Stop)");
  }
  const std::string action = input["action"].GetString();
  if (action == "Spawn") return executeSpawn(input, ctx);
  if (action == "Wait") return executeWait(input, ctx);
  if (action == "Stop") return executeStop(input, ctx);
  return shared::ToolResult::fail("Delegate.action must be Spawn, Wait, or Stop");
}

} // namespace firmius::core
