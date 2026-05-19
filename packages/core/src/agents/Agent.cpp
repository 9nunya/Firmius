#include "agents/Agent.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "agents/modes/Mode.hpp"
#include "utils/Logger.hpp"

#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "EnvLoader.hpp"
#include "HeuristicTokenizer.hpp"
#include "Events.hpp"
#include "Message.hpp"
#include "Panic.hpp"
#include "Serialization.hpp"
#include "agents/ContextBudget.hpp"
#include "agents/PurposeLoader.hpp"
#include "agents/RuntimeOverlay.hpp"
#include "agents/working_memory/WorkingMemory.hpp"
#include "agents/working_memory/WorkingMemoryWorker.hpp"
#include "harness/Harness.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/LLMSearchProviderRegistry.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/FSUtil.hpp"
#include "utils/InterruptibleSleep.hpp"
#include "utils/StringUtil.hpp"
#include "utils/Hashline.hpp"
#include "tools/McpToolUtil.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <map>
#include <unordered_set>

namespace firmius::core {

using namespace firmius::shared;

namespace {
constexpr std::uint32_t kMissingToolCallIndex =
    std::numeric_limits<std::uint32_t>::max();

// ── Mode-scoped tool gating ───────────────────────────────────────────
// A mode's `tool_scope: { allow: [...], deny: [...] }` declares which
// security scopes the agent may exercise while that mode is active. The
// prompts already promise this enforcement to the model ("FilesystemWrite
// is denied in this mode"); without runtime gating the promise is a lie
// the model is free to ignore. This block makes the lie real.
//
// Semantics:
//   - empty allow + empty deny  → no constraint (legacy modes pass through)
//   - non-empty allow            → whitelist (tool's required scope must be in it)
//   - non-empty deny             → blacklist (overrides allow on conflict)
//   - ModeSwitch is always permitted (escape hatch — the agent must always
//     be able to leave a stuck mode without playing god-mode tricks)
struct ModeGateVerdict {
  bool permitted = true;
  std::string reason; // populated when permitted == false
};

// `toolScopeToString` lives in Serialization.cpp without a header
// declaration, so we keep a parallel mini-stringifier here for the
// nudge text. Kept in lock-step with `firmius::shared::ToolScope`.
const char *scopeLabel(shared::ToolScope s) {
  using shared::ToolScope;
  switch (s) {
  case ToolScope::FilesystemRead:
    return "FilesystemRead";
  case ToolScope::FilesystemWrite:
    return "FilesystemWrite";
  case ToolScope::Process:
    return "Process";
  case ToolScope::Semantic:
    return "Semantic";
  case ToolScope::Delegation:
    return "Delegation";
  case ToolScope::Web:
    return "Web";
  case ToolScope::Git:
    return "Git";
  case ToolScope::PlanRead:
    return "PlanRead";
  case ToolScope::PlanWrite:
    return "PlanWrite";
  case ToolScope::ChunkRead:
    return "ChunkRead";
  case ToolScope::ChunkWrite:
    return "ChunkWrite";
  case ToolScope::ChunkAssign:
    return "ChunkAssign";
  case ToolScope::ChunkReview:
    return "ChunkReview";
  }
  return "Unknown";
}

std::string joinScopes(const std::vector<shared::ToolScope> &scopes) {
  std::string out;
  for (std::size_t i = 0; i < scopes.size(); ++i) {
    if (i > 0)
      out += ", ";
    out += scopeLabel(scopes[i]);
  }
  return out.empty() ? std::string{"<none>"} : out;
}

ModeGateVerdict evaluateModeGate(const std::string &activeMode,
                                 const std::string &personaName,
                                 shared::ToolScope toolRequires,
                                 const std::string &toolName) {
  // No active mode → no gate. Sub-agents and "no overlay" stance both
  // hit this branch and behave exactly as they did pre-modes.
  if (activeMode.empty()) {
    return {true, ""};
  }
  // ModeSwitch is the escape hatch. If we gated it the agent could
  // get stuck in a denied stance with no way out.
  if (toolName == "ModeSwitch" || toolName == "mode_switch") {
    return {true, ""};
  }
  const auto *mode = modes::ModeRegistry::instance().resolveForPersona(
      activeMode, personaName);
  if (mode == nullptr) {
    // Unknown mode (e.g. someone renamed a sub-mode without restarting)
    // — fail open rather than brick the agent. The status pill will
    // still show the dangling name so the operator notices.
    return {true, ""};
  }

  // Hard-deny first: deny entries override an allow whitelist.
  for (auto s : mode->denyScopes) {
    if (s == toolRequires) {
      ModeGateVerdict v;
      v.permitted = false;
      v.reason = "Mode '" + mode->qualifiedName() + "' denies scope '" +
                 scopeLabel(toolRequires) + "'. Tool '" +
                 toolName + "' requires it. Call mode_switch to a stance " +
                 "that permits this scope, or hand off to a persona that " +
                 "can.";
      return v;
    }
  }
  // Allow-list (only enforced when non-empty). Empty allow == "no
  // explicit whitelist" == permit by default. This matches the YAML
  // intent: a mode that only declares deny is a soft gate.
  if (!mode->allowScopes.empty()) {
    bool found = false;
    for (auto s : mode->allowScopes) {
      if (s == toolRequires) {
        found = true;
        break;
      }
    }
    if (!found) {
      ModeGateVerdict v;
      v.permitted = false;
      v.reason = "Mode '" + mode->qualifiedName() + "' does not list scope '" +
                 scopeLabel(toolRequires) +
                 "' in its allow set. Tool '" + toolName +
                 "' requires it. Allowed scopes here: " +
                 joinScopes(mode->allowScopes) +
                 ". Call mode_switch to change stance.";
      return v;
    }
  }
  return {true, ""};
}

// Maximum accumulated response/thinking buffer size per turn (500KB)
static constexpr std::size_t kMaxAccumulatedResponseBytes = 500 * 1024;
static constexpr std::size_t kMaxAccumulatedThinkingBytes = 500 * 1024;
static constexpr std::size_t kMaxPersistedResponseBytes = 32 * 1024;
static constexpr std::size_t kMaxPersistedThinkingBytes = 16 * 1024;

std::string clampPersistedAssistantBody(const std::string &text,
                                        std::size_t maxBytes,
                                        const char *label) {
  if (text.size() <= maxBytes) {
    return text;
  }

  std::ostringstream out;
  out << text.substr(0, maxBytes);
  out << "\n\n[Assistant " << label << " truncated for persistence: "
      << (text.size() - maxBytes)
      << " additional bytes omitted.]";
  return out.str();
}

bool hasToolCallIndex(const ToolCallChunk &chunk) {
  return chunk.index != kMissingToolCallIndex;
}

bool isValidJsonObjectPayload(const std::string &payload) {
  const std::string trimmed = shared::StringUtil::trim(payload);
  if (trimmed.empty()) {
    return false;
  }

  rapidjson::Document parsed;
  parsed.Parse(trimmed.c_str());
  return !parsed.HasParseError() && parsed.IsObject();
}

std::vector<std::string> extractFileEditPaths(const std::string &toolArgs) {
  std::vector<std::string> paths;
  rapidjson::Document input;
  input.Parse(toolArgs.c_str());
  if (input.HasParseError() || !input.IsObject()) {
    return paths;
  }

  if (input.HasMember("files") && input["files"].IsArray()) {
    for (const auto &entry : input["files"].GetArray()) {
      if (entry.IsObject() && entry.HasMember("path") &&
          entry["path"].IsString()) {
        paths.emplace_back(entry["path"].GetString());
      }
    }
    return paths;
  }

  if (input.HasMember("path") && input["path"].IsString()) {
    paths.emplace_back(input["path"].GetString());
  }
  return paths;
}

struct EditedFileEventPayload {
  std::string path;
  std::string diffPreview;
  int addedLines = 0;
  int removedLines = 0;
};

std::vector<std::string> parseStringArrayMember(const rapidjson::Value &value,
                                                const char *key) {
  std::vector<std::string> lines;
  if (!value.IsObject() || !value.HasMember(key) || !value[key].IsArray()) {
    return lines;
  }
  for (const auto &entry : value[key].GetArray()) {
    if (entry.IsString()) {
      lines.emplace_back(entry.GetString());
    }
  }
  return lines;
}

std::string buildDiffPreviewFromOperations(const rapidjson::Value &value) {
  if (!value.IsObject() || !value.HasMember("operations") ||
      !value["operations"].IsArray()) {
    return "";
  }
  std::ostringstream out;
  bool wrote_any = false;
  for (const auto &op : value["operations"].GetArray()) {
    if (!op.IsObject()) {
      continue;
    }
    const std::string description =
        op.HasMember("description") && op["description"].IsString()
            ? op["description"].GetString()
            : (op.HasMember("op") && op["op"].IsString() ? op["op"].GetString()
                                                           : "edit");
    const auto oldLines = parseStringArrayMember(op, "old_lines");
    const auto newLines = parseStringArrayMember(op, "new_lines");
    if (oldLines.empty() && newLines.empty()) {
      continue;
    }
    if (wrote_any) {
      out << "\n";
    }
    out << "@@ " << description << " @@\n";
    for (const auto &line : oldLines) {
      out << "-" << line << "\n";
    }
    for (const auto &line : newLines) {
      out << "+" << line << "\n";
    }
    wrote_any = true;
  }
  return out.str();
}

std::unordered_map<std::string, std::string>
extractOverwriteContentByPath(const std::string &toolArgs) {
  std::unordered_map<std::string, std::string> contents;
  rapidjson::Document input;
  input.Parse(toolArgs.c_str());
  if (input.HasParseError() || !input.IsObject()) {
    return contents;
  }

  auto appendFromObject = [&](const rapidjson::Value &value) {
    if (!value.IsObject() || !value.HasMember("path") || !value["path"].IsString() ||
        !value.HasMember("content") || !value["content"].IsString()) {
      return;
    }
    contents[value["path"].GetString()] = value["content"].GetString();
  };

  if (input.HasMember("files") && input["files"].IsArray()) {
    for (const auto &entry : input["files"].GetArray()) {
      appendFromObject(entry);
    }
    return contents;
  }

  appendFromObject(input);
  return contents;
}

struct EditLedgerToolExecutionResultView {
  std::string toolCallId;
  std::string toolName;
  std::string resultStr;
};

std::string computeContentFingerprint(const std::string &content) {
  return shared::utils::Hashline::computeHash(content) + "-" +
         std::to_string(content.size());
}

std::string detectNewlineMode(const std::string &content) {
  if (content.find("\r\n") != std::string::npos) {
    return "crlf";
  }
  if (content.find('\n') != std::string::npos) {
    return "lf";
  }
  return "none";
}

std::string buildEditBatchSummaryText(const rapidjson::Value &doc) {
  const bool hasFiles = doc.IsObject() && doc.HasMember("files") && doc["files"].IsArray();
  if (hasFiles) {
    return "Edited " + std::to_string(doc["files"].Size()) + " files";
  }
  if (doc.IsObject() && doc.HasMember("path") && doc["path"].IsString()) {
    return std::string("Edited ") + doc["path"].GetString();
  }
  return "Edited file";
}

std::vector<shared::EditFileMutation>
extractEditLedgerMutations(const std::string &threadId,
                           const rapidjson::Value &resultDoc,
                           const std::string &editBatchId) {
  std::vector<shared::EditFileMutation> mutations;
  auto appendFile = [&](const rapidjson::Value &fileDoc, int ordinal) {
    if (!fileDoc.IsObject() || !fileDoc.HasMember("path") || !fileDoc["path"].IsString()) {
      return;
    }
    shared::EditFileMutation mutation;
    mutation.fileMutationId = shared::StringUtil::generateUuid();
    mutation.editBatchId = editBatchId;
    mutation.threadId = threadId;
    mutation.filePath = fileDoc["path"].GetString();
    mutation.ordinalInBatch = ordinal;
    mutation.mode = fileDoc.HasMember("resolved_mode") && fileDoc["resolved_mode"].IsString()
                        ? fileDoc["resolved_mode"].GetString()
                        : "";
    mutation.diffPreview = fileDoc.HasMember("diff_preview") && fileDoc["diff_preview"].IsString()
                               ? fileDoc["diff_preview"].GetString()
                               : "";
    if (fileDoc.HasMember("operations") && fileDoc["operations"].IsArray()) {
      for (const auto &op : fileDoc["operations"].GetArray()) {
        if (!op.IsObject()) {
          continue;
        }
        shared::EditMutationOperation operation;
        operation.description = op.HasMember("description") && op["description"].IsString()
                                    ? op["description"].GetString()
                                    : "";
        operation.startLine = op.HasMember("start_line") && op["start_line"].IsInt()
                                  ? op["start_line"].GetInt()
                                  : 1;
        operation.endLine = op.HasMember("end_line") && op["end_line"].IsInt()
                                ? op["end_line"].GetInt()
                                : 0;
        operation.oldLines = parseStringArrayMember(op, "old_lines");
        operation.newLines = parseStringArrayMember(op, "new_lines");
        mutation.operations.push_back(std::move(operation));
      }
    }

    std::string beforeContent;
    std::string afterContent;
    bool first = true;
    for (const auto &op : mutation.operations) {
      if (!first) {
        beforeContent += "\n";
        afterContent += "\n";
      }
      first = false;
      for (size_t i = 0; i < op.oldLines.size(); ++i) {
        if (i > 0) beforeContent += "\n";
        beforeContent += op.oldLines[i];
      }
      for (size_t i = 0; i < op.newLines.size(); ++i) {
        if (i > 0) afterContent += "\n";
        afterContent += op.newLines[i];
      }
    }
    mutation.hadFileBefore = !(mutation.mode == "write" && !mutation.operations.empty() &&
                               mutation.operations.front().oldLines.empty() &&
                               mutation.operations.front().description == "create file");
    // The honest signal: if the tool emitted "had_file_before" we trust
    // it directly. The prior heuristic (string-match on description)
    // silently broke whenever a different code path emitted a different
    // verb, leaving create-style undos as no-op writes. The fallback
    // above is kept for backwards compat with tool results that haven't
    // been migrated yet.
    if (fileDoc.HasMember("had_file_before") &&
        fileDoc["had_file_before"].IsBool()) {
      mutation.hadFileBefore = fileDoc["had_file_before"].GetBool();
    }
    mutation.hasFileAfter = true;
    mutation.preSize = beforeContent.size();
    mutation.postSize = afterContent.size();
    mutation.preHash = computeContentFingerprint(beforeContent);
    mutation.postHash = computeContentFingerprint(afterContent);
    mutation.newlineModeBefore = detectNewlineMode(beforeContent);
    mutation.newlineModeAfter = detectNewlineMode(afterContent);
    mutations.push_back(std::move(mutation));
  };

  if (resultDoc.IsObject() && resultDoc.HasMember("files") && resultDoc["files"].IsArray()) {
    int ordinal = 0;
    for (const auto &file : resultDoc["files"].GetArray()) {
      appendFile(file, ordinal++);
    }
  } else {
    appendFile(resultDoc, 0);
  }
  return mutations;
}

void persistEditLedgerBatch(const shared::AgentContext &context,
                            const EditLedgerToolExecutionResultView &result) {
  if (!context.history || context.history->threadId.empty()) {
    return;
  }
  rapidjson::Document resultDoc;
  resultDoc.Parse(result.resultStr.c_str());
  if (resultDoc.HasParseError() || !resultDoc.IsObject()) {
    return;
  }
  const std::string editBatchId = shared::StringUtil::generateUuid();
  shared::EditBatchSummary summary;
  summary.editBatchId = editBatchId;
  summary.threadId = context.history->threadId;
  summary.agentId = context.identity.id;
  summary.parentAgentId = context.identity.parentId;
  summary.friendlyName = context.identity.friendlyName;
  summary.turnId = context.history->turns.empty() ? "" : context.history->turns.back().turnId;
  summary.toolCallId = result.toolCallId;
  summary.toolName = result.toolName;
  summary.requestMode = resultDoc.HasMember("resolved_mode") && resultDoc["resolved_mode"].IsString()
                            ? resultDoc["resolved_mode"].GetString()
                            : "";
  summary.createdAt = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count());
  summary.status = shared::EditBatchStatus::Applied;
  summary.summaryText = buildEditBatchSummaryText(resultDoc);
  auto mutations = extractEditLedgerMutations(context.history->threadId, resultDoc, editBatchId);
  for (const auto &mutation : mutations) {
    summary.files.push_back(mutation.filePath);
    for (const auto &op : mutation.operations) {
      summary.addedLines += static_cast<int>(op.newLines.size());
      summary.removedLines += static_cast<int>(op.oldLines.size());
    }
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  tm.writeEditBatch(context.history->threadId, summary, mutations);
}

// Token-waste pass 1: split a combined unified diff (the kind FileEditTool
// emits for multi-file patches, with `--- a/path` / `+++ b/path` headers
// per file) into one diff slice per path. Returns a map keyed by path.
//
// The slicing tolerates both real unified-diff hunk bodies and the
// `@@ description @@` style buildDiffPreview produces; whatever lies
// between two consecutive `--- a/...` headers is attributed to the
// preceding file.
std::map<std::string, std::string> splitCombinedDiffByPath(
    const std::string &combinedDiff) {
  std::map<std::string, std::string> out;
  if (combinedDiff.empty()) return out;

  std::istringstream stream(combinedDiff);
  std::string line;
  std::string currentPath;
  std::ostringstream currentSlice;

  auto flush = [&]() {
    if (!currentPath.empty()) {
      out[currentPath] += currentSlice.str();
    }
    currentSlice.str("");
    currentSlice.clear();
  };

  while (std::getline(stream, line)) {
    if (line.rfind("--- a/", 0) == 0) {
      flush();
      currentPath = line.substr(6);
    } else if (line.rfind("+++ b/", 0) == 0) {
      // header line — skip, it carries no diff body
    } else {
      currentSlice << line << "\n";
    }
  }
  flush();
  return out;
}

std::vector<EditedFileEventPayload>
extractFileEditEventPayloads(const std::string &toolArgs,
                             const std::string &resultStr) {
  std::vector<EditedFileEventPayload> payloads;
  const auto overwriteContents = extractOverwriteContentByPath(toolArgs);

  rapidjson::Document resultDoc;
  resultDoc.Parse(resultStr.c_str());
  if (!resultDoc.HasParseError() && resultDoc.IsObject()) {
    auto appendFromObject = [&](const rapidjson::Value &value) {
      if (!value.IsObject() || !value.HasMember("path") ||
          !value["path"].IsString()) {
        return;
      }
      EditedFileEventPayload payload;
      payload.path = value["path"].GetString();
      // Token-waste pass 1: prefer the new `diff` field; fall back to the
      // legacy `diff_preview` for older test inputs.
      if (value.HasMember("diff") && value["diff"].IsString()) {
        payload.diffPreview = value["diff"].GetString();
      } else if (value.HasMember("diff_preview") &&
                 value["diff_preview"].IsString()) {
        payload.diffPreview = value["diff_preview"].GetString();
      }
      if (payload.diffPreview.empty()) {
        payload.diffPreview = buildDiffPreviewFromOperations(value);
      }
      if (value.HasMember("added_lines") && value["added_lines"].IsInt()) {
        payload.addedLines = value["added_lines"].GetInt();
      }
      if (value.HasMember("removed_lines") && value["removed_lines"].IsInt()) {
        payload.removedLines = value["removed_lines"].GetInt();
      }
      if (payload.diffPreview.empty()) {
        auto overwriteIt = overwriteContents.find(payload.path);
        if (overwriteIt != overwriteContents.end()) {
          std::istringstream stream(overwriteIt->second);
          std::string line;
          std::ostringstream preview;
          preview << "@@ overwrite file @@\n";
          int added = 0;
          while (std::getline(stream, line)) {
            preview << "+" << line << "\n";
            ++added;
          }
          payload.diffPreview = preview.str();
          if (payload.addedLines == 0) {
            payload.addedLines = added;
          }
        }
      }
      payloads.push_back(std::move(payload));
    };

    if (resultDoc.HasMember("files") && resultDoc["files"].IsArray()) {
      const auto &filesArr = resultDoc["files"];
      // Token-waste pass 1: multi-file aggregate emits `files: [paths]` as
      // a string array plus a single combined `diff`. Detect that shape and
      // synthesize per-path payloads from the split combined diff.
      bool allStrings = filesArr.Size() > 0;
      for (const auto &entry : filesArr.GetArray()) {
        if (!entry.IsString()) {
          allStrings = false;
          break;
        }
      }

      if (allStrings) {
        std::string combinedDiff;
        if (resultDoc.HasMember("diff") && resultDoc["diff"].IsString()) {
          combinedDiff = resultDoc["diff"].GetString();
        }
        const auto perPath = splitCombinedDiffByPath(combinedDiff);
        for (const auto &entry : filesArr.GetArray()) {
          EditedFileEventPayload payload;
          payload.path = entry.GetString();
          auto it = perPath.find(payload.path);
          if (it != perPath.end()) {
            payload.diffPreview = it->second;
          }
          payloads.push_back(std::move(payload));
        }
      } else {
        for (const auto &entry : filesArr.GetArray()) {
          appendFromObject(entry);
        }
      }
    } else {
      appendFromObject(resultDoc);
    }
  }

  if (!payloads.empty()) {
    return payloads;
  }

  for (const auto &path : extractFileEditPaths(toolArgs)) {
    payloads.push_back(EditedFileEventPayload{path, "", 0, 0});
  }
  return payloads;
}

void mergeToolCallName(std::string &existing, const std::string &incoming) {
  if (incoming.empty()) {
    return;
  }
  if (existing.empty()) {
    existing = incoming;
    return;
  }
  if (incoming == existing) {
    return;
  }
  if (incoming.rfind(existing, 0) == 0) {
    existing = incoming;
    return;
  }
  if (existing.rfind(incoming, 0) == 0) {
    return;
  }
  existing += incoming;
}

void mergeToolCallArgs(std::string &existing, const std::string &incoming) {
  if (incoming.empty()) {
    return;
  }
  if (existing.empty()) {
    existing = incoming;
    return;
  }
  if (incoming == existing) {
    return;
  }

  if (isValidJsonObjectPayload(incoming)) {
    existing = incoming;
    return;
  }

  existing += incoming;
}

std::vector<ToolCallChunk>::iterator
findMatchingToolCallChunk(std::vector<ToolCallChunk> &accumulated,
                          const ToolCallChunk &incoming) {
  if (!incoming.id.empty()) {
    auto byId = std::find_if(accumulated.begin(), accumulated.end(),
                             [&](const ToolCallChunk &existing) {
                               return existing.id == incoming.id;
                             });
    if (byId != accumulated.end()) {
      return byId;
    }
  }

  if (hasToolCallIndex(incoming)) {
    auto byIndex = std::find_if(
        accumulated.begin(), accumulated.end(),
        [&](const ToolCallChunk &existing) {
          if (!hasToolCallIndex(existing) || existing.index != incoming.index) {
            return false;
          }
          return incoming.id.empty() || existing.id.empty() ||
                 existing.id == incoming.id;
        });
    if (byIndex != accumulated.end()) {
      return byIndex;
    }
  }

  return accumulated.end();
}

void mergeToolCallChunk(std::vector<ToolCallChunk> &accumulated,
                        const ToolCallChunk &incoming,
                        std::uint32_t syntheticIdSerial,
                        std::uint32_t turnCount) {
  auto it = findMatchingToolCallChunk(accumulated, incoming);
  if (it == accumulated.end()) {
    auto appended = incoming;
    if (appended.id.empty()) {
      appended.id = "call_" + std::to_string(turnCount) + "_" +
                    std::to_string(syntheticIdSerial);
    }
    accumulated.push_back(std::move(appended));
    return;
  }

  if (it->id.empty() && !incoming.id.empty()) {
    it->id = incoming.id;
  }
  if (!hasToolCallIndex(*it) && hasToolCallIndex(incoming)) {
    it->index = incoming.index;
  }
  mergeToolCallName(it->nameDelta, incoming.nameDelta);
  mergeToolCallArgs(it->argsDelta, incoming.argsDelta);
}

std::vector<ToolCall>::iterator findMatchingToolCall(
    std::vector<ToolCall> &accumulated, const ToolCall &incoming) {
  if (!incoming.id.empty()) {
    auto byId = std::find_if(accumulated.begin(), accumulated.end(),
                             [&](const ToolCall &existing) {
                               return existing.id == incoming.id;
                             });
    if (byId != accumulated.end()) {
      return byId;
    }
  }

  if (incoming.index != kMissingToolCallIndex) {
    auto byIndex = std::find_if(
        accumulated.begin(), accumulated.end(),
        [&](const ToolCall &existing) {
          if (existing.index != incoming.index) {
            return false;
          }
          return incoming.id.empty() || existing.id.empty() ||
                 existing.id == incoming.id;
        });
    if (byIndex != accumulated.end()) {
      return byIndex;
    }
  }

  return accumulated.end();
}

void mergeFinalToolCall(std::vector<ToolCall> &accumulated,
                        const ToolCall &incoming) {
  auto it = findMatchingToolCall(accumulated, incoming);
  if (it == accumulated.end()) {
    accumulated.push_back(incoming);
    return;
  }

  if (it->id.empty() && !incoming.id.empty()) {
    it->id = incoming.id;
  }
  if (it->index == kMissingToolCallIndex &&
      incoming.index != kMissingToolCallIndex) {
    it->index = incoming.index;
  }
  if (!incoming.name.empty()) {
    it->name = incoming.name;
  }
  if (!incoming.args.empty()) {
    it->args = incoming.args;
  }
}

std::vector<ToolCall>
materializeFinalToolCalls(const std::vector<ToolCallChunk> &chunks) {
  std::vector<ToolCall> calls;
  calls.reserve(chunks.size());
  for (const auto &chunk : chunks) {
    calls.push_back(ToolCall{chunk.id, chunk.index, chunk.nameDelta,
                             chunk.argsDelta});
  }
  return calls;
}

bool isChunkFinalized(const ToolCallChunk &chunk,
                      const std::vector<ToolCall> &calls) {
  return std::any_of(calls.begin(), calls.end(), [&](const ToolCall &call) {
    if (!chunk.id.empty() && !call.id.empty() && chunk.id == call.id) {
      return true;
    }
    return hasToolCallIndex(chunk) && call.index != kMissingToolCallIndex &&
           chunk.index == call.index;
  });
}

bool shouldRetryProviderFailureAtAgentLayer(int httpStatus) {
  if (httpStatus == -1 || httpStatus == 0 || httpStatus >= 500) {
    return true;
  }
  return httpStatus == 408 || httpStatus == 409 || httpStatus == 425;
}

std::string appendProviderModelContext(const AgentConfig &config,
                                       const std::string &message) {
  std::string detailed = message;
  if (!config.providerId.empty() &&
      detailed.find("\nProvider: ") == std::string::npos) {
    detailed += "\nProvider: " + config.providerId;
  }

  if (!config.modelId.empty() &&
      detailed.find("\nModel: ") == std::string::npos) {
    detailed += "\nModel: " + config.modelId;
  }

  if (!config.modelVariant.empty() &&
      detailed.find("\nVariant: ") == std::string::npos) {
    detailed += "\nVariant: " + config.modelVariant;
  }
  return detailed;
}
std::string encodeDynamicMcpNamePart(const std::string &value) {
  std::ostringstream out;
  out << std::hex << std::uppercase;
  for (unsigned char ch : value) {
    if (std::isalnum(ch)) {
      out << static_cast<char>(ch);
      continue;
    }
    out << "_x";
    out.width(2);
    out.fill('0');
    out << static_cast<int>(ch);
    out << "_";
  }
  return out.str();
}

std::string decodeDynamicMcpNamePart(const std::string &value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size();) {
    if (i + 4 < value.size() && value[i] == '_' && value[i + 1] == 'x' &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2])) &&
        std::isxdigit(static_cast<unsigned char>(value[i + 3])) &&
        value[i + 4] == '_') {
      std::string hex = value.substr(i + 2, 2);
      const int parsed = std::stoi(hex, nullptr, 16);
      decoded.push_back(static_cast<char>(parsed));
      i += 5;
      continue;
    }
    decoded.push_back(value[i]);
    ++i;
  }
  return decoded;
}

