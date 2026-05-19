#include "tools/TodoWriteTool.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include "IAgent.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace firmius::core {

using namespace firmius::shared;

namespace {

// ---------------------------------------------------------------------------
// Action-based TodoWriteTool
//
// Old shape required the model to send the entire todo list as a numbered
// text blob every call ("1. [ ] task\n2. [x] task\n"). Models routinely
// dropped lines, mis-numbered, or simply forgot to call the tool after
// finishing work because rewriting the whole thing felt expensive.
//
// New shape: pick an action — set / add / update / complete / remove /
// clear / list — and supply only the fields you need. Inputs are lenient
// (singular `id` or plural `ids`, a string `items` shorthand for a single
// add). Every call reads the current state from disk, applies the requested
// mutation, persists the result, and returns the canonical numbered listing
// so the agent never has to guess what the list looks like now.
// ---------------------------------------------------------------------------

std::string requireCurrentThreadId(shared::ToolContext &ctx) {
  const auto &context = ctx.agent.getContext();
  if (!context.history || context.history->threadId.empty()) {
    throw std::runtime_error("No current thread exists");
  }
  return context.history->threadId;
}

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

shared::TodoStatus parseStatusName(const std::string &raw) {
  std::string s;
  s.reserve(raw.size());
  for (char c : raw) {
    if (c == '-' || c == '_' || c == ' ')
      continue;
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (s == "pending" || s == "todo" || s == "open" || s == "p" || s == " ")
    return shared::TodoStatus::Pending;
  if (s == "inprogress" || s == "active" || s == "doing" || s == "wip" ||
      s == "*")
    return shared::TodoStatus::InProgress;
  if (s == "done" || s == "complete" || s == "completed" || s == "finished" ||
      s == "x")
    return shared::TodoStatus::Done;
  throw std::runtime_error(
      "Unknown todo status '" + raw +
      "'. Use one of: pending, in_progress, done.");
}

bool tryParseJsonDocFromString(const std::string &raw,
                               rapidjson::Document &out);

// Accept `id` (int or string), `ids` (array or single int/string), and
// return the de-duplicated list of integer ids in input order. Empty
// input is allowed; the action handler decides whether that's an error.
std::vector<int> readIds(const rapidjson::Value &input) {
  std::vector<int> ids;
  std::unordered_set<int> seen;

  auto pushInt = [&](int parsed) {
    if (seen.insert(parsed).second) ids.push_back(parsed);
  };

  auto pushParsed = [&](const rapidjson::Value &v, const char *whichKey) {
    int parsed = 0;
    if (v.IsInt()) {
      parsed = v.GetInt();
    } else if (v.IsUint()) {
      parsed = static_cast<int>(v.GetUint());
    } else if (v.IsInt64()) {
      parsed = static_cast<int>(v.GetInt64());
    } else if (v.IsString()) {
      const std::string raw = shared::StringUtil::trim(std::string(v.GetString()));
      if (raw.empty())
        return;
      try {
        parsed = std::stoi(raw);
      } catch (const std::exception &) {
        throw std::runtime_error(std::string("Invalid id '") + v.GetString() +
                                 "' in '" + whichKey +
                                 "'. Expected an integer.");
      }
    } else {
      throw std::runtime_error(std::string("Field '") + whichKey +
                               "' must be an integer or list of integers.");
    }
    pushInt(parsed);
  };

  if (input.HasMember("ids")) {
    const auto &v = input["ids"];
    if (v.IsArray()) {
      for (const auto &el : v.GetArray()) {
        pushParsed(el, "ids");
      }
    } else if (v.IsString()) {
      // Some callers send JSON-encoded arrays: "ids":"[1,2,3]".
      rapidjson::Document d;
      if (tryParseJsonDocFromString(v.GetString(), d)) {
        if (d.IsArray()) {
          for (const auto &el : d.GetArray()) {
            pushParsed(el, "ids");
          }
        } else {
          pushParsed(d, "ids");
        }
      } else {
        // Also accept simple "1,2,3" strings.
        std::string raw = shared::StringUtil::trim(std::string(v.GetString()));
        if (raw.find(',') != std::string::npos) {
          std::stringstream ss(raw);
          std::string token;
          while (std::getline(ss, token, ',')) {
            const std::string t = shared::StringUtil::trim(token);
            if (t.empty()) continue;
            try {
              pushInt(std::stoi(t));
            } catch (const std::exception &) {
              throw std::runtime_error(
                  std::string("Invalid id '") + t +
                  "' in 'ids'. Expected integers separated by commas.");
            }
          }
        } else {
          pushParsed(v, "ids");
        }
      }
    } else if (!v.IsNull()) {
      pushParsed(v, "ids");
    }
  }
  if (input.HasMember("id")) {
    pushParsed(input["id"], "id");
  }
  return ids;
}

struct ItemSpec {
  std::string text;
  // unset => caller didn't specify; defaults differ per action
  bool hasStatus = false;
  shared::TodoStatus status = shared::TodoStatus::Pending;
  // optional explicit id — only honored by `set`
  bool hasId = false;
  int id = 0;
};

ItemSpec readItemSpec(const rapidjson::Value &v, const char *path) {
  ItemSpec out;
  if (v.IsString()) {
    out.text = shared::StringUtil::trim(std::string(v.GetString()));
    return out;
  }
  if (!v.IsObject()) {
    throw std::runtime_error(std::string("Item at '") + path +
                             "' must be a string or {text, status?} object.");
  }
  if (v.HasMember("text") && v["text"].IsString()) {
    out.text = shared::StringUtil::trim(std::string(v["text"].GetString()));
  } else if (v.HasMember("task") && v["task"].IsString()) {
    out.text = shared::StringUtil::trim(std::string(v["task"].GetString()));
  } else if (v.HasMember("title") && v["title"].IsString()) {
    out.text = shared::StringUtil::trim(std::string(v["title"].GetString()));
  }
  if (v.HasMember("status") && v["status"].IsString()) {
    out.hasStatus = true;
    out.status = parseStatusName(v["status"].GetString());
  }
  if (v.HasMember("id")) {
    const auto &idv = v["id"];
    if (idv.IsInt()) {
      out.hasId = true;
      out.id = idv.GetInt();
    } else if (idv.IsString()) {
      try {
        out.hasId = true;
        out.id = std::stoi(idv.GetString());
      } catch (const std::exception &) {
        out.hasId = false;
      }
    }
  }
  return out;
}

bool tryParseJsonDocFromString(const std::string &raw,
                               rapidjson::Document &out) {
  const std::string trimmed = shared::StringUtil::trim(raw);
  if (trimmed.empty()) return false;
  const char first = trimmed.front();
  if (first != '[' && first != '{') return false;
  out.Parse(trimmed.c_str());
  return !out.HasParseError();
}

const rapidjson::Value *unwrapEmbeddedItemsContainer(const rapidjson::Value &v) {
  if (v.IsObject() && v.HasMember("items")) {
    return &v["items"];
  }
  return &v;
}

// Accept `items` as: array of strings, array of objects, single string,
// or single object. Returns the parsed specs in order.
std::vector<ItemSpec> readItems(const rapidjson::Value &input) {
  std::vector<ItemSpec> out;
  if (!input.HasMember("items")) {
    return out;
  }
  const auto &v = input["items"];
  if (v.IsArray()) {
    int i = 0;
    for (const auto &el : v.GetArray()) {
      out.push_back(readItemSpec(el, ("items[" + std::to_string(i) + "]").c_str()));
      ++i;
    }
  } else if (v.IsString()) {
    rapidjson::Document d;
    if (tryParseJsonDocFromString(v.GetString(), d)) {
      const rapidjson::Value *inner = unwrapEmbeddedItemsContainer(d);
      if (inner->IsArray()) {
        int i = 0;
        for (const auto &el : inner->GetArray()) {
          out.push_back(readItemSpec(
              el, ("items[" + std::to_string(i) + "]").c_str()));
          ++i;
        }
        return out;
      }
      if (inner->IsObject()) {
        out.push_back(readItemSpec(*inner, "items"));
        return out;
      }
    }
    // Fall back to the original shorthand: treat the string as task text.
    out.push_back(readItemSpec(v, "items"));
  } else if (!v.IsNull()) {
    out.push_back(readItemSpec(v, "items"));
  }
  return out;
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

std::string renderListing(const shared::AgentTodoList &list) {
  std::ostringstream out;
  for (const auto &item : list.items) {
    out << item.id << ". [" << statusMarker(item.status) << "] " << item.text
        << "\n";
  }
  return out.str();
}

std::string renderProse(const shared::AgentTodoList &list,
                        const std::string &headline,
                        const std::vector<std::string> &warnings) {
  std::ostringstream out;
  out << headline;
  if (list.items.empty()) {
    out << "\nList is now empty.";
  } else {
    int done = 0, inProgress = 0, pending = 0;
    for (const auto &item : list.items) {
      switch (item.status) {
      case shared::TodoStatus::Done:
        ++done;
        break;
      case shared::TodoStatus::InProgress:
        ++inProgress;
        break;
      case shared::TodoStatus::Pending:
        ++pending;
        break;
      }
    }
    out << " (" << done << " done, " << inProgress << " in-progress, "
        << pending << " pending)\n"
        << renderListing(list);
  }
  for (const auto &w : warnings) {
    out << "Note: " << w << "\n";
  }
  return out.str();
}

shared::ToolResult buildResult(const shared::AgentTodoList &list,
                               const std::string &headline,
                               const std::vector<std::string> &warnings) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  const std::string proseStr = renderProse(list, headline, warnings);
  doc.AddMember(
      "result",
      rapidjson::Value(proseStr.c_str(),
                       static_cast<rapidjson::SizeType>(proseStr.size()),
                       alloc)
          .Move(),
      alloc);
  doc.AddMember("next_id", list.nextId, alloc);
  // Item array stays available for the UI presenter / fleet snapshots.
  rapidjson::Value items(rapidjson::kArrayType);
  for (const auto &item : list.items) {
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("id", item.id, alloc);
    const std::string sname = statusName(item.status);
    obj.AddMember(
        "status",
        rapidjson::Value(sname.c_str(),
                         static_cast<rapidjson::SizeType>(sname.size()), alloc)
            .Move(),
        alloc);
    obj.AddMember(
        "text",
        rapidjson::Value(item.text.c_str(),
                         static_cast<rapidjson::SizeType>(item.text.size()),
                         alloc)
            .Move(),
        alloc);
    items.PushBack(obj, alloc);
  }
  doc.AddMember("items", items, alloc);
  return shared::ToolResult::ok(doc);
}

// Maintain ordering and recompute nextId based on the highest id observed.
void recomputeNextId(shared::AgentTodoList &list) {
  int maxId = 0;
  for (const auto &item : list.items) {
    if (item.id > maxId) {
      maxId = item.id;
    }
  }
  list.nextId = std::max(maxId + 1, 1);
}

std::string readAction(const rapidjson::Value &input) {
  if (!input.HasMember("action")) {
    return "";
  }
  if (!input["action"].IsString()) {
    return "";
  }
  std::string raw = input["action"].GetString();
  std::string s;
  s.reserve(raw.size());
  for (char c : raw) {
    if (c == '-' || c == '_' || c == ' ')
      continue;
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  // Normalize a few common synonyms.
  if (s == "replace" || s == "rewrite" || s == "overwrite")
    return "set";
  if (s == "append" || s == "create" || s == "new")
    return "add";
  if (s == "edit" || s == "modify")
    return "update";
  if (s == "done" || s == "finish" || s == "completed")
    return "complete";
  if (s == "delete" || s == "drop" || s == "del")
    return "remove";
  if (s == "reset" || s == "empty" || s == "wipe")
    return "clear";
  if (s == "show" || s == "get" || s == "read")
    return "list";
  return s;
}

} // namespace

shared::ToolMetadata TodoWriteTool::getMetadata() const {
  return {
      "Todo",
      "Maintain a numbered checklist of the steps you are working through "
      "for the current task. Use it to plan multi-step work, mark progress "
      "as you go, and signal completion. The persisted list is the source "
      "of truth — the result of every call returns the canonical numbered "
      "listing, so you never need to resend items you didn't change.\n"
      "\n"
      "When to use:\n"
      "- The user gives a task with 2+ distinct steps. Add them up front.\n"
      "- You finish a step. Mark it done in the same turn — don't batch.\n"
      "- You start a step. Mark it in_progress so the run is observable.\n"
      "- You discover a new sub-step. Add it; don't rebuild the list.\n"
      "\n"
      "When NOT to use:\n"
      "- Single-step requests. Just do the thing.\n"
      "- Trivial reads / lookups. Skip the ceremony.\n"
      "\n"
      "Cheat sheet (each row is one tool call):\n"
      "  add a step       {action:'add',      items:'run the tests'}\n"
      "  add several      {action:'add',      items:['plan','code','verify']}\n"
      "  start a step     {action:'update',   id:2, status:'in_progress'}\n"
      "  finish a step    {action:'complete', id:2}\n"
      "  finish several   {action:'complete', ids:[1,2,3]}\n"
      "  rename / retext  {action:'update',   id:2, text:'run unit tests'}\n"
      "  drop a step      {action:'remove',   id:2}\n"
      "  start over       {action:'set',      items:[...]}\n"
      "  wipe the list    {action:'clear'}\n"
      "  inspect          {action:'list'}",
      shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> TodoWriteTool::getSchema() const {
  // Kept intentionally small. Only `action` is required. The rest are
  // optional and the executor decides which fields apply per action.
  auto todoItemObj = zObject(
      {{"text", zString()->setOptional()},
       {"task", zString()->setOptional()},
       {"title", zString()->setOptional()},
       {"status", zString()->setOptional()},
       {"id", zInteger()->setOptional()}});
  auto todoItemUnion = zAnyOf({zString(), todoItemObj});
  auto itemsUnion = zAnyOf({zString(), todoItemObj, zArray(todoItemUnion)});

  auto updateObj = zObject({{"id", zInteger()->describe("Task id to edit.")},
                            {"status", zString()->setOptional()},
                            {"text", zString()->setOptional()}})
                       ->required({"id"});
  auto updatesUnion = zAnyOf({updateObj, zArray(updateObj), zString()});

  auto idsUnion = zAnyOf({zInteger(), zArray(zInteger()), zString()});

  return zObject(
             {{"action",
               zEnum({"set", "add", "update", "complete", "remove", "clear",
                      "list"})
                   ->describe(
                       "Which mutation to perform.\n"
                       "- 'add'      append new task(s); see `items`. "
                       "DEFAULT for putting work on the list.\n"
                       "- 'complete' mark task(s) done; see `id` / `ids`. "
                       "DEFAULT for closing finished work.\n"
                       "- 'update'   change status and/or text of task(s); "
                       "see `updates`, or shorthand `id`+`status`+`text`.\n"
                       "- 'remove'   delete task(s); see `id` / `ids`.\n"
                       "- 'set'      replace the entire list with `items`. "
                       "Use sparingly — prefer add/update/complete.\n"
                       "- 'clear'    drop every task. No other fields "
                       "needed.\n"
                      "- 'list'     read-only; return the current list "
                      "without mutating.")},
              {"items",
               itemsUnion
                   ->setOptional()
                   ->describe(
                       "Tasks for 'add' or 'set'. Several shapes accepted:\n"
                       "  \"finish the refactor\"                 (single "
                       "string shortcut)\n"
                       "  [\"plan\",\"code\",\"verify\"]               (array "
                       "of strings, all pending)\n"
                       "  [{\"text\":\"plan\"},{\"text\":\"code\","
                       "\"status\":\"in_progress\"}]\n"
                       "Status defaults to 'pending' when omitted. Schema "
                       "also accepts objects and arrays (preferred).")},
              {"updates",
               updatesUnion
                   ->setOptional()
                   ->describe(
                       "Batch edits for 'update'. Array of "
                       "{id, status?, text?} where each item needs `id` plus "
                       "at least one of `status`/`text`. Example:\n"
                       "  [{\"id\":1,\"status\":\"done\"},"
                       "{\"id\":2,\"text\":\"new wording\"}]\n"
                       "For a single edit, prefer the shorthand: "
                       "{action:'update', id:N, status:'in_progress'}.\n"
                       "Schema also accepts object/array (preferred).")},
              {"id",
               zInteger()
                   ->setOptional()
                   ->describe(
                       "Single task id. Use with 'complete', 'remove', or as "
                       "the shorthand for 'update' (then also pass `status` "
                       "and/or `text`). Equivalent to `ids:[N]`.")},
              {"ids",
               idsUnion
                   ->setOptional()
                   ->describe(
                       "Multiple task ids for 'complete' or 'remove'. "
                       "Examples: [1,2,3], a single integer, or \"1,2,3\". Unknown ids "
                       "are reported as warnings in the result, not errors. "
                       "Schema also accepts arrays/integers (preferred).")},
              {"status",
               zString()
                   ->setOptional()
                   ->describe(
                       "Status for the 'update' shorthand. One of "
                       "'pending', 'in_progress', 'done'. Aliases such as "
                       "'wip' or 'completed' are also accepted.")},
              {"text",
               zString()
                   ->setOptional()
                   ->describe(
                       "New text for the 'update' shorthand. Pair with `id` "
                       "to rename a single task without touching its "
                       "status.")}})
      ->required({"action"});
}

shared::ToolResult TodoWriteTool::execute(const rapidjson::Value &input,
                                          shared::ToolContext &ctx) {
  try {
    const std::string action = readAction(input);
    if (action.empty()) {
      throw std::runtime_error(
          "Todo requires field 'action' (one of set/add/update/complete/"
          "remove/clear/list).");
    }

    const auto &agentContext = ctx.agent.getContext();
    const std::string threadId = requireCurrentThreadId(ctx);
    const std::string agentId = agentContext.identity.id;
    if (agentId.empty()) {
      throw std::runtime_error("Cannot mutate todo list without agent id");
    }

    ThreadManager tm(ThreadManager::defaultBasePath());
    shared::AgentTodoList list = tm.getAgentTodo(threadId, agentId);
    list.threadId = threadId;
    list.agentId = agentId;
    recomputeNextId(list);

    const uint64_t now = nowEpochMs();
    std::vector<std::string> warnings;
    std::string headline;
    bool mutated = false;

    if (action == "list") {
      headline = list.items.empty() ? "Todo list is empty."
                                    : "Current todo list";
    } else if (action == "clear") {
      if (list.items.empty()) {
        headline = "Todo list was already empty.";
      } else {
        list.items.clear();
        list.nextId = 1;
        headline = "Cleared todo list.";
        mutated = true;
      }
    } else if (action == "set") {
      const auto specs = readItems(input);
      // Empty `items` for 'set' is a deliberate clear-via-set; honor it.
      std::unordered_set<int> assignedIds;
      shared::AgentTodoList rebuilt;
      rebuilt.threadId = threadId;
      rebuilt.agentId = agentId;
      // Preserve createdAt/chunkId/planId for items whose id matches.
      std::unordered_map<int, const shared::TodoItem *> oldById;
      for (const auto &item : list.items) {
        oldById[item.id] = &item;
      }
      int autoId = 1;
      for (size_t i = 0; i < specs.size(); ++i) {
        const auto &s = specs[i];
        if (s.text.empty()) {
          throw std::runtime_error(
              "items[" + std::to_string(i) +
              "] has empty text. Each item needs a non-empty text/task/title.");
        }
        shared::TodoItem item;
        if (s.hasId && s.id > 0 && !assignedIds.count(s.id)) {
          item.id = s.id;
        } else {
          while (assignedIds.count(autoId))
            ++autoId;
          item.id = autoId++;
        }
        assignedIds.insert(item.id);
        item.text = s.text;
        item.status = s.hasStatus ? s.status : shared::TodoStatus::Pending;
        auto it = oldById.find(item.id);
        if (it != oldById.end()) {
          item.createdAt = it->second->createdAt;
          item.chunkId = it->second->chunkId;
          item.planId = it->second->planId;
          item.updatedAt = (it->second->text != item.text ||
                            it->second->status != item.status)
                               ? now
                               : it->second->updatedAt;
        } else {
          item.createdAt = now;
          item.updatedAt = now;
        }
        rebuilt.items.push_back(std::move(item));
      }
      list = std::move(rebuilt);
      recomputeNextId(list);
      headline = "Replaced todo list.";
      mutated = true;
    } else if (action == "add") {
      const auto specs = readItems(input);
      if (specs.empty()) {
        throw std::runtime_error(
            "'add' requires 'items' with at least one task. Pass a string, an "
            "object {text, status?}, or an array of either.");
      }
      int added = 0;
      for (size_t i = 0; i < specs.size(); ++i) {
        const auto &s = specs[i];
        if (s.text.empty()) {
          throw std::runtime_error(
              "items[" + std::to_string(i) +
              "] has empty text. Each new task needs a non-empty text.");
        }
        shared::TodoItem item;
        item.id = list.nextId++;
        item.text = s.text;
        item.status = s.hasStatus ? s.status : shared::TodoStatus::Pending;
        item.createdAt = now;
        item.updatedAt = now;
        list.items.push_back(std::move(item));
        ++added;
      }
      headline = "Added " + std::to_string(added) +
                 (added == 1 ? " task." : " tasks.");
      mutated = true;
    } else if (action == "update") {
      // Build updates from explicit `updates` array, or fall back to a
      // single-shot {id, status?, text?} on the input itself.
      std::vector<ItemSpec> updates;
      if (input.HasMember("updates")) {
        const auto &uv = input["updates"];
        if (uv.IsArray()) {
          int i = 0;
          for (const auto &el : uv.GetArray()) {
            updates.push_back(readItemSpec(
                el, ("updates[" + std::to_string(i) + "]").c_str()));
            ++i;
          }
        } else if (uv.IsObject()) {
          updates.push_back(readItemSpec(uv, "updates"));
        } else if (uv.IsString()) {
          rapidjson::Document d;
          if (tryParseJsonDocFromString(uv.GetString(), d)) {
            if (d.IsArray()) {
              int i = 0;
              for (const auto &el : d.GetArray()) {
                updates.push_back(readItemSpec(
                    el, ("updates[" + std::to_string(i) + "]").c_str()));
                ++i;
              }
            } else if (d.IsObject()) {
              updates.push_back(readItemSpec(d, "updates"));
            }
          }
        }
      }
      if (updates.empty()) {
        // Allow {action:"update", id:N, status:"done", text:"..."} shorthand.
        ItemSpec single;
        if (input.HasMember("id")) {
          const auto &idv = input["id"];
          if (idv.IsInt()) {
            single.hasId = true;
            single.id = idv.GetInt();
          } else if (idv.IsString()) {
            try {
              single.hasId = true;
              single.id = std::stoi(idv.GetString());
            } catch (const std::exception &) {
              single.hasId = false;
            }
          }
        }
        if (input.HasMember("status") && input["status"].IsString()) {
          single.hasStatus = true;
          single.status = parseStatusName(input["status"].GetString());
        }
        if (input.HasMember("text") && input["text"].IsString()) {
          single.text = shared::StringUtil::trim(std::string(input["text"].GetString()));
        }
        if (single.hasId) {
          updates.push_back(single);
        }
      }
      if (updates.empty()) {
        throw std::runtime_error(
            "'update' requires 'updates' (array of {id, status?, text?}) or "
            "the shorthand {action:'update', id, status?, text?}.");
      }
      int touched = 0;
      for (size_t i = 0; i < updates.size(); ++i) {
        const auto &u = updates[i];
        if (!u.hasId) {
          throw std::runtime_error(
              "updates[" + std::to_string(i) +
              "] missing required 'id'. Each update needs the item id.");
        }
        if (!u.hasStatus && u.text.empty()) {
          throw std::runtime_error(
              "updates[" + std::to_string(i) +
              "] needs at least one of 'status' or 'text'.");
        }
        auto it = std::find_if(list.items.begin(), list.items.end(),
                               [&](const shared::TodoItem &item) {
                                 return item.id == u.id;
                               });
        if (it == list.items.end()) {
          warnings.push_back("id " + std::to_string(u.id) +
                             " not found; skipped.");
          continue;
        }
        bool changed = false;
        if (u.hasStatus && it->status != u.status) {
          it->status = u.status;
          changed = true;
        }
        if (!u.text.empty() && it->text != u.text) {
          it->text = u.text;
          changed = true;
        }
        if (changed) {
          it->updatedAt = now;
          ++touched;
        }
      }
      headline = "Updated " + std::to_string(touched) +
                 (touched == 1 ? " task." : " tasks.");
      if (touched > 0) {
        mutated = true;
      }
    } else if (action == "complete") {
      const auto ids = readIds(input);
      if (ids.empty()) {
        throw std::runtime_error(
            "'complete' requires 'id' (single integer) or 'ids' (array of "
            "integers).");
      }
      int touched = 0;
      for (int id : ids) {
        auto it = std::find_if(list.items.begin(), list.items.end(),
                               [&](const shared::TodoItem &item) {
                                 return item.id == id;
                               });
        if (it == list.items.end()) {
          warnings.push_back("id " + std::to_string(id) +
                             " not found; skipped.");
          continue;
        }
        if (it->status != shared::TodoStatus::Done) {
          it->status = shared::TodoStatus::Done;
          it->updatedAt = now;
          ++touched;
        }
      }
      headline = "Marked " + std::to_string(touched) +
                 (touched == 1 ? " task done." : " tasks done.");
      if (touched > 0) {
        mutated = true;
      }
    } else if (action == "remove") {
      const auto ids = readIds(input);
      if (ids.empty()) {
        throw std::runtime_error(
            "'remove' requires 'id' (single integer) or 'ids' (array of "
            "integers).");
      }
      std::unordered_set<int> existingIds;
      for (const auto &item : list.items) {
        existingIds.insert(item.id);
      }
      std::unordered_set<int> drop(ids.begin(), ids.end());
      list.items.erase(
          std::remove_if(list.items.begin(), list.items.end(),
                         [&](const shared::TodoItem &item) {
                           return drop.count(item.id) > 0;
                         }),
          list.items.end());
      int removed = 0;
      for (int id : ids) {
        if (existingIds.count(id) > 0) {
          ++removed;
        } else {
          warnings.push_back("id " + std::to_string(id) +
                             " not found; skipped.");
        }
      }
      headline = "Removed " + std::to_string(removed) +
                 (removed == 1 ? " task." : " tasks.");
      if (removed > 0) {
        mutated = true;
      }
    } else {
      throw std::runtime_error(
          "Unknown action '" + action +
          "'. Use one of: set, add, update, complete, remove, clear, list.");
    }

    if (mutated) {
      tm.writeAgentTodo(threadId, agentId, list);
      firmius::core::Harness::instance().publishEvent(
          shared::AgentTodoUpdated{threadId, agentId, list});
    }
    return buildResult(list, headline, warnings);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
