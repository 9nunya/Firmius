#include "tools/MemoryRecallTool.hpp"

#include "IAgent.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"

#include <cstdlib>
#include <sstream>

namespace firmius::core {

namespace {

std::string threadStorageRootPath() {
  if (const char *home = std::getenv("HOME")) {
    return std::string(home) + "/.firmius/threads";
  }
  return ".firmius/threads";
}

std::string renderTurn(const shared::AgentTurn &turn, bool includeSystem) {
  std::ostringstream out;
  out << "turn_id=" << turn.turnId << "\n";
  for (const auto &msg : turn.messages) {
    if (!includeSystem && msg.role == shared::Role::System) {
      continue;
    }
    out << "- role=";
    switch (msg.role) {
    case shared::Role::System:
      out << "system";
      break;
    case shared::Role::User:
      out << "user";
      break;
    case shared::Role::Assistant:
      out << "assistant";
      break;
    case shared::Role::ToolResult:
      out << "tool_result";
      break;
    case shared::Role::Error:
      out << "error";
      break;
    }
    out << " ";
    bool firstPart = true;
    for (const auto &part : msg.content) {
      if (!firstPart) {
        out << " ";
      }
      firstPart = false;
      if (const auto *txt = std::get_if<shared::TextContent>(&part)) {
        out << txt->text;
      } else if (const auto *thinking =
                     std::get_if<shared::ThinkingContent>(&part)) {
        out << "<thinking>" << thinking->thinking << "</thinking>";
      } else if (const auto *toolCall =
                     std::get_if<shared::ToolCallContent>(&part)) {
        out << "<tool_call name=\"" << toolCall->name << "\">"
            << toolCall->args << "</tool_call>";
      } else if (const auto *toolResult =
                     std::get_if<shared::ToolResultContent>(&part)) {
        out << "<tool_result success=\"" << (toolResult->success ? "true" : "false")
            << "\">" << toolResult->result << "</tool_result>";
      } else if (const auto *notice =
                     std::get_if<shared::NoticeContent>(&part)) {
        out << notice->title << ": " << notice->message;
      } else if (const auto *error = std::get_if<shared::ErrorContent>(&part)) {
        out << error->errorName << ": " << error->description;
      }
    }
    out << "\n";
  }
  return out.str();
}

} // namespace

shared::ToolMetadata MemoryRecallTool::getMetadata() const {
  return {"memory_recall",
          "Recall exact preserved thread turns by turn id or simple paging.",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> MemoryRecallTool::getSchema() const {
  return zObject({
      {"start_turn_id", zString()->setOptional()->describe(
                            "Inclusive start turn id for an exact range.")},
      {"end_turn_id", zString()->setOptional()->describe(
                          "Inclusive end turn id for an exact range.")},
      {"cursor_turn_id", zString()->setOptional()->describe(
                             "Turn id to page around when recalling history.")},
      {"page", zNumber()->setOptional()->describe(
                   "Relative page offset when using cursor_turn_id.")},
      {"page_size", zNumber()->setOptional()->describe(
                        "Number of turns to return (default 8).")},
      {"include_system", zBoolean()->setOptional()->describe(
                             "Include persisted system turns in the output.")},
  });
}

shared::ToolResult MemoryRecallTool::execute(const MemoryRecallInput &input,
                                             shared::ToolContext &ctx) {
  auto &agentCtx = ctx.agent.getContext();
  if (!agentCtx.history || agentCtx.history->threadId.empty()) {
    return shared::ToolResult::fail("memory_recall requires an active thread");
  }

  const std::string threadId = agentCtx.history->threadId;
  const std::string agentId = agentCtx.identity.id;
  const bool includeSystem = input.include_system.value_or(false);
  const int pageSize = std::clamp(input.page_size.value_or(8), 1, 64);

  ThreadManager tm(threadStorageRootPath());
  const auto history = tm.loadAgentHistory(threadId, agentId);
  if (history.turns.empty()) {
    return shared::ToolResult::fail("No history is available for memory_recall");
  }

  int startIndex = 0;
  int endIndex = static_cast<int>(history.turns.size()) - 1;

  auto findTurnIndex = [&](const std::string &turnId) -> int {
    for (std::size_t i = 0; i < history.turns.size(); ++i) {
      if (history.turns[i].turnId == turnId) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  if (input.start_turn_id.has_value() || input.end_turn_id.has_value()) {
    if (input.start_turn_id.has_value()) {
      startIndex = findTurnIndex(*input.start_turn_id);
    }
    if (input.end_turn_id.has_value()) {
      endIndex = findTurnIndex(*input.end_turn_id);
    }
    if (startIndex < 0 || endIndex < 0 || startIndex > endIndex) {
      return shared::ToolResult::fail(
          "Invalid turn range requested for memory_recall");
    }
  } else if (input.cursor_turn_id.has_value()) {
    const int cursorIndex = findTurnIndex(*input.cursor_turn_id);
    if (cursorIndex < 0) {
      return shared::ToolResult::fail("cursor_turn_id was not found");
    }
    const int page = input.page.value_or(0);
    startIndex = std::clamp(cursorIndex + (page * pageSize), 0,
                            std::max(0, static_cast<int>(history.turns.size()) - 1));
    endIndex = std::min(startIndex + pageSize - 1,
                        static_cast<int>(history.turns.size()) - 1);
  } else {
    startIndex = std::max(0, static_cast<int>(history.turns.size()) - pageSize);
    endIndex = static_cast<int>(history.turns.size()) - 1;
  }

  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();
  rapidjson::Value turns(rapidjson::kArrayType);
  for (int i = startIndex; i <= endIndex; ++i) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("turnId",
                   rapidjson::Value(history.turns[i].turnId.c_str(), a), a);
    const std::string rendered = renderTurn(history.turns[i], includeSystem);
    item.AddMember("content", rapidjson::Value(rendered.c_str(), a), a);
    turns.PushBack(item, a);
  }
  doc.AddMember("threadId", rapidjson::Value(threadId.c_str(), a), a);
  doc.AddMember("agentId", rapidjson::Value(agentId.c_str(), a), a);
  doc.AddMember("startIndex", startIndex, a);
  doc.AddMember("endIndex", endIndex, a);
  doc.AddMember("turns", turns, a);
  return shared::ToolResult::ok(doc);
}

} // namespace firmius::core
