#include "agents/hooks/ScriptRuntime.hpp"

#if FIRMIUS_ENABLE_LUAU_HOOKS
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "agents/hooks/HookState.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <luacode.h>
}

#include <rapidjson/document.h>
#include <rapidjson/pointer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <variant>
#endif

namespace firmius::core::hooks {

namespace {

// Always-on no-op runtime used when Luau is not built in. Returns Allow
// with a tagged reminder so authors learn why their `kind: script` hook
// did nothing.
class DisabledRuntime final : public ScriptRuntime {
public:
  HookOutcome eval(const std::string &hookId, const std::string &,
                   const HookEnvelope &) override {
    HookOutcome out;
    out.decision = HookOutcome::Decision::Allow;
    out.outcomeLabel = "luau_disabled";
    out.tags["hook_id"] = hookId;
    out.tags["reason"] =
        "Firmius was built without FIRMIUS_ENABLE_LUAU_HOOKS=ON; "
        "kind: script hooks are inert.";
    return out;
  }
};

#if FIRMIUS_ENABLE_LUAU_HOOKS
// ─── Luau-backed runtime ───────────────────────────────────────────────────
//
// Per `eval()` we spin up a fresh `lua_State` sealed via `luaL_sandbox`,
// inject the envelope as a `event` global, expose `outcome.{allow,block,
// replace}` builders + `state.read(scope, path)`, and
// bound execution by:
//   - instruction count via `lua_callbacks(L)->interrupt`
//   - wall clock via the same interrupt polling steady_clock
//   - heap usage via a custom `lua_Alloc` that returns NULL when over
//     budget (Luau treats this as out-of-memory and raises gracefully)
//
// What is NOT bound yet (deferred to the harness-integration pass):
//   - `agent.spawn(...)`  — needs branched delegation + yielded resume.
//   - `state.write(...)`  — easy to add but writes-from-script need
//     audit hooks first; for now writes flow through the action's
//     declarative `state_writes` channel.
//   - `host.run(cmd)`     — spawning processes from Luau requires the
//     same intent-analyzer guard as Process.Execute. Deferred.
// ───────────────────────────────────────────────────────────────────────────

// Per-eval context held in `lua_callbacks(L)->userdata` so binding
// closures can find the active hook id, watch the deadline, and bail
// when the script blows its budget.
struct EvalCtx {
  std::string hookId;
  HookEnvelope env;
  ScriptLimits limits;
  std::chrono::steady_clock::time_point deadline;
  std::atomic<std::uint64_t> instructionCount{0};
  std::atomic<std::size_t> allocBytes{0};
};

// ─── Budget-tracking allocator ─────────────────────────────────────────────

void *budgetAlloc(void *ud, void *ptr, std::size_t osize, std::size_t nsize) {
  auto *ctx = static_cast<EvalCtx *>(ud);
  if (nsize == 0) {
    if (ptr != nullptr) {
      ctx->allocBytes.fetch_sub(osize, std::memory_order_relaxed);
      std::free(ptr);
    }
    return nullptr;
  }
  if (nsize > osize) {
    const std::size_t delta = nsize - osize;
    const std::size_t after =
        ctx->allocBytes.load(std::memory_order_relaxed) + delta;
    if (after > ctx->limits.memoryBudgetBytes) {
      return nullptr;  // Luau treats null-on-grow as OOM and raises.
    }
  }
  void *p = std::realloc(ptr, nsize);
  if (p != nullptr) {
    if (nsize > osize) {
      ctx->allocBytes.fetch_add(nsize - osize, std::memory_order_relaxed);
    } else if (osize > nsize) {
      ctx->allocBytes.fetch_sub(osize - nsize, std::memory_order_relaxed);
    }
  }
  return p;
}

// ─── Instruction + wall-clock interrupt ────────────────────────────────────

EvalCtx *ctxFor(lua_State *L) {
  // userdata is set on the main thread's callbacks block.
  if (auto *cbs = lua_callbacks(L); cbs != nullptr) {
    return static_cast<EvalCtx *>(cbs->userdata);
  }
  return nullptr;
}

void budgetInterrupt(lua_State *L, int gc) {
  if (gc >= 0) return;  // GC step; not a regular interrupt
  auto *ctx = ctxFor(L);
  if (ctx == nullptr) return;
  const auto count =
      ctx->instructionCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count > ctx->limits.maxInstructions) {
    luaL_error(L, "hook script: instruction budget (%llu) exceeded",
               static_cast<unsigned long long>(ctx->limits.maxInstructions));
  }
  if (std::chrono::steady_clock::now() >= ctx->deadline) {
    luaL_error(
        L, "hook script: wall-clock timeout (%lldms) reached",
        static_cast<long long>(ctx->limits.wallClockTimeout.count()));
  }
}

// ─── JSON ↔ Lua bridges ────────────────────────────────────────────────────

void pushJsonValue(lua_State *L, const rapidjson::Value &v) {
  switch (v.GetType()) {
  case rapidjson::kNullType:
    lua_pushnil(L);
    return;
  case rapidjson::kFalseType:
    lua_pushboolean(L, 0);
    return;
  case rapidjson::kTrueType:
    lua_pushboolean(L, 1);
    return;
  case rapidjson::kStringType:
    lua_pushlstring(L, v.GetString(), v.GetStringLength());
    return;
  case rapidjson::kNumberType:
    if (v.IsInt64()) {
      lua_pushinteger(L, static_cast<int>(v.GetInt64()));
    } else {
      lua_pushnumber(L, v.GetDouble());
    }
    return;
  case rapidjson::kArrayType: {
    lua_createtable(L, static_cast<int>(v.Size()), 0);
    int i = 1;
    for (const auto &elem : v.GetArray()) {
      pushJsonValue(L, elem);
      lua_rawseti(L, -2, i++);
    }
    return;
  }
  case rapidjson::kObjectType: {
    lua_createtable(L, 0, static_cast<int>(v.MemberCount()));
    for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
      lua_pushlstring(L, it->name.GetString(), it->name.GetStringLength());
      pushJsonValue(L, it->value);
      lua_settable(L, -3);
    }
    return;
  }
  }
}