std::string buildDynamicMcpToolName(const std::string &serverName,
                                    const std::string &toolName) {
  return "mcp__" + encodeDynamicMcpNamePart(serverName) + "__" +
         encodeDynamicMcpNamePart(toolName);
}

bool parseDynamicMcpToolName(const std::string &name, std::string &serverName,
                             std::string &toolName) {
  constexpr std::string_view kPrefix = "mcp__";
  if (name.rfind(kPrefix.data(), 0) != 0) {
    return false;
  }

  const std::string encoded = name.substr(kPrefix.size());
  const std::size_t delim = encoded.find("__");
  if (delim == std::string::npos || delim == 0 || delim + 2 >= encoded.size()) {
    return false;
  }

  serverName = decodeDynamicMcpNamePart(encoded.substr(0, delim));
  toolName = decodeDynamicMcpNamePart(encoded.substr(delim + 2));
  return !serverName.empty() && !toolName.empty();
}

void appendDynamicMcpToolDefinitions(const AgentContext &context,
                                     std::vector<provider::ToolDefinition> &defs) {
  std::unordered_set<std::string> seen;
  seen.reserve(defs.size() + context.state.loadedMcpTools.size());
  for (const auto &def : defs) {
    seen.insert(def.name);
  }

  for (const auto &[serverName, toolDefs] : context.state.loadedMcpToolDefinitions) {
    for (const auto &toolDef : toolDefs) {
      if (toolDef.name.empty()) {
        continue;
      }
      const std::string dynamicName = buildDynamicMcpToolName(serverName, toolDef.name);
      if (seen.count(dynamicName) > 0) {
        continue;
      }
      provider::ToolDefinition dynamicDef;
      dynamicDef.name = dynamicName;
      dynamicDef.description = toolDef.description.empty()
          ? ("Invoke loaded MCP tool '" + toolDef.name + "' on server '" + serverName + "'.")
          : toolDef.description;
      dynamicDef.inputSchema = toolDef.inputSchema;
      defs.push_back(std::move(dynamicDef));
      seen.insert(dynamicName);
    }
  }
}

std::vector<provider::ToolDefinition> getProviderToolDefinitions(
    const AgentContext &context, const ToolRegistry &toolRegistry) {
  auto defs = toolRegistry.getAvailableToolDefinitions(context.permissions);
  appendDynamicMcpToolDefinitions(context, defs);
  return defs;
}

