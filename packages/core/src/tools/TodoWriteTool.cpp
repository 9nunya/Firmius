#include "tools/TodoWriteTool.hpp"
#include "IAgent.hpp"
#include "tools/WorkToolCommon.hpp"
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

struct TodoMutation {
  int id = 0;
  char marker = ' ';
  std::string text;
  int line = 0;
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

std::vector<TodoMutation> parseMutations(const std::string &patch) {
  if (shared::StringUtil::trim(patch).empty()) {
    throw std::runtime_error(
        "Todo patch must not be empty. Use numbered lines like "
        "'1. [ ] First task'.");
  }

  static const std::regex linePattern(
      R"(^([0-9]+)\.\s+\[([ *x\-\+])\]\s+(.+)$)");

  std::vector<TodoMutation> mutations;
  std::set<int> seenIds;
  std::istringstream stream(patch);
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
      throw std::runtime_error("Duplicate todo id in patch: " +
                               std::to_string(id));
    }
    seenIds.insert(id);
    if (text.empty()) {
      throw std::runtime_error("Todo text must not be empty for id " +
                               std::to_string(id));
    }
    mutations.push_back(TodoMutation{id, marker, text, lineNo});
  }
  return mutations;
}

std::string existingIdsSummary(const std::vector<shared::TodoItem> &items) {
  if (items.empty()) {
    return "(none)";
  }

  std::vector<int> ids;
  ids.reserve(items.size());
  for (const auto &item : items) {
    ids.push_back(item.id);
  }
  std::sort(ids.begin(), ids.end());

  std::ostringstream out;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << ids[i];
  }
  return out.str();
}

} // namespace

shared::ToolMetadata TodoWriteTool::getMetadata() const {
  return {"todo_write",
          "Patch the calling agent's todo list using strict numbered syntax",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> TodoWriteTool::getSchema() const {
  return zObject({{"patch", zString()}})->required({"patch"});
}

shared::ToolResult TodoWriteTool::execute(const rapidjson::Value &input,
                                          shared::ToolContext &ctx) {
  try {
    if (!input.HasMember("patch") || !input["patch"].IsString()) {
      throw std::runtime_error("todo_write requires string field 'patch'");
    }

    const auto &agentContext = ctx.agent.getContext();
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    const std::string agentId = agentContext.identity.id;
    if (agentId.empty()) {
      throw std::runtime_error("Cannot mutate todo list without agent id");
    }

    const auto mutations = parseMutations(input["patch"].GetString());
    auto tm = worktools::makeThreadManager();
    auto todoList = tm.getAgentTodo(threadId, agentId);

    std::unordered_map<int, size_t> indexById;
    indexById.reserve(todoList.items.size());
    for (size_t i = 0; i < todoList.items.size(); ++i) {
      indexById[todoList.items[i].id] = i;
    }

    int expectedNextId = std::max(todoList.nextId, 1);
    const uint64_t now = nowEpochMs();
    if (todoList.items.empty()) {
      bool hasMarkerAdd = false;
      bool hasMarkerDelete = false;
      for (const auto &mutation : mutations) {
        hasMarkerAdd = hasMarkerAdd || mutation.marker == '+';
        hasMarkerDelete = hasMarkerDelete || mutation.marker == '-';
      }

      if (hasMarkerDelete) {
        throw std::runtime_error(
            "Todo list is empty. Create items with '1. [ ] First task' and "
            "sequential ids.");
      }

      if (!hasMarkerAdd) {
        int expectedId = 1;
        for (const auto &mutation : mutations) {
          if (mutation.id != expectedId) {
            throw std::runtime_error(
                "Todo list is empty. Create items with '1. [ ] First task' "
                "and sequential ids.");
          }
          shared::TodoItem item;
          item.id = mutation.id;
          item.text = mutation.text;
          item.status = markerToStatus(mutation.marker);
          item.createdAt = now;
          item.updatedAt = now;
          todoList.items.push_back(std::move(item));
          indexById[mutation.id] = todoList.items.size() - 1;
          expectedId++;
        }
        expectedNextId = expectedId;
      }
    }

    for (const auto &mutation : mutations) {
      if (mutation.marker == '+') {
        if (mutation.id != expectedNextId) {
          throw std::runtime_error(
              "Add id " + std::to_string(mutation.id) +
              " is invalid: expected next id " + std::to_string(expectedNextId));
        }
        shared::TodoItem item;
        item.id = mutation.id;
        item.text = mutation.text;
        item.status = shared::TodoStatus::Pending;
        item.createdAt = now;
        item.updatedAt = now;
        todoList.items.push_back(std::move(item));
        indexById[mutation.id] = todoList.items.size() - 1;
        expectedNextId++;
        continue;
      }

      auto it = indexById.find(mutation.id);
      if (it == indexById.end()) {
        const bool inferredAdd = mutation.marker == ' ' &&
                                 mutation.id == expectedNextId;
        if (inferredAdd) {
          shared::TodoItem item;
          item.id = mutation.id;
          item.text = mutation.text;
          item.status = markerToStatus(mutation.marker);
          item.createdAt = now;
          item.updatedAt = now;
          todoList.items.push_back(std::move(item));
          indexById[mutation.id] = todoList.items.size() - 1;
          expectedNextId++;
          continue;
        }
        if (todoList.items.empty()) {
          throw std::runtime_error(
              "Todo list is empty. Create items with '1. [ ] First task' and "
              "sequential ids.");
        }
        throw std::runtime_error(
            "Unknown todo id " + std::to_string(mutation.id) +
            ". Existing ids: " + existingIdsSummary(todoList.items) +
            ". To add a new item, use '" + std::to_string(expectedNextId) +
            ". [+] <task>'.");
      }

      if (mutation.marker == '-') {
        const size_t removeIndex = it->second;
        todoList.items.erase(todoList.items.begin() +
                             static_cast<std::ptrdiff_t>(removeIndex));
        indexById.clear();
        for (size_t i = 0; i < todoList.items.size(); ++i) {
          indexById[todoList.items[i].id] = i;
        }
        continue;
      }

      auto &item = todoList.items[it->second];
      item.status = markerToStatus(mutation.marker);
      item.text = mutation.text;
      item.updatedAt = now;
    }

    todoList.nextId = expectedNextId;
    tm.writeAgentTodo(threadId, agentId, todoList);

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("thread_id", rapidjson::Value(threadId.c_str(), alloc), alloc);
    doc.AddMember("agent_id", rapidjson::Value(agentId.c_str(), alloc), alloc);
    doc.AddMember("next_id", todoList.nextId, alloc);

    rapidjson::Value items(rapidjson::kArrayType);
    std::ostringstream summary;
    for (const auto &item : todoList.items) {
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