// Best-effort Lua → rapidjson conversion. Tables with consecutive 1..N
// integer keys become arrays; other tables become objects.
void luaToJsonValue(lua_State *L, int idx, rapidjson::Value &out,
                    rapidjson::Document::AllocatorType &alloc) {
  idx = lua_absindex(L, idx);
  switch (lua_type(L, idx)) {
  case LUA_TNIL:
    out.SetNull();
    return;
  case LUA_TBOOLEAN:
    out.SetBool(lua_toboolean(L, idx) != 0);
    return;
  case LUA_TNUMBER:
    out.SetDouble(lua_tonumber(L, idx));
    return;
  case LUA_TSTRING: {
    std::size_t len = 0;
    const char *s = lua_tolstring(L, idx, &len);
    out.SetString(s, static_cast<rapidjson::SizeType>(len), alloc);
    return;
  }
  case LUA_TTABLE: {
    // Detect array shape: keys 1..N, all integer.
    bool isArray = true;
    int n = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
      ++n;
      if (lua_type(L, -2) != LUA_TNUMBER) {
        isArray = false;
        lua_pop(L, 2);
        break;
      }
      lua_pop(L, 1);  // pop value, keep key
    }
    if (isArray) {
      out.SetArray();
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        rapidjson::Value child;
        luaToJsonValue(L, -1, child, alloc);
        out.PushBack(child, alloc);
        lua_pop(L, 1);
      }
    } else {
      out.SetObject();
      lua_pushnil(L);
      while (lua_next(L, idx) != 0) {
        // Stringify the key.
        std::size_t klen = 0;
        const char *kstr = nullptr;
        if (lua_type(L, -2) == LUA_TSTRING) {
          kstr = lua_tolstring(L, -2, &klen);
        } else {
          // Coerce non-string keys to string for JSON compatibility.
          lua_pushvalue(L, -2);
          kstr = lua_tolstring(L, -1, &klen);
          lua_pop(L, 1);
        }
        rapidjson::Value child;
        luaToJsonValue(L, -1, child, alloc);
        rapidjson::Value key(kstr ? kstr : "", static_cast<rapidjson::SizeType>(klen), alloc);
        out.AddMember(key, child, alloc);
        lua_pop(L, 1);
      }
    }
    return;
  }
  default:
    out.SetNull();
    return;
  }
}