std::optional<shared::ToolResult> executeDynamicMcpToolCall(
    const AgentContext &context, const std::string &toolName,
    const rapidjson::Document &toolInput, ToolContext &toolCtx) {
  std::string serverName;
  std::string remoteToolName;
  if (!parseDynamicMcpToolName(toolName, serverName, remoteToolName)) {
    return std::nullopt;
  }

  if (std::find(context.state.loadedMcpServers.begin(),
                context.state.loadedMcpServers.end(),
                serverName) == context.state.loadedMcpServers.end()) {
    return shared::ToolResult::fail("MCP server is not loaded: " + serverName);
  }

  const auto loadedToolsIt = context.state.loadedMcpTools.find(serverName);
  if (loadedToolsIt == context.state.loadedMcpTools.end() ||
      std::find(loadedToolsIt->second.begin(), loadedToolsIt->second.end(),
                remoteToolName) == loadedToolsIt->second.end()) {
    return shared::ToolResult::fail("Loaded MCP tool not available on server '" +
                                    serverName + "': " + remoteToolName);
  }

  rapidjson::Document mcpCallInput;
  mcpCallInput.SetObject();
  auto &alloc = mcpCallInput.GetAllocator();
  mcpCallInput.AddMember(
      "server_name",
      rapidjson::Value(serverName.c_str(), alloc).Move(), alloc);
  mcpCallInput.AddMember(
      "tool_name",
      rapidjson::Value(remoteToolName.c_str(), alloc).Move(), alloc);
  rapidjson::Value argsValue;
  argsValue.CopyFrom(toolInput, alloc);
  mcpCallInput.AddMember("arguments", argsValue.Move(), alloc);

  auto &agent = dynamic_cast<Agent &>(toolCtx.agent);
  auto client = agent.getMcpClient(serverName, toolCtx);
  if (!client) {
    return shared::ToolResult::fail("Failed to get MCP client for: " + serverName);
  }

  if (!client->isInitialized()) {
    client->initialize(mcp_tools::kDefaultTimeoutMs, &toolCtx);
  }

  try {
    const rapidjson::Document callResponse =
        client->callTool(remoteToolName, toolInput, mcp_tools::kDefaultTimeoutMs, &toolCtx);

    rapidjson::Document out;
    out.SetObject();
    auto &a = out.GetAllocator();
    out.AddMember("server_name",
                  rapidjson::Value(serverName.c_str(), a).Move(), a);
    out.AddMember("tool_name", rapidjson::Value(remoteToolName.c_str(), a).Move(),
                  a);

    rapidjson::Value remoteResult;
    remoteResult.CopyFrom(callResponse["result"], a);
    out.AddMember("remote_result", remoteResult, a);

    return shared::ToolResult::ok(out);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

void saveCompactionSnapshot(const std::string &threadId,
                            const std::string &agentId,
                            const std::string &compactionId,
                            std::uint32_t previousContextSize,
                            const std::vector<AgentTurn> &turns) {
  if (threadId.empty() || agentId.empty() || compactionId.empty()) {
    return;
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  CompactionSnapshot snapshot;
  snapshot.compactionId = compactionId;
  snapshot.threadId = threadId;
  snapshot.agentId = agentId;
  snapshot.previousContextSize = previousContextSize;
  snapshot.createdAt = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  snapshot.turns = turns;
  tm.appendCompactionSnapshot(threadId, agentId, snapshot);
}

// Retained for /compact undo support — used by CompactionSnapshot restoration.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
std::string buildPlanAndTodoSnapshot(const AgentContext &context) {
  if (!context.history || context.history->threadId.empty() ||
      context.identity.id.empty()) {
    return "";
  }

  std::ostringstream state;
  auto todoStatusLabel = [](TodoStatus status) -> const char * {
    switch (status) {
    case TodoStatus::Pending:
      return "Pending";
    case TodoStatus::InProgress:
      return "InProgress";
    case TodoStatus::Done:
      return "Done";
    }
    return "Unknown";
  };
  auto trimForPrompt = [](const std::string &value, std::size_t maxLen) {
    const std::string trimmed = shared::StringUtil::trim(value);
    if (trimmed.size() <= maxLen) {
      return trimmed;
    }
    return trimmed.substr(0, maxLen) + "...";
  };

  try {
    ThreadManager tm(ThreadManager::defaultBasePath());
    const AgentTodoList todo =
        tm.getAgentTodo(context.history->threadId, context.identity.id);
    if (!todo.items.empty()) {
      int incompleteTodo = 0;
      for (const auto &item : todo.items) {
        if (item.status != TodoStatus::Done) {
          incompleteTodo++;
        }
      }
      state << "**Todo Items:** " << todo.items.size() << "\n";
      state << "**Todo Incomplete:** " << incompleteTodo << "\n";
      constexpr std::size_t kMaxTodoItems = 12;
      state << "**Todo Ledger:**\n";
      for (std::size_t i = 0; i < todo.items.size() && i < kMaxTodoItems;
           ++i) {
        const auto &item = todo.items[i];
        state << "- (#" << item.id << ") [" << todoStatusLabel(item.status)
              << "] " << trimForPrompt(item.text, 220);
        if (!item.chunkId.empty()) {
          state << " chunk=" << item.chunkId;
        }
        if (!item.planId.empty()) {
          state << " plan=" << item.planId;
        }
        state << "\n";
      }
      if (todo.items.size() > kMaxTodoItems) {
        state << "- ... " << (todo.items.size() - kMaxTodoItems)
              << " additional todo item(s)\n";
      }
    }
  } catch (...) {
    Logger::instance().logDebug("Agent: failed to build todo state for context");
  }

  return state.str();
}

struct TodoStateSnapshot {
  bool hasAny = false;
  bool hasIncomplete = false;
  std::vector<TodoItem> incompleteItems;
};

std::string todoContinuationFingerprint(const TodoStateSnapshot &todoState) {
  std::ostringstream fingerprint;
  for (const auto &item : todoState.incompleteItems) {
    fingerprint << item.id << '\n'
                << static_cast<int>(item.status) << '\n'
                << item.text << '\n'
                << item.chunkId << '\n'
                << item.planId << "\n---\n";
  }
  return fingerprint.str();
}

std::string todoStatusLabel(TodoStatus status) {
  switch (status) {
  case TodoStatus::Pending:
    return "Pending";
  case TodoStatus::InProgress:
    return "InProgress";
  case TodoStatus::Done:
    return "Done";
  }
  return "Unknown";
}

// Wraps any runtime nudge in a structurally-recognisable signal block. The
// tag is taught in prompts/base.md so that the model treats these as
// machine-emitted control instructions rather than user prose. Add new
// `kind` values here as new nudge categories appear.
std::string wrapSystemSignal(
    const std::string &kind,
    const std::string &body,
    std::initializer_list<std::pair<std::string, std::string>> attrs = {}) {
  std::ostringstream out;
  out << "<FIRMIUS_SYSTEM_SIGNAL kind=\"" << kind << "\"";
  for (const auto &[key, value] : attrs) {
    if (!key.empty()) {
      out << " " << key << "=\"" << value << "\"";
    }
  }
  out << ">\n" << body << "\n</FIRMIUS_SYSTEM_SIGNAL>";
  return out.str();
}

std::string buildIncompleteTodoNudge(const TodoStateSnapshot &todoState) {
  std::ostringstream prompt;
  prompt << "You stopped while todo items are still open. Continue working "
            "through the remaining items: ";
  for (std::size_t i = 0; i < todoState.incompleteItems.size(); ++i) {
    const auto &item = todoState.incompleteItems[i];
    if (i > 0) {
      prompt << ", ";
    }
    prompt << "#" << item.id << " [" << todoStatusLabel(item.status)
           << "] ";
    if (!shared::StringUtil::trim(item.text).empty()) {
      prompt << item.text;
    } else {
      prompt << "(no text)";
    }
  }
  return wrapSystemSignal("todo_continuation", prompt.str(),
                          {{"open_count",
                            std::to_string(todoState.incompleteItems.size())}});
}

std::string buildIncompleteTodoEscalationNudge(std::size_t openCount) {
  return wrapSystemSignal(
      "todo_enforcement",
      "This is the second nudge for the same open todos. Make a tool call "
      "this turn — either advance an item or mark it done/cancelled via the "
      "Todo tool. Do not narrate. Do not summarise.",
      {{"open_count", std::to_string(openCount)}, {"escalated", "true"}});
}

std::string buildEmptyProviderRetryNudge(int attempt) {
  std::ostringstream prompt;
  prompt << "The provider returned no visible reply, no thinking, and no tool "
            "calls. Continue the current task. Either provide the next useful "
            "response or call the next tool.";
  if (attempt > 1) {
    prompt << " This is empty response retry " << attempt << ".";
  }
  return wrapSystemSignal("empty_response_retry", prompt.str(),
                          {{"attempt", std::to_string(attempt)}});
}

std::string buildActiveWorkContinuationNudge() {
  return wrapSystemSignal(
      "active_work_continuation",
      "Runtime work is still active (tool lifecycle, blocking process, "
      "background process, or descendant subagent). Continue coordinating "
      "until it settles. If there is nothing new to do yet, give a concise "
      "progress update and keep monitoring.");
}

std::string buildBlockedWorkflowReentryNudge(int attempt) {
  std::string body =
      "A workflow blocked your previous stop attempt, and you still have not "
      "resumed the task. Follow the injected workflow instructions already in "
      "history. Make concrete progress now. If work remains, use the next "
      "tool instead of repeating the blocked completion claim.";
  if (attempt > 1) {
    body += " This is repeated workflow continuation attempt " +
            std::to_string(attempt) +
            ". Make a tool call or produce a materially different response.";
  }
  return wrapSystemSignal("workflow_continuation", body,
                          {{"blocked", "true"}, {"attempt", std::to_string(attempt)}});
}

TodoStateSnapshot readTodoState(const AgentContext &context) {
  TodoStateSnapshot snapshot;
  if (!context.history || context.history->threadId.empty()) {
    return snapshot;
  }
  if (context.identity.id.empty()) {
    return snapshot;
  }

  try {
    ThreadManager tm(ThreadManager::defaultBasePath());
    const AgentTodoList todo =
        tm.getAgentTodo(context.history->threadId, context.identity.id);
    snapshot.hasAny = !todo.items.empty();
    for (const auto &item : todo.items) {
      if (item.status != TodoStatus::Done) {
        snapshot.hasIncomplete = true;
        snapshot.incompleteItems.push_back(item);
      }
    }
    return snapshot;
  } catch (...) {
    Logger::instance().logDebug("Agent: failed to build todo snapshot");
    return snapshot;
  }
}

bool isExecutionalStatus(AgentStatus status) {
  return status == AgentStatus::ExecutingTool ||
         status == AgentStatus::Compacting;
}

bool isDescendantAgentRunning(const std::string &candidateAgentId,
                              const std::string &ancestorAgentId) {
  if (candidateAgentId.empty() || ancestorAgentId.empty() ||
      candidateAgentId == ancestorAgentId) {
    return false;
  }

  auto candidate = AgentRegistry::instance().getAgent(candidateAgentId);
  int depth = 0;
  while (candidate && depth < 100) {
    const auto &parentId = candidate->getContext().identity.parentId;
    if (parentId.empty()) {
      return false;
    }
    if (parentId == ancestorAgentId) {
      return candidate->isRunning() || candidate->isBooting();
    }
    candidate = AgentRegistry::instance().getAgent(parentId);
    depth++;
  }
  return false;
}

struct ToolCallValidationFailure {
  std::string toolCallId;
  std::string message;
};

std::vector<ToolCallValidationFailure>
validateStreamedToolCalls(const std::vector<ToolCallChunk> &chunks) {
  std::vector<ToolCallValidationFailure> failures;
  for (const auto &chunk : chunks) {
    if (shared::StringUtil::trim(chunk.nameDelta).empty()) {
      failures.push_back({chunk.id, "missing tool name"});
      continue;
    }

    rapidjson::Document args;
    args.Parse(chunk.argsDelta.c_str());
    if (args.HasParseError()) {
      failures.push_back(
          {chunk.id, "invalid or truncated JSON arguments for tool '" +
                         chunk.nameDelta + "'"});
      continue;
    }

    if (!args.IsObject()) {
      failures.push_back({chunk.id, "tool arguments for '" + chunk.nameDelta +
                                        "' must be a JSON object"});
    }
  }
  return failures;
}

std::string buildToolStreamRetryNudge(const std::string &details,
                                      int attempt,
                                      int maxAttempts) {
  std::string body =
      "The previous response ended during tool-call generation before every "
      "tool call was fully finalized. Retry the entire pending tool-call "
      "batch from scratch in your next response. Do not continue from the "
      "partial payload. Emit only complete tool calls with full JSON object "
      "arguments before any normal prose.";
  if (!details.empty()) {
    body += "\nObserved failure: " + details;
  }
  body += "\nRetry attempt " + std::to_string(attempt) + " of " +
          std::to_string(maxAttempts) + ".";
  return wrapSystemSignal("tool_stream_retry", body,
                          {{"attempt", std::to_string(attempt)},
                           {"max_attempts", std::to_string(maxAttempts)}});
}

std::string buildInsanityInterventionNudge(const std::string &reason) {
  std::string body =
      "The previous turn exhibited signs of degenerate output "
      "(repetition/gibberish). Recover and continue constructively with a "
      "different approach.";
  if (!reason.empty()) {
    body += "\nDetected reason: " + reason;
  }
  return wrapSystemSignal("insanity_intervention", body);
}

std::string buildToolRepetitionNudge(const std::string &toolName,
                                     int repeatCount) {
  std::string body =
      "You are calling the same tool with identical arguments repeatedly (" +
      std::to_string(repeatCount + 1) +
      " times). This indicates an insanity loop. Stop and try a different "
      "approach. Do not repeat this tool call.";
  return wrapSystemSignal("tool_repetition", body,
                          {{"tool", toolName},
                           {"repeats", std::to_string(repeatCount + 1)}});
}
#pragma GCC diagnostic pop

// Build a synchronous SummarizerFn for the deflation path. Returns an
// empty function (no-op caller falls back to deterministic stubs) when
// no summarizer model is configured. When configured, returns a lambda
// that calls IProvider::generateSummary, accumulates the streamed text,
// and returns it. Errors and empty responses produce empty strings; the
// Deflator handles that by falling back to the deterministic stub.
working_memory::SummarizerFn
buildDeflationSummarizer(const shared::WorkingMemoryConfig &cfg) {
  if (cfg.summarizerProviderId.empty() || cfg.summarizerModelId.empty()) {
    return nullptr;
  }
  const std::string providerId = cfg.summarizerProviderId;
  const std::string modelId = cfg.summarizerModelId;
  return [providerId, modelId](const std::string &toolName,
                               const std::string &toolArgs,
                               const std::string &body,
                               std::uint32_t budgetTokens,
                               std::atomic<bool> *abort) -> std::string {
    auto provider =
        firmius::provider::ProviderRegistry::instance().getProvider(providerId);
    if (!provider) {
      return "";
    }
    std::ostringstream prompt;
    prompt
        << "You are summarizing a tool result body so it can be replaced "
           "in the agent's context window with a compact stub. Goals: "
           "preserve concrete facts, file paths, names, counts, and any "
           "explicit values the agent might want later. Discard prose, "
           "ASCII art, padding, and obvious filler. Aim for under "
        << budgetTokens
        << " tokens. Return ONLY the summary, no preface, no JSON.\n\n";
    if (!toolName.empty()) {
      prompt << "Tool: " << toolName << "\n";
    }
    if (!toolArgs.empty()) {
      prompt << "Args: " << toolArgs << "\n";
    }
    prompt << "Body:\n" << body;

    shared::AgentHistory emptyHistory;
    std::string accumulated;
    accumulated.reserve(static_cast<std::size_t>(budgetTokens) * 4);
    try {
      provider->generateSummary(
          modelId, emptyHistory, prompt.str(),
          [&](const shared::StreamEvent &ev) {
            if (const auto *txt = std::get_if<shared::TextChunk>(&ev)) {
              accumulated += txt->delta;
            } else if (const auto *th =
                           std::get_if<shared::ThinkingChunk>(&ev)) {
              // Thinking content is not part of the summary text, but some
              // providers may emit usable text only via the compaction
              // event types below. Ignore.
              (void)th;
            } else if (const auto *ct =
                           std::get_if<shared::AgentCompactionText>(&ev)) {
              accumulated += ct->delta;
            } else if (const auto *cth =
                           std::get_if<shared::AgentCompactionThinking>(&ev)) {
              (void)cth;
            }
          },
          abort);
    } catch (...) {
      Logger::instance().logDebug("Agent: title generation stream failed");
      return "";
    }
    // Trim leading whitespace; many models emit a leading newline.
    std::size_t start = 0;
    while (start < accumulated.size() &&
           std::isspace(static_cast<unsigned char>(accumulated[start]))) {
      ++start;
    }
    if (start > 0) accumulated.erase(0, start);
    return accumulated;
  };
}

} // namespace

using namespace firmius::shared;

std::uint64_t Agent::nowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

AgentTurn Agent::makeInternalNudgeTurn(const std::string &turnIdPrefix,
                                       const std::string &text,
                                       Role role) const {
  AgentTurn nudgeTurn;
  nudgeTurn.turnId =
      turnIdPrefix +
      std::to_string(context.history ? context.history->turns.size() : 0);

  Message nudgeMsg;
  nudgeMsg.role = role;
  nudgeMsg.visibility = MessageVisibility::Internal;
  nudgeMsg.content.push_back(TextContent{text});
  auto now = std::chrono::system_clock::now();
  nudgeMsg.timestamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count());
  nudgeTurn.messages.push_back(std::move(nudgeMsg));
  return nudgeTurn;
}

void Agent::appendTurnToHistory(const AgentTurn &turn) {
  {
    std::lock_guard<std::mutex> lk(historyMutex_);
    context.history->turns.push_back(turn);
  }
  if (context.config.persistHistory && journaler) {
    journaler->appendTurn(turn);
  }
  // Working-memory layer: enqueue a compact queryable representation of
  // this turn for asynchronous embedding. The worker is per-thread and
  // is created lazily by the request-shaping path; here we look it up
  // best-effort and skip if it isn't ready yet.
  if (context.config.workingMemory.enabled && context.history &&
      !context.history->threadId.empty()) {
    std::string queryText;
    queryText.reserve(512);
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (const auto *txt = std::get_if<TextContent>(&part)) {
          if (queryText.size() + txt->text.size() < 4096) {
            queryText += txt->text;
            queryText += '\n';
          }
        } else if (const auto *tc = std::get_if<ToolCallContent>(&part)) {
          queryText += tc->name;
          queryText += ' ';
          if (queryText.size() + tc->args.size() < 4096) {
            queryText += tc->args;
            queryText += '\n';
          }
        } else if (const auto *tr = std::get_if<ToolResultContent>(&part)) {
          // Use only the first 1KB of the tool result body for embedding —
          // good enough for retrieval, keeps embedding cost bounded.
          const std::size_t take = std::min<std::size_t>(1024, tr->result.size());
          queryText.append(tr->result, 0, take);
          queryText += '\n';
        }
      }
    }
    if (!queryText.empty()) {
      try {
        const std::string threadDir = ThreadManager::threadDirectoryPath(
            ThreadManager::defaultBasePath(), context.history->threadId);
        auto wmWorker =
            working_memory::WorkingMemoryWorker::instance().forThread(
                context.history->threadId, threadDir,
                working_memory::deterministicEmbedFn());
        if (wmWorker) {
          wmWorker->enqueueEmbedding(turn.turnId, std::move(queryText));
        }
      } catch (...) {
        // Best-effort; embedding is non-critical to correctness.
      }
    }
  }
}

