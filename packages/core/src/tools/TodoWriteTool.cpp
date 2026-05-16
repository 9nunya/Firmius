#include "tools/TodoWriteTool.hpp"
#include "persistence/ThreadManager.hpp"
#include "IAgent.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <chrono>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace firmius::core {

namespace {

std::string requireCurrentThreadId(shared::ToolContext &ctx) {
  const auto &context = ctx.agent.getContext();
  if (!context.history || context.history->threadId.empty()) {
    throw std::runtime_error("No current thread exists");
  }
  return context.history->threadId;
}

struct TodoItemState {
  int id = 0;
  char marker = ' ';
  std::string text;
};

uint64_t nowEpochMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

char statusMarker(shared::TodoStatus status) {
  switch (status) {
  case shared::TodoStatus::Pending:
    return ' ';
  case shared::TodoStatus::InProgress:
    return '*';
  case shared::TodoStatus::Done:
    return 'x';
  }
  return ' ';
}

std::string statusName(shared::TodoStatus status) {
  switch (status) {
  case shared::TodoStatus::Pending:
    return "pending";
  case shared::TodoStatus::InProgress:
    return "in_progress";
  case shared::TodoStatus::Done:
    return "done";
  }
  return "pending";
}

shared::TodoStatus markerToStatus(char marker) {
  switch (marker) {
  case ' ':
    return shared::TodoStatus::Pending;
  case '*':
    return shared::TodoStatus::InProgress;
  case 'x':
    return shared::TodoStatus::Done;
  default:
    throw std::runtime_error("Invalid todo status marker");
  }
}

std::vector<TodoItemState> parseTodoList(const std::string &state) {
  if (shared::StringUtil::trim(state).empty()) {
    throw std::runtime_error(
        "Todo state must not be empty. Use numbered lines like "
        "'1. [ ] First task'.");
  }

  static const std::regex linePattern(
      R"(^([0-9]+)\.\s+\[([ *x])\]\s+(.+)$)");

  std::vector<TodoItemState> items;
  std::set<int> seenIds;
  std::istringstream stream(state);
  std::string rawLine;
  int lineNo = 0;
  while (std::getline(stream, rawLine)) {
    lineNo++;
    if (rawLine.empty()) {
      throw std::runtime_error("Malformed todo line " + std::to_string(lineNo) +
                               ": line must not be empty");
    }
    std::smatch match;
    if (!std::regex_match(rawLine, match, linePattern)) {
      throw std::runtime_error("Malformed todo line " + std::to_string(lineNo) + 
                               ": expected '<id>. [status] text' (example: "
                               "'1. [ ] First task')");
    }
    const int id = std::stoi(match[1].str());
    const char marker = match[2].str()[0];
    const std::string text = shared::StringUtil::trim(match[3].str());
    if (seenIds.count(id) > 0) {
      throw std::runtime_error("Duplicate todo id in state: " +
                               std::to_string(id));
    }
    seenIds.insert(id);
    if (text.empty()) {
      throw std::runtime_error("Todo text must not be empty for id " +
                               std::to_string(id));
    }
    items.push_back(TodoItemState{id, marker, text});
  }
  return items;
}

} // namespace

shared::ToolMetadata TodoWriteTool::getMetadata() const {
  return {"Todo",
          "Update the calling agent's execution state by providing the full todo list.",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> TodoWriteTool::getSchema() const {
  return zObject({{"patch",
                   zString()->describe(
                       "Full todo list state in numbered text format.\n\n"
                       "USAGE:\n"
                       "- Pass the ENTIRE desired todo list, not a partial delta.\n"
                       "- Each item should preserve numbering/id semantics expected by the parser.\n"
                       "- Use this tool to rewrite current todo state atomically after progress, decomposition, or completion changes.\n\n"
                       "IMPORTANT:\n"
                       "- This is the source of truth for the runtime todo contract.\n"
                       "- Omitting an existing item removes it from the todo list.")}})
      ->required({"patch"});
}

shared::ToolResult TodoWriteTool::execute(const rapidjson::Value &input,
                                          shared::ToolContext &ctx) {
  try {
    if (!input.HasMember("patch") || !input["patch"].IsString()) {
      throw std::runtime_error("Todo requires string field 'patch' containing the full state");
    }

    const auto &agentContext = ctx.agent.getContext();
    const std::string threadId = requireCurrentThreadId(ctx);
    const std::string agentId = agentContext.identity.id;
    if (agentId.empty()) {
      throw std::runtime_error("Cannot mutate todo list without agent id");
    }

    const auto newState = parseTodoList(input["patch"].GetString());
    ThreadManager tm(ThreadManager::defaultBasePath());
    const auto oldTodoList = tm.getAgentTodo(threadId, agentId);

    std::unordered_map<int, const shared::TodoItem *> oldItemsById;
    for (const auto &item : oldTodoList.items) {
      oldItemsById[item.id] = &item;
    }

    shared::AgentTodoList newList;
    newList.threadId = threadId;
    newList.agentId = agentId;
    
    const uint64_t now = nowEpochMs();
    int maxId = 0;

    for (const auto &stateItem : newState) {
      shared::TodoItem item;
      item.id = stateItem.id;
      item.text = stateItem.text;
      item.status = markerToStatus(stateItem.marker);
      
      auto it = oldItemsById.find(stateItem.id);
      if (it != oldItemsById.end()) {
        const auto *old = it->second;
        item.createdAt = old->createdAt;
        item.updatedAt = (old->text != item.text || old->status != item.status) ? now : old->updatedAt;
        item.chunkId = old->chunkId;
        item.planId = old->planId;
      } else {
        item.createdAt = now;
        item.updatedAt = now;
      }

      newList.items.push_back(std::move(item));
      if (stateItem.id > maxId) {
        maxId = stateItem.id;
      }
    }

    newList.nextId = std::max(maxId + 1, 1);
    tm.writeAgentTodo(threadId, agentId, newList);

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("thread_id", rapidjson::Value(threadId.c_str(), alloc), alloc);
    doc.AddMember("agent_id", rapidjson::Value(agentId.c_str(), alloc), alloc);
    doc.AddMember("next_id", newList.nextId, alloc);

    rapidjson::Value items(rapidjson::kArrayType);
    std::ostringstream summary;
    for (const auto &item : newList.items) {
      rapidjson::Value row(rapidjson::kObjectType);
      row.AddMember("id", item.id, alloc);
      row.AddMember("text", rapidjson::Value(item.text.c_str(), alloc), alloc);
      const std::string status = statusName(item.status);
      row.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
      row.AddMember("chunk_id", rapidjson::Value(item.chunkId.c_str(), alloc),
                    alloc);
      row.AddMember("plan_id", rapidjson::Value(item.planId.c_str(), alloc), alloc);
      row.AddMember("created_at", item.createdAt, alloc);
      row.AddMember("updated_at", item.updatedAt, alloc);
      items.PushBack(row, alloc);

      summary << item.id << ". [" << statusMarker(item.status) << "] "
              << item.text << "\n";
    }
    doc.AddMember("items", items, alloc);
    doc.AddMember("summary", rapidjson::Value(summary.str().c_str(), alloc),
                  alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