std::string luaTableToJson(lua_State *L, int idx) {
  rapidjson::Document doc;
  luaToJsonValue(L, idx, doc, doc.GetAllocator());
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  doc.Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

// ─── outcome.{allow,block,replace} builders ───────────────────────────────
//
// Each function takes an optional table and returns it tagged with a
// `decision` field. The host parses that table after the script returns.

int outcomeTagAndReturn(lua_State *L, const char *decision) {
  if (lua_gettop(L) == 0 || lua_type(L, 1) != LUA_TTABLE) {
    lua_createtable(L, 0, 1);
  } else {
    lua_pushvalue(L, 1);
  }
  lua_pushstring(L, decision);
  lua_setfield(L, -2, "decision");
  return 1;
}

int outcome_allow(lua_State *L)   { return outcomeTagAndReturn(L, "allow"); }
int outcome_block(lua_State *L)   { return outcomeTagAndReturn(L, "block"); }
int outcome_replace(lua_State *L) { return outcomeTagAndReturn(L, "replace"); }

// ─── state.read(scope, path) → Lua value or nil ──────────────────────────

int stateRead(lua_State *L) {
  const char *scope = luaL_checkstring(L, 1);
  const char *path = luaL_checkstring(L, 2);
  auto *ctx = ctxFor(L);
  const std::string hookId = ctx ? ctx->hookId : "";

  HookState::Scope s;
  try {
    s = parseScope(scope);
  } catch (const std::exception &) {
    luaL_error(L, "state.read: unknown scope %s", scope);
  }
  auto val = HookState::instance().readJson(s, path, hookId);
  if (!val) {
    lua_pushnil(L);
    return 1;
  }
  rapidjson::Document d;
  if (d.Parse(val->c_str()).HasParseError()) {
    lua_pushlstring(L, val->data(), val->size());
    return 1;
  }
  pushJsonValue(L, d);
  return 1;
}

int stateWrite(lua_State *L) {
  const char *scope = luaL_checkstring(L, 1);
  const char *path = luaL_checkstring(L, 2);
  auto *ctx = ctxFor(L);
  const std::string hookId = ctx ? ctx->hookId : "";

  HookState::Scope s;
  try {
    s = parseScope(scope);
  } catch (const std::exception &) {
    luaL_error(L, "state.write: unknown scope %s", scope);
  }

  rapidjson::Document doc;
  rapidjson::Value value;
  luaToJsonValue(L, 3, value, doc.GetAllocator());
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  value.Accept(writer);
  lua_pushboolean(L,
                  HookState::instance().writeJson(
                      s, path, std::string(sb.GetString(), sb.GetSize()),
                      hookId));
  return 1;
}

int stateAppend(lua_State *L) {
  const char *scope = luaL_checkstring(L, 1);
  const char *path = luaL_checkstring(L, 2);
  auto *ctx = ctxFor(L);
  const std::string hookId = ctx ? ctx->hookId : "";

  HookState::Scope s;
  try {
    s = parseScope(scope);
  } catch (const std::exception &) {
    luaL_error(L, "state.append: unknown scope %s", scope);
  }

  rapidjson::Document doc;
  rapidjson::Value value;
  luaToJsonValue(L, 3, value, doc.GetAllocator());
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  value.Accept(writer);
  lua_pushboolean(L,
                  HookState::instance().appendJson(
                      s, path, std::string(sb.GetString(), sb.GetSize()),
                      hookId));
  return 1;
}

std::string textFromMessage(const firmius::shared::Message &msg) {
  std::string out;
  for (const auto &part : msg.content) {
    if (const auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
      out += txt->text;
    }
  }
  return out;
}

std::string roleName(firmius::shared::Role role) {
  switch (role) {
  case firmius::shared::Role::System:
    return "system";
  case firmius::shared::Role::User:
    return "user";
  case firmius::shared::Role::Assistant:
    return "assistant";
  case firmius::shared::Role::ToolResult:
    return "tool_result";
  case firmius::shared::Role::Error:
    return "error";
  }
  return "unknown";
}

void addString(rapidjson::Value &obj, const char *key, const std::string &value,
               rapidjson::Document::AllocatorType &alloc) {
  obj.AddMember(rapidjson::Value(key, alloc).Move(),
                rapidjson::Value(value.c_str(), alloc).Move(), alloc);
}

firmius::shared::AgentHistory historyForEnv(const HookEnvelope &env) {
  firmius::shared::AgentHistory history;
  if (auto agent =
          firmius::core::AgentRegistry::instance().getAgent(env.agentId)) {
    if (agent->getContext().history) {
      return *agent->getContext().history;
    }
  }
  if (!env.threadId.empty()) {
    try {
      history = firmius::core::ThreadManager(
                    firmius::core::ThreadManager::defaultBasePath())
                    .loadAgentHistory(env.threadId, env.agentId);
    } catch (...) {
    }
  }
  return history;
}

struct ThreadFilter {
  std::string role;
  std::string tool;
  std::string messageContains;
  int sinceTurn = 0;
  int limit = 0;
  std::optional<bool> success;
};

ThreadFilter parseThreadFilter(lua_State *L, int idx) {
  ThreadFilter f;
  if (lua_gettop(L) < idx || lua_type(L, idx) != LUA_TTABLE) {
    return f;
  }
  idx = lua_absindex(L, idx);
  auto getStringField = [&](const char *key, std::string &out) {
    lua_getfield(L, idx, key);
    if (lua_type(L, -1) == LUA_TSTRING) {
      out = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
  };
  auto getIntField = [&](const char *key, int &out) {
    lua_getfield(L, idx, key);
    if (lua_type(L, -1) == LUA_TNUMBER) {
      out = static_cast<int>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
  };
  getStringField("role", f.role);
  getStringField("tool", f.tool);
  getStringField("message_contains", f.messageContains);
  getStringField("contains", f.messageContains);
  getIntField("since_turn", f.sinceTurn);
  getIntField("limit", f.limit);
  lua_getfield(L, idx, "success");
  if (lua_type(L, -1) == LUA_TBOOLEAN) {
    f.success = lua_toboolean(L, -1) != 0;
  }
  lua_pop(L, 1);
  return f;
}

bool filterAcceptsText(const ThreadFilter &filter, const std::string &text) {
  return filter.messageContains.empty() ||
         text.find(filter.messageContains) != std::string::npos;
}

void addMessageParts(rapidjson::Value &msgObj,
                     const firmius::shared::Message &msg,
                     rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value parts(rapidjson::kArrayType);
  rapidjson::Value calls(rapidjson::kArrayType);
  rapidjson::Value results(rapidjson::kArrayType);
  for (const auto &part : msg.content) {
    rapidjson::Value item(rapidjson::kObjectType);
    if (const auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
      addString(item, "kind", "text", alloc);
      addString(item, "text", txt->text, alloc);
    } else if (const auto *thinking =
                   std::get_if<firmius::shared::ThinkingContent>(&part)) {
      addString(item, "kind", "thinking", alloc);
      addString(item, "text", thinking->thinking, alloc);
    } else if (const auto *call =
                   std::get_if<firmius::shared::ToolCallContent>(&part)) {
      addString(item, "kind", "tool_call", alloc);
      addString(item, "id", call->id, alloc);
      addString(item, "name", call->name, alloc);
      addString(item, "args", call->args, alloc);
      rapidjson::Value callObj(rapidjson::kObjectType);
      addString(callObj, "id", call->id, alloc);
      addString(callObj, "name", call->name, alloc);
      addString(callObj, "args", call->args, alloc);
      calls.PushBack(callObj, alloc);
    } else if (const auto *result =
                   std::get_if<firmius::shared::ToolResultContent>(&part)) {
      addString(item, "kind", "tool_result", alloc);
      addString(item, "tool_call_id", result->toolCallId, alloc);
      addString(item, "result", result->result, alloc);
      item.AddMember("success", result->success, alloc);
      rapidjson::Value resultObj(rapidjson::kObjectType);
      addString(resultObj, "tool_call_id", result->toolCallId, alloc);
      addString(resultObj, "result", result->result, alloc);
      addString(resultObj, "process_id", result->processId, alloc);
      addString(resultObj, "subagent_id", result->subagentId, alloc);
      resultObj.AddMember("success", result->success, alloc);
      results.PushBack(resultObj, alloc);
    } else if (const auto *image =
                   std::get_if<firmius::shared::ImageContent>(&part)) {
      addString(item, "kind", "image", alloc);
      addString(item, "url", image->url, alloc);
      addString(item, "media_type", image->mediaType, alloc);
      addString(item, "detail", image->detail, alloc);
    } else if (const auto *err =
                   std::get_if<firmius::shared::ErrorContent>(&part)) {
      addString(item, "kind", "error", alloc);
      addString(item, "name", err->errorName, alloc);
      addString(item, "description", err->description, alloc);
      addString(item, "details", err->details, alloc);
    } else if (const auto *notice =
                   std::get_if<firmius::shared::NoticeContent>(&part)) {
      addString(item, "kind", "notice", alloc);
      addString(item, "title", notice->title, alloc);
      addString(item, "message", notice->message, alloc);
      addString(item, "details", notice->details, alloc);
    }
    if (item.MemberCount() > 0) {
      parts.PushBack(item, alloc);
    }
  }
  msgObj.AddMember("parts", parts, alloc);
  msgObj.AddMember("tool_calls", calls, alloc);
  msgObj.AddMember("tool_results", results, alloc);
}

rapidjson::Document buildThreadLogSummary(const HookEnvelope &env) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  addString(doc, "thread_id", env.threadId, alloc);
  addString(doc, "agent_id", env.agentId, alloc);

  firmius::shared::AgentHistory history = historyForEnv(env);
  if (auto agent = firmius::core::AgentRegistry::instance().getAgent(env.agentId)) {
    if (agent->getContext().history) {
      rapidjson::Value reads(rapidjson::kArrayType);
      for (const auto &path : agent->getContext().state.readFiles) {
        reads.PushBack(rapidjson::Value(path.c_str(), alloc).Move(), alloc);
      }
      doc.AddMember("files_read", reads, alloc);

      rapidjson::Value edits(rapidjson::kArrayType);
      for (const auto &path : agent->getContext().state.editedFiles) {
        edits.PushBack(rapidjson::Value(path.c_str(), alloc).Move(), alloc);
      }
      doc.AddMember("files_edited", edits, alloc);

      rapidjson::Value metrics(rapidjson::kObjectType);
      metrics.AddMember("tokens_total",
                        agent->getContext().aggregateMetrics.tokens.total,
                        alloc);
      metrics.AddMember("tokens_prompt",
                        agent->getContext().aggregateMetrics.tokens.prompt,
                        alloc);
      metrics.AddMember("tokens_completion",
                        agent->getContext().aggregateMetrics.tokens.completion,
                        alloc);
      metrics.AddMember("tool_execution_ms",
                        agent->getContext().aggregateMetrics.timing.toolExecutionMs,
                        alloc);
      doc.AddMember("metrics", metrics, alloc);
    }
  }

  rapidjson::Value toolCalls(rapidjson::kArrayType);
  rapidjson::Value toolResults(rapidjson::kArrayType);
  rapidjson::Value commands(rapidjson::kArrayType);
  std::string finalMessage;

  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == firmius::shared::Role::Assistant) {
        const std::string text = textFromMessage(msg);
        if (!firmius::shared::StringUtil::trim(text).empty()) {
          finalMessage = text;
        }
      }
      for (const auto &part : msg.content) {
        if (const auto *call =
                std::get_if<firmius::shared::ToolCallContent>(&part)) {
          rapidjson::Value c(rapidjson::kObjectType);
          addString(c, "id", call->id, alloc);
          addString(c, "name", call->name, alloc);
          addString(c, "args", call->args, alloc);
          toolCalls.PushBack(c, alloc);

          rapidjson::Document args;
          if ((call->name == "Process" || call->name == "process") &&
              !args.Parse(call->args.c_str()).HasParseError() &&
              args.IsObject() && args.HasMember("command") &&
              args["command"].IsString()) {
            commands.PushBack(
                rapidjson::Value(args["command"].GetString(), alloc).Move(),
                alloc);
          }
        } else if (const auto *result =
                       std::get_if<firmius::shared::ToolResultContent>(&part)) {
          rapidjson::Value r(rapidjson::kObjectType);
          addString(r, "tool_call_id", result->toolCallId, alloc);
          addString(r, "result", result->result, alloc);
          r.AddMember("success", result->success, alloc);
          toolResults.PushBack(r, alloc);
        }
      }
    }
  }

  addString(doc, "final_message", finalMessage, alloc);
  doc.AddMember("tool_calls", toolCalls, alloc);
  doc.AddMember("tool_results", toolResults, alloc);
  doc.AddMember("commands_run", commands, alloc);
  return doc;
}