Agent::Agent(AgentContext ctx, std::shared_ptr<shared::IEnvironment> env,
             std::shared_ptr<shared::IPermissions> perms, ToolRegistry &reg,
             std::shared_ptr<Journaler> jnl)
    : context(std::move(ctx)), environment_(std::move(env)),
      permissions_(std::move(perms)), toolRegistry(reg), journaler(jnl) {
  if (!context.history) {
    context.history = std::make_shared<AgentHistory>();
  }

  provider = firmius::provider::ProviderRegistry::instance().getProvider(
      context.config.providerId);
  if (!provider) {
    auto preferred = getPreferredModel();
    provider = firmius::provider::ProviderRegistry::instance().getProvider(
        preferred.providerId);
    if (!provider) {
      throw std::runtime_error(
          "Unknown provider: " + context.config.providerId +
          " (Fallback failed: " + preferred.providerId + ")");
    }
    context.config.providerId = preferred.providerId;
    context.config.modelId = preferred.modelId;
    if (preferred.variantName) {
      context.config.modelVariant = *preferred.variantName;
    }
  }

  initializeMcpServers();
}

std::shared_ptr<mcp::McpClient> Agent::getMcpClient(const std::string &serverName, shared::ToolContext &toolCtx) {
  const auto &config = shared::ConfigLoader::instance().getConfig();
  const auto it = config.mcpServers.find(serverName);
  if (it == config.mcpServers.end()) {
    return nullptr;
  }

  const auto &server = it->second;
  if (!server.enabled) {
    return nullptr;
  }

  return mcp::McpManager::shared().getOrCreateClient(serverName, [&]() {
    if (server.transport == "stdio") {
      if (shared::StringUtil::trim(server.command).empty()) {
        return std::shared_ptr<mcp::McpClient>{};
      }

      const std::string command = mcp_tools::composeCommand(server);
      auto process = toolCtx.host.spawn(command, server.cwd, server.env);
      if (!process) {
        return std::shared_ptr<mcp::McpClient>{};
      }

      return std::make_shared<mcp::McpClient>(std::move(process));
    }

    if (server.transport == "http") {
      if (shared::StringUtil::trim(server.url).empty()) {
        return std::shared_ptr<mcp::McpClient>{};
      }

      mcp::McpHttpTransportConfig httpConfig;
      httpConfig.url = server.url;
      httpConfig.authHeader = server.authHeader;
      httpConfig.authBearerToken = server.authBearerToken;
      httpConfig.allowInsecureTls = server.allowInsecureTls;
      httpConfig.caCertPath = server.caCertPath;

      return std::make_shared<mcp::McpClient>(httpConfig);
    }

    return std::shared_ptr<mcp::McpClient>{};
  });
}

void Agent::initializeMcpServers() {
  const auto &config = shared::ConfigLoader::instance().getConfig();
  for (const auto &[name, server] : config.mcpServers) {
    if (!server.enabled) {
      continue;
    }

    // Create a transient tool context for initialization
    // We use a dummy cancel token that is never set to true during init
    static std::atomic<bool> neverCancel{false};
    ToolContext toolCtx{*environment_->getHost(), *this, "", &neverCancel,
                        &provider::LLMSearchProviderRegistry::instance()};

    std::shared_ptr<mcp::McpClient> client;
    try {
      client = getMcpClient(name, toolCtx);
    } catch (const std::exception &e) {
      // Surface and skip — common causes are docker exec not ready, missing
      // binary in the sandbox, network, etc. None should be fatal to the
      // agent itself (the agent can run without this MCP server).
      Logger::instance().logWarning("Agent: failed to spawn MCP server '" + name
                + "': " + e.what());
      continue;
    }
    if (!client) {
      continue;
    }

    try {
      if (!client->isInitialized()) {
        client->initialize(mcp_tools::kDefaultTimeoutMs);
      }

      const rapidjson::Document toolsResponse =
          client->listTools(mcp_tools::kDefaultTimeoutMs);
      const auto &toolsResult = toolsResponse["result"];
      if (toolsResult.IsObject() && toolsResult.HasMember("tools") &&
          toolsResult["tools"].IsArray()) {
        std::vector<std::string> toolNames;
        std::vector<AgentState::DynamicMcpTool> toolDefs;
        for (const auto &tool : toolsResult["tools"].GetArray()) {
          if (tool.IsObject() && tool.HasMember("name") &&
              tool["name"].IsString()) {
            std::string toolName = tool["name"].GetString();
            toolNames.push_back(toolName);

            AgentState::DynamicMcpTool def;
            def.name = toolName;
            if (tool.HasMember("description") && tool["description"].IsString()) {
              def.description = tool["description"].GetString();
            }
            if (tool.HasMember("inputSchema") && tool["inputSchema"].IsObject()) {
              rapidjson::StringBuffer sb;
              rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
              tool["inputSchema"].Accept(writer);
              def.inputSchema = sb.GetString();
            } else {
              def.inputSchema = R"({"type":"object","additionalProperties":true})";
            }
            toolDefs.push_back(std::move(def));
          }
        }
        context.state.loadedMcpServers.push_back(name);
        context.state.loadedMcpTools[name] = std::move(toolNames);
        context.state.loadedMcpToolDefinitions[name] = std::move(toolDefs);
      }
    } catch (const std::exception &e) {
      Logger::instance().logWarning("Agent: failed to initialize MCP server '" + name
                + "': " + e.what());
    }
  }
}

Agent::~Agent() {
  for (const auto &id : backgroundProcessIds) {
    try {
      environment_->getHost()->killBackgroundProcess(id);
    } catch (...) {
      Logger::instance().logDebug("Agent: failed to kill background process during destruction");
    }
  }
  if (environment_->getHost())
    environment_->getHost()->destroy();
}

void Agent::reset() {
  context.history->turns.clear();
  // WORKAROUND: `context.aggregateMetrics = {};` triggers a SIGSEGV in
  // QuotaMetrics::operator= when running benchmarks (UAF on the embedded
  // strings/vectors/maps inside aggregateMetrics). Pre-existing bug in
  // AgentContext lifecycle, hit by SWEBench's reset+reuse path. Zeroing
  // the POD-only sub-structs is safe; the next provider call overwrites
  // the rest anyway. Investigate with ASan when there's time.
  context.aggregateMetrics.tokens = {};
  context.aggregateMetrics.timing = {};
  context.aggregateMetrics.estimatedCostUsd = 0.0;
  context.state = {};
  interrupted = false;
  running = false;
  {
    std::lock_guard<std::mutex> lock(cancelTokenMutex_);
    activeRunCancelToken_.reset();
    activeRunAbortController_.reset();
  }

  for (const auto &id : backgroundProcessIds) {
    try {
      environment_->getHost()->killBackgroundProcess(id);
    } catch (...) {
      Logger::instance().logDebug("Agent: failed to kill background process during reset");
    }
  }
  backgroundProcessIds.clear();
}

std::string Agent::resolvePath(const std::string &inputPath) const {
  return environment_->getWorkspace().resolvePath(inputPath);
}

std::shared_ptr<shared::IHost> Agent::getHost() {
  return environment_->getHost();
}

ModelChoice Agent::getPreferredModel() const {
  const auto &config = shared::ConfigLoader::instance().getConfig();
  auto persona = context.config.personaName;

  auto useDefaultRoute = [&config]() {
    ModelChoice choice;
    choice.providerId = config.defaultProviderId;
    choice.modelId = config.defaultModelId;
    if (!config.defaultModelVariant.empty()) {
      choice.variantName = config.defaultModelVariant;
    }
    return choice;
  };

  auto findCategory =
      [&config](const std::string &name) -> const shared::ModelRouteCategory * {
    auto it = config.modelRouterCategories.find(name);
    if (it == config.modelRouterCategories.end()) {
      return nullptr;
    }
    return &it->second;
  };

  auto resolveFromCategory = [&](const std::string &categoryName,
                                 const shared::ModelRouteCategory &category)
      -> std::optional<ModelChoice> {
    if (category.models.empty()) {
      return std::nullopt;
    }

    // Check for runtime preferred model for this category
    std::string preferredKey =
        shared::ConfigLoader::instance().getPreferredModelKey(categoryName);
    if (!preferredKey.empty()) {
      for (const auto &opt : category.models) {
        if (opt.providerId + ":" + opt.modelId == preferredKey) {
          ModelChoice choice;
          choice.providerId = opt.providerId;
          choice.modelId = opt.modelId;
          choice.variantName = opt.variantName;
          return choice;
        }
      }
    }

    // Default to first model
    const auto &opt = category.models.front();
    ModelChoice choice;
    choice.providerId = opt.providerId;
    choice.modelId = opt.modelId;
    choice.variantName = opt.variantName;
    return choice;
  };

  auto it_purpose = config.purposeRoutes.find(persona);
  if (it_purpose != config.purposeRoutes.end() && !it_purpose->second.empty()) {
    const std::string mapped_category = it_purpose->second;
    if (const auto *category = findCategory(mapped_category)) {
      if (auto choice = resolveFromCategory(mapped_category, *category)) {
        return *choice;
      }
    }
  }

  if (!config.defaultRouteCategory.empty()) {
    if (const auto *category = findCategory(config.defaultRouteCategory)) {
      if (auto choice =
              resolveFromCategory(config.defaultRouteCategory, *category)) {
        return *choice;
      }
    }
  }

  return useDefaultRoute();
}

void Agent::interrupt() {
  interrupted = true;
  std::shared_ptr<std::atomic<bool>> runCancelToken;
  std::shared_ptr<shared::AbortController> runAbortController;
  {
    std::lock_guard<std::mutex> lock(cancelTokenMutex_);
    runCancelToken = activeRunCancelToken_;
    runAbortController = activeRunAbortController_;
  }
  if (runCancelToken) {
    runCancelToken->store(true);
  }
  if (runAbortController) {
    runAbortController->cancel();
  }
}

void Agent::clearInterrupt() { interrupted = false; }

void Agent::setModel(const std::string &providerId,
                     const std::string &modelId) {
  setModelInternal(providerId, modelId, std::nullopt);
}

void Agent::compactNow(
    std::function<void(const shared::StreamEvent &)> onEvent) {
  compactContext(std::move(onEvent));
}

void Agent::setModel(const std::string &providerId, const std::string &modelId,
                     const std::string &variantName) {
  setModelInternal(providerId, modelId, variantName);
}

void Agent::setModelInternal(const std::string &providerId,
                             const std::string &modelId,
                             const std::optional<std::string> &variantName) {
  auto newProvider =
      firmius::provider::ProviderRegistry::instance().getProvider(providerId);
  if (!newProvider) {
    auto preferred = getPreferredModel();
    newProvider = firmius::provider::ProviderRegistry::instance().getProvider(
        preferred.providerId);
    if (!newProvider) {
      throw std::runtime_error("Unknown provider: " + providerId +
                               " (Fallback failed: " + preferred.providerId +
                               ")");
    }
    std::lock_guard<std::mutex> lock(modelSwitchMutex);
    if (running.load()) {
      pendingModelSwitch_ = PendingModelSwitch{
          preferred.providerId, preferred.modelId, preferred.variantName};
      return;
    }
    context.config.providerId = preferred.providerId;
    context.config.modelId = preferred.modelId;
    context.config.modelVariant = preferred.variantName.value_or("");
    provider = newProvider;
    return;
  }

  std::lock_guard<std::mutex> lock(modelSwitchMutex);
  if (running.load()) {
    pendingModelSwitch_ = PendingModelSwitch{providerId, modelId, variantName};
    return;
  }

  context.config.providerId = providerId;
  context.config.modelId = modelId;
  if (variantName.has_value()) {
    context.config.modelVariant = *variantName;
  }
  provider = newProvider;
}

void Agent::applyPendingModelSwitchIfAny() {
  std::optional<PendingModelSwitch> pending;
  {
    std::lock_guard<std::mutex> lock(modelSwitchMutex);
    if (!pendingModelSwitch_.has_value()) {
      return;
    }
    pending = pendingModelSwitch_;
    pendingModelSwitch_.reset();
  }

  auto newProvider =
      firmius::provider::ProviderRegistry::instance().getProvider(
          pending->providerId);
  if (!newProvider) {
    throw std::runtime_error("Unknown provider: " + pending->providerId);
  }

  context.config.providerId = pending->providerId;
  context.config.modelId = pending->modelId;
  if (pending->variantName.has_value()) {
    context.config.modelVariant = *pending->variantName;
  }
  provider = newProvider;
}

std::string Agent::spawnProcess(const std::string &command,
                                const std::string &toolCallId,
                                const std::string &cwd,
                                const std::map<std::string, std::string> &env,
                                bool monitorCompletion) {
  return environment_->getProcessManager().spawnProcess(
      command, toolCallId, cwd, env, monitorCompletion);
}

shared::ProcessSnapshot Agent::inspectProcess(const std::string &id) {
  return environment_->getProcessManager().inspectProcess(id);
}

void Agent::writeToProcess(const std::string &id, const std::string &data) {
  environment_->getProcessManager().writeToProcess(id, data);
}

void Agent::registerProcessId(const std::string &id) {
  environment_->getProcessManager().registerProcessId(id);
}

void Agent::emitProcessSpawned(const std::string &processId,
                               const std::string &toolCallId,
                               const std::string &command) {
  environment_->getProcessManager().emitProcessSpawned(processId, toolCallId,
                                                       command);
}

void Agent::addBlockingProcessId(const std::string &id) {
  environment_->getProcessManager().addBlockingProcessId(id);
}

void Agent::removeBlockingProcessId(const std::string &id) {
  environment_->getProcessManager().removeBlockingProcessId(id);
}

std::vector<std::string> Agent::getBlockingProcessIds() {
  return environment_->getProcessManager().getBlockingProcessIds();
}

bool Agent::hasReadFile(const std::string &path) const {
  return environment_->getWorkspace().hasReadFile(path);
}

void Agent::markFileAsRead(const std::string &path) {
  if (environment_->getWorkspace().hasReadFile(path)) {
    context.aggregateMetrics.memory.redundantReadCount += 1;
  }
  environment_->getWorkspace().markFileAsRead(path);
}

bool Agent::hasFullyReadFile(const std::string &path) const {
  return environment_->getWorkspace().hasFullyReadFile(path);
}

void Agent::markFileAsFullyRead(const std::string &path) {
  environment_->getWorkspace().markFileAsFullyRead(path);
}

void Agent::bootstrapHistory(const std::optional<std::string> &task,
                             const std::vector<ImageContent> &images) {
  // 1. Bootstrap System Message
  if (context.history->turns.empty()) {
    auto toolDefs = getProviderToolDefinitions(context, toolRegistry);
    std::string personaName = context.config.personaName.empty()
                                  ? "aster"
                                  : context.config.personaName;
    std::string effectivePersonaName = personaName;
    Persona persona;
    try {
      persona = PurposeLoader::load(personaName);
    } catch (const std::exception &e) {
      auto available = PurposeLoader::listPurposes();
      if (available.empty()) {
        throw;
      }
      effectivePersonaName = available.front();
      Logger::instance().logWarning("Agent: failed to load persona '" + personaName + "' ("
                + e.what() + "); falling back to '" + effectivePersonaName + "'");
      persona = PurposeLoader::load(effectivePersonaName);
    }
    if (effectivePersonaName != personaName) {
      context.config.personaName = effectivePersonaName;
    }
    PurposeLoader::loadProjectRootAgentsIntoContext(context);
    std::string toolBlock = PurposeLoader::buildToolsBlock(toolDefs);

    std::string protocolAddon =
        "\n\n# PROTOCOL STRICTNESS\n"
        "- If you are calling a tool, your message MUST contain ONLY the tool "
        "call JSON.\n"
        "- Do NOT include any other text or thinking when calling a tool.\n";

    std::string systemPrompt =
        PurposeLoader::composeSystemPrompt(persona, context, toolBlock) +
        protocolAddon;
    context.identity.systemPrompt = systemPrompt;

    AgentTurn turn;
    turn.turnId = "bootstrap-system";
    Message msg;
    msg.role = Role::System;
    msg.content.push_back(TextContent{systemPrompt});
    auto now = std::chrono::system_clock::now();
    msg.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
    turn.messages.push_back(msg);
    context.history->turns.push_back(turn);
    if (context.config.persistHistory && journaler)
      journaler->appendTurn(turn);
  }

  // 2. Add Task as User Message when explicitly re-tasking.
  if (task.has_value()) {
    AgentTurn taskTurn;
    taskTurn.turnId =
        "user-task-" + std::to_string(context.history->turns.size());
    Message taskMsg;
    taskMsg.role = Role::User;
    taskMsg.content.push_back(TextContent{*task});

    for (const auto &img : images) {
      taskMsg.content.push_back(img);
    }
    auto now = std::chrono::system_clock::now();
    taskMsg.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
    taskTurn.messages.push_back(taskMsg);
    context.history->turns.push_back(taskTurn);
    if (context.config.persistHistory && journaler) {
      journaler->appendTurn(taskTurn);
    }
  }
}