int threadLogSummary(lua_State *L) {
  auto *ctx = ctxFor(L);
  if (ctx == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  rapidjson::Document doc = buildThreadLogSummary(ctx->env);
  pushJsonValue(L, doc);
  return 1;
}

int threadHistory(lua_State *L) {
  auto *ctx = ctxFor(L);
  if (ctx == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  const ThreadFilter filter = parseThreadFilter(L, 1);
  const auto history = historyForEnv(ctx->env);
  rapidjson::Document doc;
  doc.SetArray();
  auto &alloc = doc.GetAllocator();
  int emitted = 0;
  for (std::size_t ti = 0; ti < history.turns.size(); ++ti) {
    if (filter.sinceTurn > 0 &&
        static_cast<int>(ti + 1) < filter.sinceTurn) {
      continue;
    }
    const auto &turn = history.turns[ti];
    rapidjson::Value turnObj(rapidjson::kObjectType);
    addString(turnObj, "turn_id", turn.turnId, alloc);
    turnObj.AddMember("turn_index", static_cast<int>(ti + 1), alloc);
    rapidjson::Value messages(rapidjson::kArrayType);
    for (std::size_t mi = 0; mi < turn.messages.size(); ++mi) {
      const auto &msg = turn.messages[mi];
      const std::string role = roleName(msg.role);
      const std::string text = textFromMessage(msg);
      if (!filter.role.empty() && filter.role != role) {
        continue;
      }
      if (!filterAcceptsText(filter, text)) {
        continue;
      }
      rapidjson::Value msgObj(rapidjson::kObjectType);
      addString(msgObj, "id", msg.id, alloc);
      addString(msgObj, "role", role, alloc);
      addString(msgObj, "text", text, alloc);
      msgObj.AddMember("message_index", static_cast<int>(mi + 1), alloc);
      msgObj.AddMember("timestamp", msg.timestamp, alloc);
      addMessageParts(msgObj, msg, alloc);
      messages.PushBack(msgObj, alloc);
    }
    if (!messages.Empty()) {
      turnObj.AddMember("messages", messages, alloc);
      doc.PushBack(turnObj, alloc);
      ++emitted;
      if (filter.limit > 0 && emitted >= filter.limit) {
        break;
      }
    }
  }
  pushJsonValue(L, doc);
  return 1;
}

int threadMessages(lua_State *L) {
  auto *ctx = ctxFor(L);
  if (ctx == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  const ThreadFilter filter = parseThreadFilter(L, 1);
  const auto history = historyForEnv(ctx->env);
  rapidjson::Document doc;
  doc.SetArray();
  auto &alloc = doc.GetAllocator();
  int emitted = 0;
  for (std::size_t ti = 0; ti < history.turns.size(); ++ti) {
    if (filter.sinceTurn > 0 &&
        static_cast<int>(ti + 1) < filter.sinceTurn) {
      continue;
    }
    const auto &turn = history.turns[ti];
    for (std::size_t mi = 0; mi < turn.messages.size(); ++mi) {
      const auto &msg = turn.messages[mi];
      const std::string role = roleName(msg.role);
      const std::string text = textFromMessage(msg);
      if (!filter.role.empty() && filter.role != role) {
        continue;
      }
      if (!filterAcceptsText(filter, text)) {
        continue;
      }
      rapidjson::Value msgObj(rapidjson::kObjectType);
      addString(msgObj, "turn_id", turn.turnId, alloc);
      msgObj.AddMember("turn_index", static_cast<int>(ti + 1), alloc);
      msgObj.AddMember("message_index", static_cast<int>(mi + 1), alloc);
      addString(msgObj, "id", msg.id, alloc);
      addString(msgObj, "role", role, alloc);
      addString(msgObj, "text", text, alloc);
      msgObj.AddMember("timestamp", msg.timestamp, alloc);
      addMessageParts(msgObj, msg, alloc);
      doc.PushBack(msgObj, alloc);
      ++emitted;
      if (filter.limit > 0 && emitted >= filter.limit) {
        pushJsonValue(L, doc);
        return 1;
      }
    }
  }
  pushJsonValue(L, doc);
  return 1;
}

int threadToolCalls(lua_State *L) {
  auto *ctx = ctxFor(L);
  if (ctx == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  const ThreadFilter filter = parseThreadFilter(L, 1);
  const auto history = historyForEnv(ctx->env);
  std::map<std::string, const firmius::shared::ToolResultContent *> results;
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (const auto *result =
                std::get_if<firmius::shared::ToolResultContent>(&part)) {
          results[result->toolCallId] = result;
        }
      }
    }
  }

  rapidjson::Document doc;
  doc.SetArray();
  auto &alloc = doc.GetAllocator();
  int emitted = 0;
  for (std::size_t ti = 0; ti < history.turns.size(); ++ti) {
    if (filter.sinceTurn > 0 &&
        static_cast<int>(ti + 1) < filter.sinceTurn) {
      continue;
    }
    const auto &turn = history.turns[ti];
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        const auto *call =
            std::get_if<firmius::shared::ToolCallContent>(&part);
        if (!call) {
          continue;
        }
        if (!filter.tool.empty() && filter.tool != call->name) {
          continue;
        }
        rapidjson::Value obj(rapidjson::kObjectType);
        addString(obj, "turn_id", turn.turnId, alloc);
        obj.AddMember("turn_index", static_cast<int>(ti + 1), alloc);
        addString(obj, "message_id", msg.id, alloc);
        addString(obj, "id", call->id, alloc);
        addString(obj, "name", call->name, alloc);
        addString(obj, "args", call->args, alloc);
        auto resultIt = results.find(call->id);
        if (resultIt != results.end()) {
          const auto *result = resultIt->second;
          rapidjson::Value resultObj(rapidjson::kObjectType);
          addString(resultObj, "tool_call_id", result->toolCallId, alloc);
          addString(resultObj, "result", result->result, alloc);
          addString(resultObj, "process_id", result->processId, alloc);
          addString(resultObj, "subagent_id", result->subagentId, alloc);
          resultObj.AddMember("success", result->success, alloc);
          obj.AddMember("result", resultObj, alloc);
          obj.AddMember("success", result->success, alloc);
        }
        doc.PushBack(obj, alloc);
        ++emitted;
        if (filter.limit > 0 && emitted >= filter.limit) {
          pushJsonValue(L, doc);
          return 1;
        }
      }
    }
  }
  pushJsonValue(L, doc);
  return 1;
}