void Agent::run(const std::string &task,
                std::function<void(const shared::StreamEvent &)> onEvent,
                const std::vector<ImageContent> &images) {
  runImpl(task, std::move(onEvent), images);
}

void Agent::resume(std::function<void(const shared::StreamEvent &)> onEvent) {
  runImpl(std::nullopt, std::move(onEvent), {});
}

void Agent::runImpl(const std::optional<std::string> &task,
                    std::function<void(const shared::StreamEvent &)> onEvent,
                    const std::vector<ImageContent> &images) {
  {
    std::lock_guard<std::mutex> lock(callbackMutex);
    eventCallback = onEvent;
  }

  // Guard against concurrent runs with mutex
  std::lock_guard<std::mutex> lock(runMutex_);
  if (running.load()) {
    throw std::runtime_error("Agent is already running");
  }
  running = true;
  interrupted = false;
  const auto runCancelToken = std::make_shared<std::atomic<bool>>(false);
  const auto runAbortController = std::make_shared<shared::AbortController>();
  {
    std::lock_guard<std::mutex> lock(cancelTokenMutex_);
    activeRunCancelToken_ = runCancelToken;
    activeRunAbortController_ = runAbortController;
  }
  auto markRunStopped = [this, runCancelToken, runAbortController]() {
    {
      std::lock_guard<std::mutex> runStateLock(runStateMutex_);
      running = false;
    }
    runStateCv_.notify_all();
    {
      std::lock_guard<std::mutex> cancelLock(cancelTokenMutex_);
      if (activeRunCancelToken_ == runCancelToken) {
        activeRunCancelToken_.reset();
      }
      if (activeRunAbortController_ == runAbortController) {
        activeRunAbortController_.reset();
      }
    }
  };
  struct RunFinalizer {
    std::function<void()> fn;
    ~RunFinalizer() {
      if (fn) {
        fn();
      }
    }
  } runFinalizer{markRunStopped};
  booting = false;
  context.state.currentStatus = AgentStatus::ProviderWaiting;
  context.state.fatalError = std::nullopt;
  applyPendingModelSwitchIfAny();

  // Bind HookState to this thread for hook state reads/writes during dispatch.
  if (context.history && !context.history->threadId.empty()) {
    hooks::HookState::instance().bindThread(context.history->threadId);
  }

  if (context.history->turns.empty() || task.has_value()) {
    bootstrapHistory(task, images);
  }

  bool taskFinished = false;
  int maxTurns = context.config.maxTurns > 0 ? context.config.maxTurns : 200;
  int turnCount = 0;
  int consecutiveProviderFailures = 0;
  const int maxProviderRetries = 3;
  int consecutiveTruncatedToolRetries = 0;
  const int maxTruncatedToolRetries = 2;
  int consecutiveEmptyProviderResponses = 0;
  const int maxEmptyProviderRetries =
      context.identity.parentId.empty() ? 2 : 0;
  int consecutiveInsanityRetries = 0;
  struct BlockedStopRecoveryState {
    bool active = false;
    bool sawMeaningfulNewTurn = false;
    int emptyReentryCount = 0;
    std::string stopReason;
    std::string finalMessage;
  } blockedStopRecovery;

  auto resetBlockedStopRecovery = [&]() {
    blockedStopRecovery.active = false;
    blockedStopRecovery.sawMeaningfulNewTurn = false;
    blockedStopRecovery.emptyReentryCount = 0;
    blockedStopRecovery.stopReason.clear();
    blockedStopRecovery.finalMessage.clear();
  };
  std::optional<std::string> lastTodoContinuationFingerprint;
  auto hasQueuedUserTurnPending = [this]() {
    if (!context.history || context.history->turns.empty()) {
      return false;
    }
    const auto &lastTurn = context.history->turns.back();
    return lastTurn.turnId.rfind("user-task-", 0) == 0 &&
           !lastTurn.messages.empty() &&
           lastTurn.messages.front().role == Role::User;
  };
  bool agentStopEventFired = false;
  auto fireAgentStopGate = [&](const std::string &finalMessage,
                               const std::string &reason) -> bool {
    if (blockedStopRecovery.active && !blockedStopRecovery.sawMeaningfulNewTurn) {
      return true;
    }
    hooks::EventPayload payload;
    payload.threadId = context.history ? context.history->threadId : "";
    payload.agentId = context.identity.id;
    payload.persona = context.config.personaName;
    payload.activeMode = context.state.activeMode;
    payload.extra["stop_reason"] = reason;
    payload.extra["final_message"] = finalMessage;
    auto fired = hooks::HookDispatcher::fire(WorkflowEventKind::AgentStop, payload);
    if (fired.blocked) {
      blockedStopRecovery.active = true;
      blockedStopRecovery.sawMeaningfulNewTurn = false;
      blockedStopRecovery.emptyReentryCount = 0;
      blockedStopRecovery.stopReason = reason;
      blockedStopRecovery.finalMessage = finalMessage;
    } else {
      resetBlockedStopRecovery();
    }

    agentStopEventFired = true;
    if (!fired.injectedReminders.empty()) {
      std::string combined;
      for (std::size_t i = 0; i < fired.injectedReminders.size(); ++i) {
        if (i > 0) {
          combined += "\n";
        }
        combined += fired.injectedReminders[i];
      }
      appendTurnToHistory(makeInternalNudgeTurn(
          "hook-agent-stop-", combined,
          fired.blocked ? Role::User : Role::System));
    }
    return fired.blocked;
  };
  auto retryTruncatedToolStream = [&](const std::string &details,
                                      int httpStatus) -> bool {
    consecutiveTruncatedToolRetries++;
    if (consecutiveTruncatedToolRetries > maxTruncatedToolRetries) {
      return false;
    }
    constexpr int retryDelayMs = 250;
    onEvent(StreamRetrying{consecutiveTruncatedToolRetries,
                           maxTruncatedToolRetries, httpStatus,
                           retryDelayMs, "Tool-call stream truncated, retrying",
                           "", details});
    appendTurnToHistory(makeInternalNudgeTurn(
        "tool-stream-retry-",
        buildToolStreamRetryNudge(details, consecutiveTruncatedToolRetries,
                                  maxTruncatedToolRetries)));
    if (!interruptibleSleep(std::chrono::milliseconds(retryDelayMs),
                            runAbortController, runCancelToken.get())) {
      context.state.currentStatus = AgentStatus::Cancelled;
      return true;
    }
    return true;
  };
  while (!taskFinished && turnCount < maxTurns && !runCancelToken->load()) {
    applyPendingModelSwitchIfAny();

    // --- CONTEXT COMPACTION ---
    try {
      auto model = provider->getModelInfo(context.config.modelId);
      bool forceCompact = (std::getenv("FORCE_COMPACTION") != nullptr);
      if (forceCompact || (model.contextWindow > 0 &&
                                  context.aggregateMetrics.tokens.contextSize >
                                      model.contextWindow * 0.8)) {
        compactContext(onEvent);
      }
      if (runCancelToken->load())
        break;
    } catch (...) {
      // Compaction is best-effort
    }

    // Drain any pending internal queue messages (e.g., fleet edit notices)
    // at the start of each turn, so the agent sees them before its next action.
    {
      std::string tid = context.history->threadId;
      std::string aid = context.identity.id;
      if (!tid.empty() && !aid.empty()) {
        Harness::instance().drainInternalQueueForAgent(aid, tid, true);
      }
    }

    turnCount++;

    std::vector<ToolCallChunk> accumulatedToolChunks;
    std::vector<ToolCall> finalizedToolCalls;
    std::string fullResponse;
    std::string fullThinking;
    bool responseTruncated = false;
    bool thinkingTruncated = false;
    std::string lastThinkingSignature;
    AgentMetrics turnMetrics;
    StopReason turnStopReason = StopReason::Stop;
    std::string streamError;
    int streamErrorStatus = 0;
    bool sawContent = false;
    bool sawThinking = false;
    bool sawTool = false;
    std::uint32_t syntheticToolCallIdSerial = 0;

    auto appendVisibleText = [&](const std::string &delta) {
      if (delta.empty()) {
        return;
      }
      onEvent(TextChunk{delta});
      for (unsigned char c : delta) {
        if (!std::isspace(c)) {
          sawContent = true;
          break;
        }
      }
      if (!responseTruncated &&
          fullResponse.size() + delta.size() > kMaxAccumulatedResponseBytes) {
        std::size_t remaining = kMaxAccumulatedResponseBytes - fullResponse.size();
        if (remaining > 0) {
          fullResponse.append(delta, 0, remaining);
        }
        responseTruncated = true;
        Logger::instance().logWarning("Agent: response buffer capped at "
                  + std::to_string(kMaxAccumulatedResponseBytes) + " bytes for turn "
                  + std::to_string(turnCount));
      } else if (!responseTruncated) {
        fullResponse += delta;
      }
    };

    auto appendVisibleThinking = [&](const std::string &delta) {
      if (delta.empty()) {
        return;
      }
      onEvent(ThinkingChunk{delta, ""});
      sawThinking = true;
      if (!thinkingTruncated &&
          fullThinking.size() + delta.size() > kMaxAccumulatedThinkingBytes) {
        std::size_t remaining = kMaxAccumulatedThinkingBytes - fullThinking.size();
        if (remaining > 0) {
          fullThinking.append(delta, 0, remaining);
        }
        thinkingTruncated = true;
        Logger::instance().logWarning("Agent: thinking buffer capped at "
                  + std::to_string(kMaxAccumulatedThinkingBytes) + " bytes for turn "
                  + std::to_string(turnCount));
      } else if (!thinkingTruncated) {
        fullThinking += delta;
      }
    };

    auto persistAssistantTurn = [&](StopReason stopReason,
                                    bool includeToolCalls) {
      const bool hasVisibleContent =
          !fullThinking.empty() || !fullResponse.empty();
      const bool hasPersistableToolCalls =
          includeToolCalls && !finalizedToolCalls.empty();
      if (!hasVisibleContent && !hasPersistableToolCalls) {
        return false;
      }

      AgentTurn assistantTurn;
      assistantTurn.turnId =
          "assistant-" + std::to_string(context.history->turns.size());
      assistantTurn.stopReason = stopReason;
      assistantTurn.metrics = turnMetrics;

      context.aggregateMetrics += turnMetrics;

      Message assistantMsg;
      assistantMsg.role = Role::Assistant;
      if (!fullThinking.empty()) {
        assistantMsg.content.push_back(
            ThinkingContent{clampPersistedAssistantBody(
                                fullThinking, kMaxPersistedThinkingBytes,
                                "thinking"),
                            lastThinkingSignature});
      }
      if (!fullResponse.empty()) {
        assistantMsg.content.push_back(TextContent{clampPersistedAssistantBody(
            fullResponse, kMaxPersistedResponseBytes, "response")});
      }
      if (hasPersistableToolCalls) {
        for (const auto &call : finalizedToolCalls) {
          assistantMsg.content.push_back(
              ToolCallContent{call.id, call.name, call.args});
        }
      }

      assistantMsg.timestamp = nowMs();
      assistantTurn.messages.push_back(assistantMsg);
      context.history->turns.push_back(assistantTurn);
      if (context.config.persistHistory && journaler) {
        journaler->appendTurn(assistantTurn);
      }

      onEvent(AgentTurnCompleted{context.identity.id, assistantTurn,
                                 context.aggregateMetrics,
                                 context.identity.parentId});
      return true;
    };

    auto persistCancelledToolResults =
        [&](const std::vector<ToolCall> &calls,
            const std::string &message) {
          if (calls.empty()) {
            return;
          }

          AgentTurn toolResultTurn;
          toolResultTurn.turnId =
              "tools-" + std::to_string(context.history->turns.size());
          for (const auto &call : calls) {
            Message msg;
            msg.role = Role::ToolResult;
            msg.content.push_back(
                ToolResultContent{call.id, message, false, "", ""});
            msg.timestamp = nowMs();
            toolResultTurn.messages.push_back(std::move(msg));
          }

          context.history->turns.push_back(toolResultTurn);
          if (context.config.persistHistory && journaler) {
            journaler->appendTurn(toolResultTurn);
          }
          onEvent(AgentTurnCompleted{context.identity.id, toolResultTurn,
                                     context.aggregateMetrics,
                                     context.identity.parentId});
        };

    try {
      // --- State: ProviderWaiting ---
      context.state.currentStatus = AgentStatus::ProviderWaiting;
      onEvent(ProviderWaiting{});

      firmius::provider::ProviderOptions opts;
      opts.modelId = context.config.modelId;
      try {
        auto modelInfo = provider->getModelInfo(context.config.modelId);
        for (const auto &v : modelInfo.variants) {
          if (v.variantName == context.config.modelVariant) {
            opts.modelVariantJson = v.extraMetadataJson;
            break;
          }
        }
      } catch (...) {
        Logger::instance().logDebug("Agent: failed to resolve model variant");
      }
      opts.temperature = context.config.temperature;
      if (context.config.maxTokens.has_value()) {
        opts.maxTokens = context.config.maxTokens;
      }
      opts.stop = context.config.stop;
      opts.tools = getProviderToolDefinitions(context, toolRegistry);
      opts.abortSignal = runCancelToken.get();
      opts.abortController = runAbortController;

      AgentHistory requestHistory;
      const shared::HeuristicTokenizer tokenizer;

      // First, the working-memory layer assembles the request from the
      // canonical journal-backed history. Below the buffer threshold it's
      // pure pass-through; above buffer it pins/evicts/deflates per the
      // configured policy. The WorkingMemoryWorker for this thread is
      // lazy-created on first use.
      working_memory::WorkingMemoryReport memoryReport;
      AgentHistory shapedHistory;
      {
        working_memory::WorkingMemoryInputs wmInputs;
        wmInputs.tokenizer = &tokenizer;
        std::uint32_t actorContextWindow = 0;
        if (context.config.workingMemory.actorContextWindowOverride > 0) {
          actorContextWindow =
              context.config.workingMemory.actorContextWindowOverride;
        } else {
          try {
            actorContextWindow =
                provider->getModelInfo(context.config.modelId).contextWindow;
          } catch (...) {
            Logger::instance().logDebug("Agent: failed to get model context window for working memory");
          }
        }
        wmInputs.actorContextWindow = actorContextWindow;

        std::shared_ptr<working_memory::ThreadWorkingMemoryWorker> wmWorker;
        if (context.history && !context.history->threadId.empty() &&
            context.config.workingMemory.enabled) {
          const std::string threadDir =
              ThreadManager::threadDirectoryPath(
                  ThreadManager::defaultBasePath(),
                  context.history->threadId);
          wmWorker = working_memory::WorkingMemoryWorker::instance().forThread(
              context.history->threadId, threadDir,
              working_memory::deterministicEmbedFn());
          if (wmWorker) {
            wmInputs.archive = &wmWorker->archive();
            wmInputs.relevanceQuery = [wmWorker](const std::string& q,
                                                 std::size_t k) {
              return wmWorker->queryRelevant(q, k);
            };
          }
        }
        // Optional LLM-backed deflation summarizer. When the user has
        // configured WorkingMemoryConfig.summarizerProviderId+modelId,
        // pass a SummarizerFn into the assembler so deflated tool result
        // bodies are replaced with model-generated summaries instead of
        // deterministic byte-count stubs. Otherwise the Deflator falls
        // back to deterministic stubs (no LLM call).
        wmInputs.synchronousSummarizer =
            buildDeflationSummarizer(context.config.workingMemory);

        // Async upgrade hook: when both a summarizer is configured and a
        // working-memory worker exists, fire jobs onto the worker's
        // background deflation thread so the LLM call doesn't block the
        // agent's hot path. The deterministic stub stays in place until
        // the worker completes; the next request shaping picks up the
        // upgraded body. When unconfigured, the assembler falls back to
        // the synchronous path (still bounded latency for low-occupancy
        // sessions because deflation only runs above target).
        if (wmWorker && wmInputs.synchronousSummarizer) {
          auto summarizerCopy = wmInputs.synchronousSummarizer;
          auto historyShared = context.history;
          std::mutex* historyMu = &historyMutex_;
          auto journalerShared = journaler;
          std::weak_ptr<working_memory::ThreadWorkingMemoryWorker>
              workerWeak = wmWorker;
          wmInputs.asyncUpgrade =
              [summarizerCopy, historyShared, historyMu, journalerShared,
               workerWeak](const std::string& turnId, std::size_t mi,
                           std::size_t pi, const std::string& toolName,
                           const std::string& toolArgs,
                           const std::string& body,
                           const std::string& archiveId) {
                auto worker = workerWeak.lock();
                if (!worker) return;
                working_memory::ThreadWorkingMemoryWorker::DeflationJob job;
                job.turnId = turnId;
                job.messageIndex = mi;
                job.partIndex = pi;
                job.toolName = toolName;
                job.toolArgs = toolArgs;
                job.body = body;
                job.archiveId = archiveId;
                job.budgetTokens = 256;
                job.summarizer = summarizerCopy;
                job.history = historyShared;
                job.historyMutex = historyMu;
                job.journaler = journalerShared;
                worker->enqueueDeflation(std::move(job));
              };
        }

        AgentHistory baseHistory;
        {
          // Snapshot under the history mutex so a background deflation
          // upgrader can't race us on a tr->result string.
          std::lock_guard<std::mutex> hlk(historyMutex_);
          baseHistory = context.history ? *context.history : AgentHistory{};
        }
        shapedHistory = working_memory::assembleWorkingSet(
            context, baseHistory, wmInputs, memoryReport);
      }

      // Codex still gets the shaped history without overlay stickers.
      // Other providers receive shaped history + RuntimeOverlay stickers.
      if (context.config.providerId == "codex") {
        requestHistory = shapedHistory;
      } else {
        // Temporarily swap the context history so RuntimeOverlay layers
        // its stickers on top of the shaped history rather than the raw
        // journal. Restore immediately after.
        auto originalHistory = context.history;
        auto shapedShared = std::make_shared<AgentHistory>(std::move(shapedHistory));
        const_cast<AgentContext&>(context).history = shapedShared;
        requestHistory = runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
            context, *environment_->getHost(), environment_->getWorkspace());
        const_cast<AgentContext&>(context).history = originalHistory;
      }
      // Latch memory metrics into this turn's metrics so they ride along
      // with the AgentTurnCompleted event and feed the aggregate.
      turnMetrics.memory.rawHistoryTokens = memoryReport.rawHistoryTokens;
      turnMetrics.memory.workingSetTokens = memoryReport.workingSetTokens;
      turnMetrics.memory.pinnedTurnCount = memoryReport.pinnedTurnCount;
      turnMetrics.memory.evictedTurnCount = memoryReport.evictedTurnCount;
      turnMetrics.memory.recalledTurnCount = memoryReport.recalledTurnCount;
      turnMetrics.memory.deflatedPartCount = memoryReport.deflatedPartCount;
      turnMetrics.memory.tokensSavedByDeflation =
          memoryReport.tokensSavedByDeflation;
      turnMetrics.memory.tokensSavedByEviction =
          memoryReport.tokensSavedByEviction;
      turnMetrics.memory.tokensSpentOnSummaries =
          memoryReport.tokensSpentOnSummaries;
      turnMetrics.memory.tokensSpentOnEmbeddings =
          memoryReport.tokensSpentOnEmbeddings;
      turnMetrics.memory.tokensSpentOnOverlays =
          memoryReport.tokensSpentOnOverlays;
      turnMetrics.memory.userPromptsRetained = memoryReport.userPromptsRetained;
      turnMetrics.memory.userPromptsTotal = memoryReport.userPromptsTotal;
      turnMetrics.memory.imagePartsRetained = memoryReport.imagePartsRetained;
      turnMetrics.memory.imagePartsTotal = memoryReport.imagePartsTotal;
      turnMetrics.memory.hotPathLatencyMicros =
          memoryReport.hotPathLatencyMicros;
      turnMetrics.memory.aboveBufferThreshold =
          memoryReport.aboveBufferThreshold;
      turnMetrics.memory.aboveTargetThreshold =
          memoryReport.aboveTargetThreshold;
      turnMetrics.memory.aboveEmergencyThreshold =
          memoryReport.aboveEmergencyThreshold;
      const auto requestContextEstimate =
          estimateContextWindowMetrics(tokenizer, requestHistory, opts.tools);
      turnMetrics.context = requestContextEstimate;
      // Insanity detection setup
      StreamSanityDetector::Config detectorConfig;
      detectorConfig.enabled = context.config.insanityDetectionEnabled;
      // NOTE: The default repetition threshold is intentionally conservative.
      // We only want to intervene on truly degenerate output ("stuck" repetition),
      // not on normal, legible responses that happen to reuse a phrase.
      detectorConfig.minPatternLength = 32;
      detectorConfig.maxPatternLength = 512;
      detectorConfig.minConsecutiveRepeats =
          std::max(context.config.insanityRepetitionThreshold, 6);
      detectorConfig.maxTokenThreshold = context.config.insanityMaxTokenThreshold;
      StreamSanityDetector detector(detectorConfig);
      bool insanityDetectedThisTurn = false;
      std::string insanityReason;
      bool insanityAbortTriggered = false;
      provider->stream(requestHistory, opts, [&](const StreamEvent &ev) {
        if (context.state.currentStatus == AgentStatus::ProviderWaiting) {
          if (std::holds_alternative<TextChunk>(ev) ||
              std::holds_alternative<ThinkingChunk>(ev) ||
              std::holds_alternative<ToolCallChunk>(ev) ||
              std::holds_alternative<ToolCall>(ev)) {
            context.state.currentStatus = AgentStatus::Streaming;
          }
        }

        if (auto *txt = std::get_if<TextChunk>(&ev)) {
          appendVisibleText(txt->delta);
          // Insanity detection
          if (!insanityDetectedThisTurn) {
            detector.addDelta(txt->delta);
            auto checkResult = detector.check();
            if (checkResult.isInsane) {
              insanityDetectedThisTurn = true;
              insanityReason = checkResult.reason;
              insanityAbortTriggered = true;
              streamError = "Insanity detected: " + checkResult.reason;
              runAbortController->cancel();
              return;
            }
          }
        } else if (auto *thk = std::get_if<ThinkingChunk>(&ev)) {
          appendVisibleThinking(thk->delta);
          if (!thk->signature.empty()) {
            lastThinkingSignature = thk->signature;
          }
          // Intentionally do NOT run insanity detection on ThinkingChunk.
          // Some providers emit tool-call preparation / scratchpad content as
          // "thinking" deltas, and false positives here are extremely costly.
        } else if (auto *tcc = std::get_if<ToolCallChunk>(&ev)) {
          sawTool = true;
          // Emit immediately so TUI can show "Preparing" state
          onEvent(ev);
          mergeToolCallChunk(accumulatedToolChunks, *tcc,
                             syntheticToolCallIdSerial++, turnCount);
        } else if (auto *tc = std::get_if<ToolCall>(&ev)) {
          sawTool = true;
          onEvent(ev);
          mergeFinalToolCall(finalizedToolCalls, *tc);
        } else if (auto *met = std::get_if<AgentMetrics>(&ev)) {
          // Preserve our pre-stream-set memory snapshot across the
          // provider-supplied metrics overwrite. The provider only emits
          // tokens/timing/quota; memory belongs to the working-memory layer.
          auto memorySnapshot = turnMetrics.memory;
          turnMetrics = *met;
          turnMetrics.memory = memorySnapshot;
          if (turnMetrics.context.buckets.empty()) {
            turnMetrics.context = requestContextEstimate;
          }
          reconcileContextWindowMetrics(turnMetrics);
          onEvent(turnMetrics);
        } else if (auto *done = std::get_if<StreamDone>(&ev)) {
          onEvent(ev);
          turnStopReason = done->reason;
        } else if (auto *err = std::get_if<StreamError>(&ev)) {
          streamErrorStatus = err->httpStatus;
          if (runCancelToken->load()) {
            context.state.currentStatus = AgentStatus::Cancelled;
            return;
          }
          streamError = err->message;
        } else {
          onEvent(ev);
        }
      });
      if (runCancelToken->load()) {
        if (insanityAbortTriggered) {
          // Clear the cancel flag; this is an insanity-triggered abort, not a user cancel.
          runCancelToken->store(false);
        } else {
          persistAssistantTurn(StopReason::Cancelled, false);
          context.state.currentStatus = AgentStatus::Cancelled;
          break;
        }
      }

      // ToolCallChunk events are now emitted immediately above
      // No need to re-emit buffered events

      if (!finalizedToolCalls.empty() || sawTool) {
        turnStopReason = StopReason::ToolUse;
      } else if (sawContent) {
        accumulatedToolChunks.clear();
        finalizedToolCalls.clear();
        if (turnStopReason == StopReason::ToolUse) {
          turnStopReason = StopReason::Stop;
        }
      }

      // Final insanity check (if not already detected during streaming)
      if (!insanityDetectedThisTurn && detector.getTotalTokens() > 0) {
        auto finalCheck = detector.check();
        if (finalCheck.isInsane) {
          insanityDetectedThisTurn = true;
          insanityReason = finalCheck.reason;
        }
      }

      // Handle insanity detection (retry or exhaust)
      if (insanityDetectedThisTurn) {
        if (consecutiveInsanityRetries < context.config.maxInsanityRetries) {
          consecutiveInsanityRetries++;
          // The problematic turn was not yet persisted (we aborted before persistAssistantTurn).
          // Add an intervention nudge turn to guide recovery.
          AgentTurn interventionTurn = makeInternalNudgeTurn(
              "insanity-intervention-",
              buildInsanityInterventionNudge(insanityReason));
          appendTurnToHistory(interventionTurn);
          // Emit retry event
          onEvent(StreamRetrying{
              consecutiveInsanityRetries,
              context.config.maxInsanityRetries,
              0, // httpStatus
              0, // delayMs
              "Insanity detected: " + insanityReason,
              "", ""});
          // Continue to next iteration to retry this turn
          continue;
        } else {
          // Exhausted retries: throw to be caught as fatal error
          throw std::runtime_error(
              "Insanity detection exhausted after " +
              std::to_string(consecutiveInsanityRetries) + " retries: " +
              insanityReason);
        }
      }

      // If there was a stream error and no content came back, retry
      if (!streamError.empty() && fullResponse.empty() &&
          finalizedToolCalls.empty() && accumulatedToolChunks.empty()) {
        // Don't retry if user interrupted
        if (runCancelToken->load()) {
          context.state.currentStatus = AgentStatus::Cancelled;
          return;
        }

        auto rotateAndContinue = [&]() -> bool {
          const auto &config = shared::ConfigLoader::instance().getConfig();
          auto findActiveCategory = [&]() -> std::string {
            auto it_purpose =
                config.purposeRoutes.find(context.config.personaName);
            if (it_purpose != config.purposeRoutes.end() &&
                !it_purpose->second.empty()) {
              return it_purpose->second;
            }
            return config.defaultRouteCategory;
          };

          std::string activeCategoryName = findActiveCategory();
          if (activeCategoryName.empty()) {
            return false;
          }

          auto it_cat = config.modelRouterCategories.find(activeCategoryName);
          if (it_cat == config.modelRouterCategories.end() ||
              it_cat->second.models.size() <= 1) {
            return false;
          }

          const auto &models = it_cat->second.models;
          int currentIndex = -1;
          for (int i = 0; i < static_cast<int>(models.size()); ++i) {
            if (models[i].providerId == provider->getId() &&
                models[i].modelId == opts.modelId) {
              currentIndex = i;
              break;
            }
          }

          if (currentIndex < 0) {
            return false;
          }

          int nextIndex = (currentIndex + 1) % static_cast<int>(models.size());
          // If we looped back to the same model, we've exhausted the category
          if (nextIndex == currentIndex) {
            return false;
          }

          const auto &nextModel = models[nextIndex];
          std::string nextKey = nextModel.providerId + ":" + nextModel.modelId;

          onEvent(StreamRetrying{
              0, 0, streamErrorStatus, 0,
              "Model retries exhausted, rotating to next model in category '" +
                  activeCategoryName + "': " + nextKey,
              "", ""});

          shared::ConfigLoader::instance().setPreferredModelKey(
              activeCategoryName, nextKey);

          // Queue the actual model switch so applyPendingModelSwitchIfAny()
          // picks it up at the top of the next loop iteration.
          {
            std::lock_guard<std::mutex> lock(modelSwitchMutex);
            pendingModelSwitch_ = PendingModelSwitch{
                nextModel.providerId, nextModel.modelId,
                nextModel.variantName.empty()
                    ? std::nullopt
                    : std::optional<std::string>(nextModel.variantName)};
          }

          // Reset error and continue loop to re-resolve provider and stream
          streamError = "";
          consecutiveProviderFailures = 0;
          return true;
        };

        if (!shouldRetryProviderFailureAtAgentLayer(streamErrorStatus)) {
          if (rotateAndContinue()) {
            continue;
          }
          throw std::runtime_error("Provider stream error: " + streamError);
        }
        consecutiveProviderFailures++;
        if (consecutiveProviderFailures > maxProviderRetries) {
          if (rotateAndContinue()) {
            continue;
          }
          throw std::runtime_error("Provider stream error: " + streamError);
        }
        // Emit retry event and wait briefly before retrying
        int retryDelaySec = 1 << (consecutiveProviderFailures - 1); // 1, 2, 4
        onEvent(StreamRetrying{consecutiveProviderFailures, maxProviderRetries,
                               streamErrorStatus, retryDelaySec * 1000,
                               "Provider error, retrying", "", ""});
        // Use interruptible sleep to allow immediate cancellation
        if (!interruptibleSleep(std::chrono::seconds(retryDelaySec),
                                runAbortController, runCancelToken.get())) {
          // Interrupted during retry delay
          context.state.currentStatus = AgentStatus::Cancelled;
          return;
        }
        continue;
      }

      const bool providerDeclaredToolStreamTruncation =
          streamError.find(
              "Provider stream truncated during tool-call generation") !=
              std::string::npos ||
          streamError.find("incomplete tool-call arguments for tool") !=
              std::string::npos;
      if (!streamError.empty() && providerDeclaredToolStreamTruncation) {
        if (retryTruncatedToolStream(streamError, streamErrorStatus)) {
          if (context.state.currentStatus == AgentStatus::Cancelled) {
            return;
          }
          continue;
        }

        // --- Model Rotation for Truncation Failure ---
        auto rotateAndContinue = [&]() -> bool {
          const auto &config = shared::ConfigLoader::instance().getConfig();
          auto findActiveCategory = [&]() -> std::string {
            auto it_purpose =
                config.purposeRoutes.find(context.config.personaName);
            if (it_purpose != config.purposeRoutes.end() &&
                !it_purpose->second.empty()) {
              return it_purpose->second;
            }
            return config.defaultRouteCategory;
          };

          std::string activeCategoryName = findActiveCategory();
          if (activeCategoryName.empty()) {
            return false;
          }

          auto it_cat = config.modelRouterCategories.find(activeCategoryName);
          if (it_cat == config.modelRouterCategories.end() ||
              it_cat->second.models.size() <= 1) {
            return false;
          }

          const auto &models = it_cat->second.models;
          int currentIndex = -1;
          for (int i = 0; i < static_cast<int>(models.size()); ++i) {
            if (models[i].providerId == provider->getId() &&
                models[i].modelId == opts.modelId) {
              currentIndex = i;
              break;
            }
          }

          if (currentIndex < 0) {
            return false;
          }

          int nextIndex = (currentIndex + 1) % static_cast<int>(models.size());
          if (nextIndex == currentIndex) {
            return false;
          }

          const auto &nextModel = models[nextIndex];
          std::string nextKey = nextModel.providerId + ":" + nextModel.modelId;

          onEvent(StreamRetrying{
              0, 0, streamErrorStatus, 0,
              "Tool stream truncation retries exhausted, rotating to next "
              "model in category '" +
                  activeCategoryName + "': " + nextKey,
              "", ""});

          shared::ConfigLoader::instance().setPreferredModelKey(
              activeCategoryName, nextKey);

          // Queue the actual model switch so applyPendingModelSwitchIfAny()
          // picks it up at the top of the next loop iteration.
          {
            std::lock_guard<std::mutex> lock(modelSwitchMutex);
            pendingModelSwitch_ = PendingModelSwitch{
                nextModel.providerId, nextModel.modelId,
                nextModel.variantName.empty()
                    ? std::nullopt
                    : std::optional<std::string>(nextModel.variantName)};
          }

          streamError = "";
          consecutiveProviderFailures = 0;
          return true;
        };

        if (rotateAndContinue()) {
          continue;
        }
        // --- End Model Rotation ---

        throw std::runtime_error("Provider stream error: " + streamError);
      }

      std::vector<ToolCallChunk> unresolvedToolChunks;
      unresolvedToolChunks.reserve(accumulatedToolChunks.size());
      for (const auto &chunk : accumulatedToolChunks) {
        if (!isChunkFinalized(chunk, finalizedToolCalls)) {
          unresolvedToolChunks.push_back(chunk);
        }
      }

      if (finalizedToolCalls.empty() && !accumulatedToolChunks.empty()) {
        const auto malformedToolCalls =
            validateStreamedToolCalls(accumulatedToolChunks);
        if (!malformedToolCalls.empty()) {
          std::ostringstream error;
          error << "Tool call stream ended before a finalized payload was "
                   "emitted: ";
          for (std::size_t i = 0; i < malformedToolCalls.size(); ++i) {
            if (i > 0) {
              error << "; ";
            }
            const auto &failure = malformedToolCalls[i];
            error << "["
                  << (failure.toolCallId.empty() ? "unknown"
                                                 : failure.toolCallId)
                  << "] " << failure.message;
          }
          if (retryTruncatedToolStream(error.str(), streamErrorStatus)) {
            if (context.state.currentStatus == AgentStatus::Cancelled) {
              return;
            }
            continue;
          }
          throw std::runtime_error(error.str());
        }

        finalizedToolCalls = materializeFinalToolCalls(accumulatedToolChunks);
        for (const auto &call : finalizedToolCalls) {
          onEvent(call);
        }
      } else if (!unresolvedToolChunks.empty()) {
        const auto malformedToolCalls =
            validateStreamedToolCalls(unresolvedToolChunks);
        std::ostringstream error;
        error << "Tool call stream ended before a finalized payload was "
                 "emitted: ";
        if (malformedToolCalls.empty()) {
          for (std::size_t i = 0; i < unresolvedToolChunks.size(); ++i) {
            if (i > 0) {
              error << "; ";
            }
            const auto &chunk = unresolvedToolChunks[i];
            error << "["
                  << (chunk.id.empty() ? "unknown" : chunk.id)
                  << "] tool call never finalized";
          }
        } else {
          for (std::size_t i = 0; i < malformedToolCalls.size(); ++i) {
            if (i > 0) {
              error << "; ";
            }
            const auto &failure = malformedToolCalls[i];
            error << "["
                  << (failure.toolCallId.empty() ? "unknown"
                                                 : failure.toolCallId)
                  << "] " << failure.message;
          }
        }
        if (retryTruncatedToolStream(error.str(), streamErrorStatus)) {
          if (context.state.currentStatus == AgentStatus::Cancelled) {
            return;
          }
          continue;
        }
        throw std::runtime_error(error.str());
      }

      // Reset consecutive failure counter on success
      consecutiveProviderFailures = 0;
      consecutiveTruncatedToolRetries = 0;
      consecutiveInsanityRetries = 0;

      if (runCancelToken->load()) {
        persistAssistantTurn(StopReason::Cancelled, false);
        context.state.currentStatus = AgentStatus::Cancelled;
        break;
      }

      // --- Build assistant turn ---
      const bool persistedAssistantTurn =
          persistAssistantTurn(turnStopReason, true);

      if (persistedAssistantTurn && hasQueuedUserTurnPending()) {
        consecutiveEmptyProviderResponses = 0;
        context.state.currentStatus = AgentStatus::ProviderWaiting;
        onEvent(ProviderWaiting{});
        continue;
      }

      // --- Check for termination ---
      if (finalizedToolCalls.empty()) {
        const bool emptyAssistantReply = fullResponse.empty();
        if (emptyAssistantReply) {
          if (blockedStopRecovery.active &&
              (!streamError.empty() || sawThinking || persistedAssistantTurn)) {
            blockedStopRecovery.sawMeaningfulNewTurn = true;
          }
          consecutiveEmptyProviderResponses++;
        } else {
          if (blockedStopRecovery.active) {
            const std::string trimmedResponse =
                shared::StringUtil::trim(fullResponse);
            if (!trimmedResponse.empty() &&
                trimmedResponse !=
                    shared::StringUtil::trim(blockedStopRecovery.finalMessage)) {
              blockedStopRecovery.sawMeaningfulNewTurn = true;
            }
          }
          consecutiveEmptyProviderResponses = 0;
        }

        // Don't error if user interrupted
        if (runCancelToken->load()) {
          context.state.currentStatus = AgentStatus::Cancelled;
          return;
        }

        const bool hasPendingToolCalls =
            !context.state.pendingToolCalls.empty();
        const bool hasBlockingProcesses = !getBlockingProcessIds().empty();
        const bool hasExecutionStatus =
            isExecutionalStatus(context.state.currentStatus);
        const bool hasRunningOwnedBackgroundProcess =
            std::any_of(context.state.ownedProcesses.begin(),
                        context.state.ownedProcesses.end(),
                        [&](const std::string &processId) {
                          if (processId.empty()) {
                            return false;
                          }
                          try {
                            return inspectProcess(processId).running;
                          } catch (...) {
                            Logger::instance().logDebug("Agent: failed to inspect process state");
                            return false;
                          }
                        });

        bool hasRunningDescendantSubagent = false;
        if (!context.identity.id.empty()) {
          for (const auto &agentId : AgentRegistry::instance().listAll()) {
            if (isDescendantAgentRunning(agentId, context.identity.id)) {
              hasRunningDescendantSubagent = true;
              break;
            }
          }
        }

        const bool hasPendingToolLifecycleActivity =
            hasPendingToolCalls || hasExecutionStatus;
        const bool hasHarnessOwnedActiveWork =
            hasPendingToolLifecycleActivity || hasBlockingProcesses ||
            hasRunningOwnedBackgroundProcess || hasRunningDescendantSubagent;
        const TodoStateSnapshot todoState = readTodoState(context);
        if (todoState.hasIncomplete) {
          const std::string fingerprint =
              todoContinuationFingerprint(todoState);
          const bool repeatedTodoSnapshot =
              lastTodoContinuationFingerprint.has_value() &&
              *lastTodoContinuationFingerprint == fingerprint;

          std::string nudgeMessage = buildIncompleteTodoNudge(todoState);
          if (repeatedTodoSnapshot) {
            nudgeMessage = buildIncompleteTodoEscalationNudge(
                todoState.incompleteItems.size());
          }

          appendTurnToHistory(
              makeInternalNudgeTurn("todo-enforcement-", nudgeMessage));
          lastTodoContinuationFingerprint = fingerprint;
          continue; // Start a new iteration with the nudge turn in history
        }

        const bool cleanNoSummary =
            emptyAssistantReply && turnStopReason == StopReason::Stop &&
            streamError.empty() && !hasHarnessOwnedActiveWork;

        if (cleanNoSummary) {
          consecutiveEmptyProviderResponses = 0;

          if (blockedStopRecovery.active &&
              !blockedStopRecovery.sawMeaningfulNewTurn) {
            blockedStopRecovery.emptyReentryCount++;
            if (blockedStopRecovery.emptyReentryCount > 2) {
              throw std::runtime_error(
                  "Workflow blocked stop, but the provider never resumed with a meaningful turn.");
            }
            appendTurnToHistory(makeInternalNudgeTurn(
                "workflow-continuation-",
                buildBlockedWorkflowReentryNudge(
                    blockedStopRecovery.emptyReentryCount),
                Role::User));
            context.state.currentStatus = AgentStatus::ProviderWaiting;
            onEvent(ProviderWaiting{});
            if (!interruptibleSleep(std::chrono::milliseconds(50),
                                    runAbortController,
                                    runCancelToken.get())) {
              context.state.currentStatus = AgentStatus::Cancelled;
              return;
            }
            continue;
          } else if (blockedStopRecovery.active) {
            blockedStopRecovery.emptyReentryCount = 0;
            resetBlockedStopRecovery();
          }

          lastTodoContinuationFingerprint.reset();
          if (fireAgentStopGate(fullResponse, "stop")) {
            context.state.currentStatus = AgentStatus::ProviderWaiting;
            onEvent(ProviderWaiting{});
            blockedStopRecovery.emptyReentryCount++;
            if (blockedStopRecovery.emptyReentryCount > 2) {
              throw std::runtime_error(
                  "Workflow blocked stop, but the provider never resumed with a meaningful turn.");
            }
            appendTurnToHistory(makeInternalNudgeTurn(
                "workflow-continuation-",
                buildBlockedWorkflowReentryNudge(
                    blockedStopRecovery.emptyReentryCount),
                Role::User));
            context.state.currentStatus = AgentStatus::ProviderWaiting;
            onEvent(ProviderWaiting{});
            if (!interruptibleSleep(std::chrono::milliseconds(50),
                                    runAbortController,
                                    runCancelToken.get())) {
              context.state.currentStatus = AgentStatus::Cancelled;
              return;
            }
            continue;
          }
          taskFinished = true;
        } else if (emptyAssistantReply) {
          if (consecutiveEmptyProviderResponses <= maxEmptyProviderRetries) {
            appendTurnToHistory(
                makeInternalNudgeTurn("empty-provider-retry-",
                                      buildEmptyProviderRetryNudge(
                                          consecutiveEmptyProviderResponses)));
            continue;
          }

          throw std::runtime_error(
              "Provider returned an empty response with no tool calls after " +
              std::to_string(consecutiveEmptyProviderResponses) + " attempts.");
        }

        if (hasHarnessOwnedActiveWork) {
          consecutiveEmptyProviderResponses = 0;
          appendTurnToHistory(makeInternalNudgeTurn(
              "active-work-continuation-",
              buildActiveWorkContinuationNudge()));
          if (!interruptibleSleep(std::chrono::milliseconds(250),
                                  runAbortController,
                                  runCancelToken.get())) {
            context.state.currentStatus = AgentStatus::Cancelled;
            return;
          }
          context.state.currentStatus = AgentStatus::ProviderWaiting;
          onEvent(ProviderWaiting{});
          continue;
        }

        lastTodoContinuationFingerprint.reset();
        resetBlockedStopRecovery();

        if (fireAgentStopGate(fullResponse, "stop")) {
          context.state.currentStatus = AgentStatus::ProviderWaiting;
          onEvent(ProviderWaiting{});
          continue;
        }
        taskFinished = true;
      } else {
        consecutiveEmptyProviderResponses = 0;
        // --- State: ExecutingTool ---
        context.state.currentStatus = AgentStatus::ExecutingTool;

        // Track pending tool calls
        for (const auto &call : finalizedToolCalls) {
          context.state.pendingToolCalls.push_back(call.id);
        }

        auto toolStartMs = nowMs();
        if (runCancelToken->load()) {
          persistCancelledToolResults(finalizedToolCalls,
                                      "User aborted tool manually.");
          context.state.pendingToolCalls.clear();
          context.state.currentStatus = AgentStatus::Cancelled;
          break;
        }
        executeTools(finalizedToolCalls, onEvent, runCancelToken);
        auto toolEndMs = nowMs();

        // Update the turn metrics with tool execution time
        // (The turn is already pushed to history, so update the last assistant
        // turn in-place)
        auto &lastTurn =
            context.history
                ->turns[context.history->turns.size() -
                        2]; // assistant turn is 2nd-to-last (tool result turn
                            // was just pushed by executeTools)
        lastTurn.metrics.timing.toolExecutionMs = toolEndMs - toolStartMs;

        // Also update the aggregate (only the tool timing delta)
        context.aggregateMetrics.timing.toolExecutionMs +=
            (toolEndMs - toolStartMs);

        // Clear pending tool calls
        context.state.pendingToolCalls.clear();

        const bool hasPendingToolCalls =
            !context.state.pendingToolCalls.empty();
        const bool hasBlockingProcesses = !getBlockingProcessIds().empty();
        const bool hasExecutionStatus =
            isExecutionalStatus(context.state.currentStatus);
        const bool hasRunningOwnedBackgroundProcess =
            std::any_of(context.state.ownedProcesses.begin(),
                        context.state.ownedProcesses.end(),
                        [&](const std::string &processId) {
                          if (processId.empty()) {
                            return false;
                          }
                          try {
                            return inspectProcess(processId).running;
                          } catch (...) {
                            Logger::instance().logDebug("Agent: failed to inspect process state");
                            return false;
                          }
                        });
        bool hasRunningDescendantSubagent = false;
        if (!context.identity.id.empty()) {
          for (const auto &agentId : AgentRegistry::instance().listAll()) {
            if (isDescendantAgentRunning(agentId, context.identity.id)) {
              hasRunningDescendantSubagent = true;
              break;
            }
          }
        }

        if (hasPendingToolCalls || hasExecutionStatus || hasBlockingProcesses ||
            hasRunningOwnedBackgroundProcess || hasRunningDescendantSubagent) {
          context.state.currentStatus = AgentStatus::ExecutingTool;
          continue;
        }

        if (hasQueuedUserTurnPending()) {
          consecutiveEmptyProviderResponses = 0;
          context.state.currentStatus = AgentStatus::ProviderWaiting;
          onEvent(ProviderWaiting{});
          continue;
        }

        const TodoStateSnapshot todoState = readTodoState(context);
        if (todoState.hasIncomplete) {
          const std::string fingerprint =
              todoContinuationFingerprint(todoState);
          const bool repeatedTodoSnapshot =
              lastTodoContinuationFingerprint.has_value() &&
              *lastTodoContinuationFingerprint == fingerprint;

          std::string nudgeMessage = buildIncompleteTodoNudge(todoState);
          if (repeatedTodoSnapshot) {
            nudgeMessage = buildIncompleteTodoEscalationNudge(
                todoState.incompleteItems.size());
          }

          appendTurnToHistory(
              makeInternalNudgeTurn("todo-enforcement-", nudgeMessage));
          lastTodoContinuationFingerprint = fingerprint;
          context.state.currentStatus = AgentStatus::ExecutingTool;
          continue;
        }
        lastTodoContinuationFingerprint.reset();
      }

    } catch (const std::exception &e) {
      if (runCancelToken->load()) {
        context.state.currentStatus = AgentStatus::Cancelled;
        break;
      }
      const std::string detailedError =
          appendProviderModelContext(context.config, e.what());
      // --- State: Error ---
      context.state.currentStatus = AgentStatus::Error;
      context.state.fatalError = detailedError;

      // Persist error as a system turn in history for journal survival
      AgentTurn errorTurn;
      errorTurn.turnId =
          "error-" + std::to_string(context.history->turns.size());
      Message errorMsg;
      errorMsg.role = Role::Error;
      errorMsg.content.push_back(ErrorContent{
          "Agent Runtime Error",
          "The agent encountered a fatal runtime exception.", detailedError});
      errorMsg.timestamp = nowMs();
      errorTurn.messages.push_back(errorMsg);
      context.history->turns.push_back(errorTurn);
      if (context.config.persistHistory && journaler)
        journaler->appendTurn(errorTurn);

      // Emit error as a StreamError event
      onEvent(StreamError{detailedError, 0, ""});
      break;
    }
  }

  // --- Final state ---
  if (runCancelToken->load()) {
    context.state.currentStatus = AgentStatus::Cancelled;
  } else if (context.state.currentStatus != AgentStatus::Error) {
    context.state.currentStatus = AgentStatus::Idle;
  }

  // Fire AgentStop lifecycle event at agent finalization boundary (end of run).
  if (!agentStopEventFired) {
    hooks::EventPayload payload;
    payload.threadId = context.history ? context.history->threadId : "";
    payload.agentId = context.identity.id;
    payload.persona = context.config.personaName;
    payload.activeMode = context.state.activeMode;
    payload.extra["stop_reason"] = runCancelToken->load() ? "cancelled" : "stop";
    auto fired = hooks::HookDispatcher::fire(WorkflowEventKind::AgentStop, payload);
    if (!fired.injectedReminders.empty()) {
      std::string combined;
      for (std::size_t i = 0; i < fired.injectedReminders.size(); ++i) {
        if (i > 0) combined += "\n";
        combined += fired.injectedReminders[i];
      }
      appendTurnToHistory(makeInternalNudgeTurn("hook-agent-stop-", combined));
    }
  }
}