int threadToolResults(lua_State *L) {
  auto *ctx = ctxFor(L);
  if (ctx == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  const ThreadFilter filter = parseThreadFilter(L, 1);
  const auto history = historyForEnv(ctx->env);
  rapidjson::Document doc;
  doc.SetArray();
  auto &alloc = doc.GetAllocator();
  int emitted = 0;
  for (std::size_t ti = 0; ti < history.turns.size(); ++ti) {
    if (filter.sinceTurn > 0 &&
        static_cast<int>(ti + 1) < filter.sinceTurn) {
      continue;
    }
    const auto &turn = history.turns[ti];
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        const auto *result =
            std::get_if<firmius::shared::ToolResultContent>(&part);
        if (!result) {
          continue;
        }
        if (filter.success.has_value() && *filter.success != result->success) {
          continue;
        }
        if (!filterAcceptsText(filter, result->result)) {
          continue;
        }
        rapidjson::Value obj(rapidjson::kObjectType);
        addString(obj, "turn_id", turn.turnId, alloc);
        obj.AddMember("turn_index", static_cast<int>(ti + 1), alloc);
        addString(obj, "message_id", msg.id, alloc);
        addString(obj, "tool_call_id", result->toolCallId, alloc);
        addString(obj, "result", result->result, alloc);
        addString(obj, "process_id", result->processId, alloc);
        addString(obj, "subagent_id", result->subagentId, alloc);
        obj.AddMember("success", result->success, alloc);
        doc.PushBack(obj, alloc);
        ++emitted;
        if (filter.limit > 0 && emitted >= filter.limit) {
          pushJsonValue(L, doc);
          return 1;
        }
      }
    }
  }
  pushJsonValue(L, doc);
  return 1;
}