void Agent::executeTools(
    const std::vector<ToolCall> &calls,
    std::function<void(const StreamEvent &)> onEvent,
    const std::shared_ptr<std::atomic<bool>> &runCancelToken) {
  // Check for insanity loop BEFORE executing tools
  for (const auto &call : calls) {
    // Create signature: "toolName:args"
    std::string signature = call.name + ":" + call.args;

    // Check if this exact call has been repeated consecutively
    int repeatCount = 0;
    for (auto it = context.state.recentToolCallSignatures.rbegin();
         it != context.state.recentToolCallSignatures.rend(); ++it) {
      if (*it == signature) {
        repeatCount++;
      } else {
        break; // Only count consecutive repeats from the end
      }
    }

    if (repeatCount >= context.config.maxIdenticalToolCalls) {
      // Inject intervention nudge
      AgentTurn interventionTurn = makeInternalNudgeTurn(
          "insanity-nudge-",
          buildToolRepetitionNudge(call.name, repeatCount));
      appendTurnToHistory(interventionTurn);

      // Clear the recent signatures to allow recovery
      context.state.recentToolCallSignatures.clear();

      // Broadcast and return early (don't execute the repeated tool)
      onEvent(AgentTurnCompleted{context.identity.id, interventionTurn,
                                 context.aggregateMetrics,
                                 context.identity.parentId});
      return;
    }
  }

  AgentTurn toolResultTurn;
  toolResultTurn.turnId =
      "tools-" + std::to_string(context.history->turns.size());

  struct ToolExecutionResult {
    std::size_t index = 0;
    std::string toolCallId;
    std::string toolName;
    std::string toolArgs;
    std::string resultStr;
    bool success = false;
    std::string resultProcessId;
    std::string resultSubagentId;
    bool isBackground = false;
  };
  struct SharedExecutionState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::optional<ToolExecutionResult>> results;
    std::size_t completed = 0;
  };

  struct RunnableChunk {
    ToolCallChunk chunk;
    std::size_t index = 0;
  };
  std::vector<ToolExecutionResult> immediateResults;
  std::vector<RunnableChunk> runnableChunks;
  runnableChunks.reserve(calls.size());
  for (std::size_t idx = 0; idx < calls.size(); ++idx) {
    const auto &call = calls[idx];
    rapidjson::Document input;
    input.Parse(call.args.c_str());
    if (input.HasParseError()) {
      ToolExecutionResult result;
      result.index = idx;
      result.toolCallId = call.id;
      result.toolName = call.name;
      result.toolArgs = call.args;
      result.resultStr = "Invalid JSON arguments: " + call.args;
      result.success = false;
      immediateResults.push_back(std::move(result));
      continue;
    }
    runnableChunks.push_back(
        RunnableChunk{ToolCallChunk{call.id, call.index, call.name, call.args},
                      idx});
  }

  const auto sharedState = std::make_shared<SharedExecutionState>();
  sharedState->results.resize(runnableChunks.size());
  std::vector<std::thread> workers;
  workers.reserve(runnableChunks.size());

  std::shared_ptr<shared::IAgent> selfKeepAlive;
  if (!context.identity.id.empty()) {
    selfKeepAlive = AgentRegistry::instance().getAgent(context.identity.id);
  }

  for (std::size_t i = 0; i < runnableChunks.size(); ++i) {
    const auto runnable = runnableChunks[i];
    workers.emplace_back([this, sharedState, runnable, i, selfKeepAlive,
                          runCancelToken]() {
      (void)selfKeepAlive;
      ToolExecutionResult execResult;
      execResult.index = runnable.index;
      execResult.toolCallId = runnable.chunk.id;
      execResult.toolName = runnable.chunk.nameDelta;
      execResult.toolArgs = runnable.chunk.argsDelta;
      try {
        rapidjson::Document input;
        input.Parse(runnable.chunk.argsDelta.c_str());
        ToolContext toolCtx{*environment_->getHost(), *this, runnable.chunk.id,
                            runCancelToken.get(),
                            &provider::LLMSearchProviderRegistry::instance()};

        // ── Mode gate ──────────────────────────────────────────────────
        // Consult the active mode's allow/deny scope policy before any
        // tool body runs. Static tools have a known scope via
        // ITool::getMetadata(); dynamic MCP tools do not (their scope is
        // negotiated per-server) so we let those through here and rely
        // on the MCP layer's own gating. ModeSwitch is hard-bypassed
        // inside evaluateModeGate to preserve the escape hatch.
        if (auto staticMeta =
                toolRegistry.getMetadataFor(runnable.chunk.nameDelta)) {
          const auto verdict = evaluateModeGate(
              context.state.activeMode, context.config.personaName,
              staticMeta->scope, runnable.chunk.nameDelta);
          if (!verdict.permitted) {
            execResult.success = false;
            execResult.resultStr = verdict.reason;
            // Skip both the dynamic MCP path and toolRegistry.execute
            // — record-only fail-fast so the agent sees the denial in
            // its tool result and can mode_switch on the next turn.
            std::lock_guard<std::mutex> lock(sharedState->mutex);
            sharedState->results[i] = std::move(execResult);
            sharedState->completed++;
            sharedState->cv.notify_all();
            return;
          }
        }

        // ── Pre-tool hook gate ───────────────────────────────────────────
        hooks::EventPayload hookPayload;
        hookPayload.threadId = context.history ? context.history->threadId : "";
        hookPayload.agentId = context.identity.id;
        hookPayload.persona = context.config.personaName;
        hookPayload.activeMode = context.state.activeMode;
        hookPayload.toolName = runnable.chunk.nameDelta;
        hookPayload.toolArgsJson = runnable.chunk.argsDelta;
        auto preToolHooks =
            hooks::HookDispatcher::fire(WorkflowEventKind::PreToolUse, hookPayload);
        for (const auto &reminder : preToolHooks.injectedReminders) {
          execResult.resultStr += reminder;
        }
        if (preToolHooks.blocked) {
          execResult.success = false;
          execResult.resultStr = preToolHooks.blockReason.empty()
                                     ? "blocked by pre_tool_use hook"
                                     : preToolHooks.blockReason;
          std::lock_guard<std::mutex> lock(sharedState->mutex);
          sharedState->results[i] = std::move(execResult);
          sharedState->completed++;
          sharedState->cv.notify_all();
          return;
        }
        if (!preToolHooks.replacementToolArgs.empty()) {
          execResult.toolArgs = preToolHooks.replacementToolArgs;
          input.Parse(preToolHooks.replacementToolArgs.c_str());
          if (input.HasParseError()) {
            execResult.success = false;
            execResult.resultStr = "Invalid replacementToolArgs JSON from pre_tool_use hook";
            std::lock_guard<std::mutex> lock(sharedState->mutex);
            sharedState->results[i] = std::move(execResult);
            sharedState->completed++;
            sharedState->cv.notify_all();
            return;
          }
        }

        auto dynamicMcpResult = executeDynamicMcpToolCall(
            context, runnable.chunk.nameDelta, input, toolCtx);
        auto result = dynamicMcpResult.has_value()
                          ? *dynamicMcpResult
                          : toolRegistry.execute(runnable.chunk.nameDelta, input,
                                                 toolCtx);
        execResult.success = result.success;
        execResult.resultProcessId = result.processId;
        execResult.resultSubagentId = result.subagentId;
        execResult.isBackground = result.is_background;
        if (result.success) {
          execResult.resultStr = result.data;
        } else {
          const std::string trimmedData = shared::StringUtil::trim(result.data);
          execResult.resultStr = (!trimmedData.empty() && trimmedData != "{}")
                                     ? result.data
                                     : result.error;
        }
      } catch (const std::exception &e) {
        execResult.success = false;
        execResult.resultStr = e.what();
      }
      {
        std::lock_guard<std::mutex> lock(sharedState->mutex);
        sharedState->results[i] = std::move(execResult);
        ++sharedState->completed;
      }
      sharedState->cv.notify_one();
    });
  }

  std::vector<ToolExecutionResult> collectedResults;
  for (auto &result : immediateResults) {
    collectedResults.push_back(std::move(result));
  }
  {
    std::unique_lock<std::mutex> lock(sharedState->mutex);
    while (sharedState->completed < runnableChunks.size() &&
           !runCancelToken->load()) {
      sharedState->cv.wait_for(lock, std::chrono::milliseconds(10));
    }
    for (std::size_t i = 0; i < sharedState->results.size(); ++i) {
      if (sharedState->results[i].has_value()) {
        collectedResults.push_back(std::move(*sharedState->results[i]));
        sharedState->results[i].reset();
      }
    }
  }

  for (std::size_t i = 0; i < workers.size(); ++i) {
    if (!workers[i].joinable()) {
      continue;
    }
    // Always join worker threads to avoid use-after-free
    // Threads that haven't completed will be joined here (may block briefly)
    if (workers[i].joinable()) {
      workers[i].join();
    }
    if (i < sharedState->results.size() &&
        sharedState->results[i].has_value()) {
      collectedResults.push_back(std::move(sharedState->results[i].value()));
      sharedState->results[i].reset();
    }
  }

  std::unordered_set<std::string> collectedToolIds;
  for (const auto &result : collectedResults) {
    if (!result.toolCallId.empty()) {
      collectedToolIds.insert(result.toolCallId);
    }
  }
  if (runCancelToken->load()) {
    for (const auto &call : calls) {
      if (call.id.empty() || collectedToolIds.count(call.id) > 0) {
        continue;
      }
      ToolExecutionResult cancelled;
      const auto it = std::find_if(runnableChunks.begin(), runnableChunks.end(),
                                   [&](const RunnableChunk &runnable) {
                                     return runnable.chunk.id == call.id;
                                   });
      cancelled.index =
          it != runnableChunks.end() ? it->index : collectedResults.size();
      cancelled.toolCallId = call.id;
      cancelled.toolName = call.name;
      cancelled.toolArgs = call.args;
      cancelled.resultStr = "User aborted tool manually.";
      cancelled.success = false;
      collectedResults.push_back(std::move(cancelled));
    }
  }

  std::sort(collectedResults.begin(), collectedResults.end(),
            [](const ToolExecutionResult &a, const ToolExecutionResult &b) {
              return a.index < b.index;
            });

  // Content-hash dedup: collapse identical tool outputs into references.
  uint64_t dedupEventCounter = context.history->turns.size();
  for (const auto &result : collectedResults) {
    // Compute content hash from tool name + result string
    std::string dedupKey = result.toolName + "|" + result.resultStr;
    uint64_t contentHash = std::hash<std::string>{}(dedupKey);

    std::string effectiveResult = result.resultStr;
    if (result.success) {
      auto [it, inserted] = context.state.toolOutputDedup.try_emplace(
          contentHash, "");
      if (!inserted) {
        // Already seen this exact output — use reference
        effectiveResult = it->second;
      } else {
        // First occurrence — store reference string for future duplicates
        it->second = "↪ identical to event #" +
                     std::to_string(dedupEventCounter);
      }
    }

    Message msg;
    msg.role = Role::ToolResult;
    msg.content.push_back(
        ToolResultContent{result.toolCallId, effectiveResult, result.success,
                          result.resultProcessId, result.resultSubagentId});
    ++dedupEventCounter;
    auto now = std::chrono::system_clock::now();
    msg.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
    toolResultTurn.messages.push_back(msg);

    if (result.success) {
      runtime_overlay::reconcileSuccessfulToolResult(
          context, *environment_->getHost(), environment_->getWorkspace(),
          result.toolName, result.toolArgs, result.resultStr);

      const bool owns_background_process =
          !result.resultProcessId.empty() &&
          (result.isBackground || result.toolName == "process_spawn");

      if (result.toolName == "Edit" || result.toolName == "EditWrite" ||
          result.toolName == "EditReplace" || result.toolName == "EditRange" ||
          result.toolName == "file_edit" || result.toolName == "file_write") {
        persistEditLedgerBatch(
            context,
            EditLedgerToolExecutionResultView{result.toolCallId, result.toolName,
                                              result.resultStr});
      }

      if (owns_background_process &&
          std::find(context.state.ownedProcesses.begin(),
                    context.state.ownedProcesses.end(),
                    result.resultProcessId) ==
              context.state.ownedProcesses.end()) {
        context.state.ownedProcesses.push_back(result.resultProcessId);
      }

      if (result.toolName == "Edit" || result.toolName == "EditWrite" ||
          result.toolName == "EditReplace" || result.toolName == "EditRange" ||
          result.toolName == "file_edit" || result.toolName == "file_write") {
        for (const auto &file :
             extractFileEditEventPayloads(result.toolArgs, result.resultStr)) {
          if (file.path.empty()) {
            continue;
          }
          if (std::find(context.state.editedFiles.begin(),
                        context.state.editedFiles.end(),
                        file.path) == context.state.editedFiles.end()) {
            context.state.editedFiles.push_back(file.path);
          }
          std::string actionDesc = "Edited file: " + file.path;
          context.state.completedActions.push_back(actionDesc);
          onEvent(AgentFileEdited{context.identity.id,
                                  context.identity.parentId, file.path,
                                  result.toolCallId, file.diffPreview,
                                  file.addedLines, file.removedLines});
        }
      }
    } else if (result.toolName == "Edit" || result.toolName == "EditWrite" ||
               result.toolName == "EditReplace" || result.toolName == "EditRange" ||
               result.toolName == "file_edit") {
      rapidjson::Document resultDoc;
      resultDoc.Parse(result.resultStr.c_str());
      if (!resultDoc.HasParseError() && resultDoc.IsObject() &&
          resultDoc.HasMember("stale_anchor") &&
          resultDoc["stale_anchor"].IsBool() &&
          resultDoc["stale_anchor"].GetBool() && resultDoc.HasMember("path") &&
          resultDoc["path"].IsString()) {
        runtime_overlay::refreshFileWatch(context, *environment_->getHost(),
                                          environment_->getWorkspace(),
                                          resultDoc["path"].GetString());
      }
    }

    std::string signature = result.toolName + ":" + result.toolArgs;
    if (std::find(context.state.recentToolCallSignatures.begin(),
                  context.state.recentToolCallSignatures.end(),
                  signature) != context.state.recentToolCallSignatures.end()) {
      context.aggregateMetrics.memory.redundantToolSignatureCount += 1;
    }
    context.state.recentToolCallSignatures.push_back(signature);
  }

  // Keep only last 20 signatures to prevent unbounded growth
  if (context.state.recentToolCallSignatures.size() > 20) {
    context.state.recentToolCallSignatures.erase(
        context.state.recentToolCallSignatures.begin(),
        context.state.recentToolCallSignatures.begin() +
            (context.state.recentToolCallSignatures.size() - 20));
  }

  if (!toolResultTurn.messages.empty()) {
    context.history->turns.push_back(toolResultTurn);
    if (context.config.persistHistory && journaler)
      journaler->appendTurn(toolResultTurn);
  }

  // ── Fire post_tool_use hooks ────────────────────────────────────────────
  // Each tool result produces an EventPayload; matching hooks get to inject
  // a system reminder before the next provider request. Hook reminders are
  // already wrapped in <FIRMIUS_HOOK> tags by the dispatcher (the model
  // recognises these via prompts/base.md, same channel as system signals).
  // Only the post_tool_use event fires here; pre_tool_use needs blocking
  // semantics and lands with the Day-3 dispatcher upgrade.
  std::vector<std::string> hookReminders;
  for (const auto &result : collectedResults) {
    hooks::EventPayload payload;
    payload.threadId = context.history ? context.history->threadId : "";
    payload.agentId = context.identity.id;
    payload.persona = context.config.personaName;
    payload.toolName = result.toolName;
    payload.toolArgsJson = result.toolArgs;
    payload.toolResultJson = result.resultStr;
    payload.toolSuccess = result.success;
    auto fired = hooks::HookDispatcher::fire(
        WorkflowEventKind::PostToolUse, payload);
    for (auto &reminder : fired.injectedReminders) {
      hookReminders.push_back(std::move(reminder));
    }
  }
  if (!hookReminders.empty()) {
    std::string combined;
    for (std::size_t i = 0; i < hookReminders.size(); ++i) {
      if (i > 0) {
        combined += "\n";
      }
      combined += hookReminders[i];
    }
    appendTurnToHistory(makeInternalNudgeTurn("hook-post-tool-", combined));
  }

  // Broadcast turn completion
  onEvent(AgentTurnCompleted{context.identity.id, toolResultTurn,
                             context.aggregateMetrics,
                             context.identity.parentId});
}

void Agent::saveHistory() {
  if (context.config.persistHistory && journaler) {
    journaler->rewriteJournal(context.history->turns);
  }
}

void Agent::flushJournal() {
  if (journaler) {
    journaler->flush();
  }
}

void Agent::appendHistoryTurn(const AgentTurn &turn) { appendTurnToHistory(turn); }

void Agent::compactContext(
    std::function<void(const shared::StreamEvent &)> onEvent) {
  context.state.currentStatus = AgentStatus::Compacting;
  onEvent(AgentCompacting{context.identity.id, context.identity.parentId});
  const std::string compactionId = std::to_string(nowMs());

  if (interrupted.load()) {
    context.state.currentStatus = AgentStatus::Cancelled;
    return;
  }

  const uint32_t oldTokens = context.aggregateMetrics.tokens.contextSize;

  // Save snapshot for undo support
  saveCompactionSnapshot(context.history->threadId, context.identity.id,
                         compactionId, oldTokens, context.history->turns);

  if (interrupted.load()) {
    context.state.currentStatus = AgentStatus::Cancelled;
    return;
  }

  const uint32_t compactedContextSize =
      context.aggregateMetrics.tokens.contextSize;
  const uint32_t tokensSaved =
      (oldTokens > compactedContextSize) ? oldTokens - compactedContextSize : 0;

  onEvent(ContextCompacted{context.identity.id, tokensSaved,
                           context.identity.parentId});

  context.state.currentStatus = AgentStatus::Idle;
}

} // namespace firmius::core