int agentSpawn(lua_State *L) {
  auto *ctx = ctxFor(L);
  if (ctx == nullptr) {
    luaL_error(L, "agent.spawn: runtime context missing");
  }
  const char *persona = luaL_checkstring(L, 1);
  const char *task = luaL_checkstring(L, 2);
  int timeoutSec = 180;
  if (lua_gettop(L) >= 3 && lua_type(L, 3) == LUA_TTABLE) {
    lua_getfield(L, 3, "timeout_sec");
    if (lua_type(L, -1) == LUA_TNUMBER) {
      timeoutSec = static_cast<int>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
  }

  const std::string childId = firmius::core::Engine::instance().summonAgent(
      ctx->env.threadId, persona, task, true, ctx->env.agentId, persona,
      "Hook Validator");
  auto outcome = firmius::core::Engine::instance().waitForAgentOutcome(
      childId, std::chrono::seconds(timeoutSec));

  rapidjson::Document ret;
  ret.SetObject();
  auto &alloc = ret.GetAllocator();
  addString(ret, "agent_id", childId, alloc);
  if (!outcome.has_value()) {
    addString(ret, "kind", "timeout", alloc);
    addString(ret, "text", "", alloc);
  } else {
    std::string kind = "response";
    switch (outcome->kind) {
    case firmius::shared::AgentOutcome::Kind::Response:
      kind = "response";
      break;
    case firmius::shared::AgentOutcome::Kind::NoSummary:
      kind = "no_summary";
      break;
    case firmius::shared::AgentOutcome::Kind::Cancelled:
      kind = "cancelled";
      break;
    case firmius::shared::AgentOutcome::Kind::Failed:
      kind = "failed";
      break;
    }
    addString(ret, "kind", kind, alloc);
    addString(ret, "text", outcome->text, alloc);
    rapidjson::Document parsed;
    if (!outcome->text.empty() &&
        !parsed.Parse(outcome->text.c_str()).HasParseError() &&
        parsed.IsObject()) {
      rapidjson::Value parsedCopy(rapidjson::kObjectType);
      parsedCopy.CopyFrom(parsed, alloc);
      ret.AddMember("json", parsedCopy, alloc);
    }
  }
  pushJsonValue(L, ret);
  return 1;
}

// ─── Outcome parsing — Lua return → HookOutcome ───────────────────────────

HookOutcome parseScriptReturn(lua_State *L, const std::string &hookId) {
  HookOutcome out;
  out.tags["hook_id"] = hookId;

  if (lua_gettop(L) == 0 || lua_type(L, -1) != LUA_TTABLE) {
    if (lua_gettop(L) > 0 && lua_type(L, -1) == LUA_TSTRING) {
      out.decision = HookOutcome::Decision::Allow;
      out.outcomeLabel = "text";
      out.reminderForAgent = lua_tostring(L, -1);
      return out;
    }
    out.decision = HookOutcome::Decision::Allow;
    out.outcomeLabel = "no_return";
    return out;
  }

  // decision
  lua_getfield(L, -1, "decision");
  if (lua_type(L, -1) == LUA_TSTRING) {
    const std::string d = lua_tostring(L, -1);
    if (d == "block")        out.decision = HookOutcome::Decision::Block;
    else if (d == "replace") out.decision = HookOutcome::Decision::Replace;
    else                     out.decision = HookOutcome::Decision::Allow;
  }
  lua_pop(L, 1);

  auto pullStr = [&](const char *key, std::string &dst) {
    lua_getfield(L, -1, key);
    if (lua_type(L, -1) == LUA_TSTRING) dst = lua_tostring(L, -1);
    lua_pop(L, 1);
  };

  pullStr("reason", out.blockReason);
  pullStr("outcome", out.outcomeLabel);

  std::string reminder;
  pullStr("reminder", reminder);
  if (reminder.empty()) {
    pullStr("text", reminder);
  }
  if (!reminder.empty()) out.reminderForAgent = reminder;

  // replacement_args (Lua key `args` for ergonomics in outcome.replace)
  lua_getfield(L, -1, "args");
  if (lua_type(L, -1) == LUA_TTABLE) {
    out.replacementToolArgs = luaTableToJson(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, -1, "state_writes");
  if (lua_type(L, -1) == LUA_TTABLE) {
    const int writesIdx = lua_absindex(L, -1);
    const int n = static_cast<int>(lua_objlen(L, writesIdx));
    for (int i = 1; i <= n; ++i) {
      lua_rawgeti(L, writesIdx, i);
      if (lua_type(L, -1) == LUA_TTABLE) {
        HookOutcome::StateWrite sw;
        lua_getfield(L, -1, "scope");
        if (lua_type(L, -1) == LUA_TSTRING) sw.scope = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "path");
        if (lua_type(L, -1) == LUA_TSTRING) sw.path = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "value");
        if (!sw.scope.empty() && !sw.path.empty()) {
          sw.valueJson = luaTableToJson(L, -1);
          out.stateWrites.push_back(std::move(sw));
        }
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  return out;
}

HookOutcome scriptError(const std::string &hookId, const std::string &what) {
  HookOutcome out;
  out.decision = HookOutcome::Decision::Allow;
  out.outcomeLabel = "script_error";
  out.tags["hook_id"] = hookId;
  out.tags["script_error"] = what;
  out.reminderForAgent = std::string("<FIRMIUS_HOOK id=\"") + hookId +
                         "\" exit=\"-1\">\nLuau error: " + what +
                         "\n</FIRMIUS_HOOK>";
  return out;
}

// ─── The runtime ──────────────────────────────────────────────────────────

class LuauRuntime final : public ScriptRuntime {
public:
  explicit LuauRuntime(const ScriptLimits &limits) : limits_(limits) {}

  HookOutcome eval(const std::string &hookId, const std::string &scriptBody,
                   const HookEnvelope &env) override {
    EvalCtx ctx;
    ctx.hookId = hookId;
    ctx.env = env;
    ctx.limits = limits_;
    ctx.deadline =
        std::chrono::steady_clock::now() + limits_.wallClockTimeout;

    lua_State *L = lua_newstate(&budgetAlloc, &ctx);
    if (L == nullptr) return scriptError(hookId, "lua_newstate failed");

    luaL_openlibs(L);
    luaL_sandbox(L);

    // Globals are readonly after sandboxing; temporarily open writes while
    // wiring the host-provided modules, then freeze them again before user
    // code runs.
    lua_setreadonly(L, LUA_GLOBALSINDEX, 0);
    if (auto *cbs = lua_callbacks(L); cbs != nullptr) {
      cbs->userdata = &ctx;
      cbs->interrupt = &budgetInterrupt;
    }

    // Inject `event` (parsed envelope, read-only).
    {
      const std::string envelopeJson = serializeEnvelope(env);
      rapidjson::Document evdoc;
      if (!evdoc.Parse(envelopeJson.c_str()).HasParseError()) {
        pushJsonValue(L, evdoc);
      } else {
        lua_createtable(L, 0, 0);
      }
      lua_setglobal(L, "event");
    }

    // Inject `outcome` module.
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, &outcome_allow, "allow");
    lua_setfield(L, -2, "allow");
    lua_pushcfunction(L, &outcome_block, "block");
    lua_setfield(L, -2, "block");
    lua_pushcfunction(L, &outcome_replace, "replace");
    lua_setfield(L, -2, "replace");
    lua_setglobal(L, "outcome");

    // Inject state and runtime inspection APIs. These are intentionally
    // small, synchronous, and deterministic at the C++ boundary so hook
    // behavior lives in Lua without growing the YAML surface.
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, &stateRead, "read");
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, &stateWrite, "write");
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, &stateAppend, "append");
    lua_setfield(L, -2, "append");
    lua_setglobal(L, "state");

    lua_createtable(L, 0, 5);
    lua_pushcfunction(L, &threadLogSummary, "log_summary");
    lua_setfield(L, -2, "log_summary");
    lua_pushcfunction(L, &threadHistory, "history");
    lua_setfield(L, -2, "history");
    lua_pushcfunction(L, &threadMessages, "messages");
    lua_setfield(L, -2, "messages");
    lua_pushcfunction(L, &threadToolCalls, "tool_calls");
    lua_setfield(L, -2, "tool_calls");
    lua_pushcfunction(L, &threadToolResults, "tool_results");
    lua_setfield(L, -2, "tool_results");
    lua_setglobal(L, "thread");

    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, &agentSpawn, "spawn");
    lua_setfield(L, -2, "spawn");
    lua_setglobal(L, "agent");

    lua_setreadonly(L, LUA_GLOBALSINDEX, 1);

    // Compile.
    std::size_t bcSize = 0;
    char *bytecode = luau_compile(scriptBody.data(), scriptBody.size(),
                                  nullptr, &bcSize);
    if (bytecode == nullptr) {
      lua_close(L);
      return scriptError(hookId, "luau_compile returned null");
    }
    const std::string chunkName = "=" + hookId;
    int rc = luau_load(L, chunkName.c_str(), bytecode, bcSize, 0);
    std::free(bytecode);
    if (rc != 0) {
      const char *err = lua_tostring(L, -1);
      const std::string what = err ? err : "unknown load error";
      lua_close(L);
      return scriptError(hookId, what);
    }

    rc = lua_pcall(L, 0, 1, 0);
    if (rc != 0) {
      const char *err = lua_tostring(L, -1);
      const std::string what = err ? err : "unknown runtime error";
      lua_close(L);
      return scriptError(hookId, what);
    }

    HookOutcome out = parseScriptReturn(L, hookId);
    out.tags["instructions"] =
        std::to_string(ctx.instructionCount.load(std::memory_order_relaxed));
    out.tags["alloc_bytes"] =
        std::to_string(ctx.allocBytes.load(std::memory_order_relaxed));
    lua_close(L);
    return out;
  }

private:
  ScriptLimits limits_;
};
#endif  // FIRMIUS_ENABLE_LUAU_HOOKS

} // namespace

std::unique_ptr<ScriptRuntime> ScriptRuntime::create(
    const ScriptLimits &limits) {
#if FIRMIUS_ENABLE_LUAU_HOOKS
  return std::make_unique<LuauRuntime>(limits);
#else
  (void)limits;
  return std::make_unique<DisabledRuntime>();
#endif
}

bool ScriptRuntime::enabled() {
#if FIRMIUS_ENABLE_LUAU_HOOKS
  return true;
#else
  return false;
#endif
}

} // namespace firmius::core::hooks
