#include "agents/hooks/HookState.hpp"
#include "agents/hooks/HookRegistry.hpp"

#include <rapidjson/document.h>
#include <rapidjson/pointer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace firmius::core::hooks {

namespace fs = std::filesystem;

// ───────────────────────────────────────────────────────────────────────────
// Scope name helpers
// ───────────────────────────────────────────────────────────────────────────

const char *scopeName(HookState::Scope s) {
  switch (s) {
  case HookState::Scope::Global: return "global";
  case HookState::Scope::Thread: return "thread";
  case HookState::Scope::Agent:  return "agent";
  case HookState::Scope::Hook:   return "hook";
  }
  return "unknown";
}

HookState::Scope parseScope(const std::string &name) {
  if (name == "global") return HookState::Scope::Global;
  if (name == "thread") return HookState::Scope::Thread;
  if (name == "agent")  return HookState::Scope::Agent;
  if (name == "hook")   return HookState::Scope::Hook;
  throw std::runtime_error("Unknown HookState scope: " + name);
}

// ───────────────────────────────────────────────────────────────────────────
// Path translator: dotted Firmius paths → RFC 6901 JSON Pointer
//
//   foo.bar       → /foo/bar
//   foo.bar[3]    → /foo/bar/3
//   foo.bar[]     → /foo/bar  + isAppend = true (caller pushes back)
//
// The append marker `[]` is only meaningful at the very end of a path; it
// tells the writer to allocate a new array slot rather than overwrite an
// indexed one. Mid-path `[]` is treated as malformed (returns empty).
// ───────────────────────────────────────────────────────────────────────────

namespace {

std::string toJsonPointer(const std::string &path, bool &isAppend) {
  isAppend = false;
  if (path.empty()) return {};
  std::string out;
  out.reserve(path.size() + 2);
  out.push_back('/');
  for (std::size_t i = 0; i < path.size(); ++i) {
    char c = path[i];
    if (c == '.') {
      out.push_back('/');
    } else if (c == '[') {
      const auto close = path.find(']', i);
      if (close == std::string::npos) return {};
      const auto inner = path.substr(i + 1, close - i - 1);
      if (inner.empty()) {
        if (close + 1 != path.size()) return {};  // [] only valid at end
        isAppend = true;
        i = close;
      } else {
        out.push_back('/');
        out.append(inner);
        i = close;
      }
    } else if (c == '/' || c == '~') {
      // RFC 6901 escaping: ~ → ~0, / → ~1
      out.push_back('~');
      out.push_back(c == '/' ? '1' : '0');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// ───────────────────────────────────────────────────────────────────────────
// Atomic JSON file writer
//
// 1. Serialize the document to a memory buffer.
// 2. Write to <path>.tmp.
// 3. fsync the tmp file (POSIX) to flush kernel buffers.
// 4. rename <tmp> → <path> — POSIX guarantees atomicity within a filesystem.
// 5. fsync the parent directory so the rename itself is durable.
//
// On non-POSIX builds we fall back to a non-fsync rename which is still
// atomic on Windows-NTFS via MoveFileExW, but loses the durability guard.
// ───────────────────────────────────────────────────────────────────────────

bool atomicWriteJson(const fs::path &target, const rapidjson::Document &doc) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec) return false;

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);

  const fs::path tmp = target.string() + ".tmp";
#if defined(__unix__) || defined(__APPLE__)
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const char *data = sb.GetString();
  std::size_t remaining = sb.GetSize();
  while (remaining > 0) {
    const ssize_t n = ::write(fd, data, remaining);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      ::unlink(tmp.c_str());
      return false;
    }
    data += n;
    remaining -= static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    ::unlink(tmp.c_str());
    return false;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), target.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  // Best-effort directory fsync for rename durability.
  const int dfd = ::open(target.parent_path().c_str(), O_RDONLY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
#else
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(sb.GetString(), static_cast<std::streamsize>(sb.GetSize()));
    if (!out) return false;
  }
  fs::rename(tmp, target, ec);
  return !ec;
#endif
}

// ───────────────────────────────────────────────────────────────────────────
// File location resolver
// ───────────────────────────────────────────────────────────────────────────

fs::path firmiusHomeDir() {
  if (const char *e = std::getenv("FIRMIUS_HOME"); e && *e) return fs::path(e);
  if (const char *h = std::getenv("HOME"); h && *h) return fs::path(h) / ".firmius";
  return fs::current_path() / ".firmius";
}

fs::path scopeFilePath(HookState::Scope scope, const std::string &threadId,
                       const std::string &hookId) {
  const fs::path root = firmiusHomeDir();
  switch (scope) {
  case HookState::Scope::Global:
    return root / "hook-state" / "global.json";
  case HookState::Scope::Thread:
    if (threadId.empty()) return {};
    return root / "threads" / threadId / "hook-state.json";
  case HookState::Scope::Agent:
    // Agent scope is in-memory only for now; persistence rides alongside
    // thread snapshot on a follow-up pass. Returning an empty path tells
    // the writer not to flush.
    return {};
  case HookState::Scope::Hook:
    if (hookId.empty()) return {};
    return root / "hook-state" / "by-hook" / (hookId + ".json");
  }
  return {};
}

bool loadJsonFile(const fs::path &p, rapidjson::Document &doc) {
  doc.SetObject();
  if (p.empty() || !fs::exists(p)) return true;
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string body = ss.str();
  if (body.empty()) {
    doc.SetObject();
    return true;
  }
  rapidjson::Document tmp;
  if (tmp.Parse(body.c_str()).HasParseError()) return false;
  if (!tmp.IsObject()) {
    doc.SetObject();
    return true;
  }
  doc.CopyFrom(tmp, doc.GetAllocator());
  return true;
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// ScopeStore — owns one Document per (scope, instance) plus its dirty flag.
// ───────────────────────────────────────────────────────────────────────────

struct HookState::ScopeStore {
  rapidjson::Document doc;
  fs::path filePath;            // empty for purely in-memory scopes
  bool loaded = false;
  bool dirty = false;
};

namespace {
// One global, plus one thread store keyed by threadId, plus per-hook stores
// keyed by hookId. Agent store is held in HookState::instance() directly.
struct StateInner {
  std::unique_ptr<HookState::ScopeStore> global = std::make_unique<HookState::ScopeStore>();
  std::unique_ptr<HookState::ScopeStore> agent = std::make_unique<HookState::ScopeStore>();
  std::unordered_map<std::string, std::unique_ptr<HookState::ScopeStore>> threads;
  std::unordered_map<std::string, std::unique_ptr<HookState::ScopeStore>> hooks;
};

StateInner &inner() {
  static StateInner s;
  return s;
}

HookState::ScopeStore *resolveStore(HookState::Scope scope,
                                    const std::string &threadId,
                                    const std::string &hookId,
                                    bool createMissing = true) {
  auto &st = inner();
  switch (scope) {
  case HookState::Scope::Global:
    if (!st.global->loaded) {
      st.global->filePath = scopeFilePath(scope, threadId, hookId);
      loadJsonFile(st.global->filePath, st.global->doc);
      st.global->loaded = true;
    }
    return st.global.get();
  case HookState::Scope::Agent:
    if (!st.agent->loaded) {
      st.agent->doc.SetObject();
      st.agent->loaded = true;
    }
    return st.agent.get();
  case HookState::Scope::Thread: {
    if (threadId.empty()) return nullptr;
    auto it = st.threads.find(threadId);
    if (it == st.threads.end()) {
      if (!createMissing) return nullptr;
      auto store = std::make_unique<HookState::ScopeStore>();
      store->filePath = scopeFilePath(scope, threadId, hookId);
      loadJsonFile(store->filePath, store->doc);
      store->loaded = true;
      it = st.threads.emplace(threadId, std::move(store)).first;
    }
    return it->second.get();
  }
  case HookState::Scope::Hook: {
    if (hookId.empty()) return nullptr;
    auto it = st.hooks.find(hookId);
    if (it == st.hooks.end()) {
      if (!createMissing) return nullptr;
      auto store = std::make_unique<HookState::ScopeStore>();
      store->filePath = scopeFilePath(scope, threadId, hookId);
      loadJsonFile(store->filePath, store->doc);
      store->loaded = true;
      it = st.hooks.emplace(hookId, std::move(store)).first;
    }
    return it->second.get();
  }
  }
  return nullptr;
}

void persistIfDirty(HookState::ScopeStore &store) {
  if (!store.dirty) return;
  if (store.filePath.empty()) {
    // Purely in-memory scope (Agent for now). Mark clean so we don't loop.
    store.dirty = false;
    return;
  }
  if (atomicWriteJson(store.filePath, store.doc)) {
    store.dirty = false;
  }
  // On failure we keep dirty=true so the next batch retries the flush.
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// Singleton + lifecycle
// ───────────────────────────────────────────────────────────────────────────

HookState &HookState::instance() {
  static HookState s;
  return s;
}

void HookState::bindThread(const std::string &threadId) {
  std::lock_guard<std::mutex> lock(mu_);
  activeThreadId_ = threadId;
  // Eagerly load so a crash before the first read leaves the on-disk
  // state file with whatever the last batch persisted.
  resolveStore(Scope::Thread, threadId, "");
}

void HookState::unbindThread(const std::string &threadId) {
  std::lock_guard<std::mutex> lock(mu_);
  if (activeThreadId_ == threadId) {
    activeThreadId_.clear();
  }
  auto it = inner().threads.find(threadId);
  if (it != inner().threads.end()) {
    persistIfDirty(*it->second);
    inner().threads.erase(it);
  }
}

// ───────────────────────────────────────────────────────────────────────────
// Read / Write / Append / Delete
// ───────────────────────────────────────────────────────────────────────────

std::optional<std::string> HookState::readJson(Scope scope,
                                               const std::string &path,
                                               const std::string &hookId) const {
  std::lock_guard<std::mutex> lock(mu_);
  bool isAppend = false;
  const std::string ptrStr = toJsonPointer(path, isAppend);
  if (ptrStr.empty()) return std::nullopt;
  rapidjson::Pointer ptr(ptrStr.c_str());
  if (!ptr.IsValid()) return std::nullopt;

  // Cast away const for the lazy-load path; reads do not mutate the doc.
  auto *store = resolveStore(scope, activeThreadId_, hookId,
                             /*createMissing=*/false);
  if (store == nullptr) return std::nullopt;

  const rapidjson::Value *v = ptr.Get(store->doc);
  if (v == nullptr) return std::nullopt;

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  v->Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

bool HookState::writeJson(Scope scope, const std::string &path,
                          const std::string &valueJson,
                          const std::string &hookId) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!hookId.empty() &&
      !HookRegistry::instance().isStateAccessAllowed(hookId, scopeName(scope),
                                                     path)) {
    return false;
  }
  return applyBatchUnlocked({{scope, path, valueJson, /*append=*/false}},
                            hookId);
}

bool HookState::appendJson(Scope scope, const std::string &path,
                           const std::string &valueJson,
                           const std::string &hookId) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!hookId.empty() &&
      !HookRegistry::instance().isStateAccessAllowed(hookId, scopeName(scope),
                                                     path)) {
    return false;
  }
  return applyBatchUnlocked({{scope, path, valueJson, /*append=*/true}},
                            hookId);
}

bool HookState::deleteJson(Scope scope, const std::string &path,
                           const std::string &hookId) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!hookId.empty() &&
      !HookRegistry::instance().isStateAccessAllowed(hookId, scopeName(scope),
                                                     path)) {
    return false;
  }
  bool isAppend = false;
  const std::string ptrStr = toJsonPointer(path, isAppend);
  if (ptrStr.empty() || isAppend) return false;
  rapidjson::Pointer ptr(ptrStr.c_str());
  if (!ptr.IsValid()) return false;

  auto *store = resolveStore(scope, activeThreadId_, hookId);
  if (store == nullptr) return false;

  const bool ok = ptr.Erase(store->doc);
  if (ok) {
    store->dirty = true;
    persistIfDirty(*store);
  }
  return ok;
}

std::string HookState::snapshotJson(const std::string &hookId) const {
  std::lock_guard<std::mutex> lock(mu_);
  rapidjson::Document out;
  out.SetObject();
  auto &alloc = out.GetAllocator();

  auto attach = [&](const char *name, ScopeStore *store) {
    rapidjson::Value v(rapidjson::kObjectType);
    if (store != nullptr) {
      v.CopyFrom(store->doc, alloc);
    }
    out.AddMember(rapidjson::Value(name, alloc).Move(), v, alloc);
  };

  attach("global", resolveStore(Scope::Global, activeThreadId_, hookId, false));
  attach("agent",  resolveStore(Scope::Agent,  activeThreadId_, hookId, false));
  attach("thread", resolveStore(Scope::Thread, activeThreadId_, hookId, false));
  if (!hookId.empty()) {
    attach("hook", resolveStore(Scope::Hook, activeThreadId_, hookId, false));
  } else {
    rapidjson::Value v(rapidjson::kObjectType);
    out.AddMember("hook", v, alloc);
  }

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  out.Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

// ───────────────────────────────────────────────────────────────────────────
// applyBatch — atomic per-scope: all writes for a given scope land in one
// document mutation pass, then a single fsync+rename publishes the update.
// The public entrypoint locks mu_; callers that already hold the mutex use
// applyBatchUnlocked directly to avoid self-recursion.
// ─────────────────────────────────────────────────────────────────────────

bool HookState::applyBatch(const std::vector<BatchWrite> &writes,
                           const std::string &hookId) {
  std::lock_guard<std::mutex> lock(mu_);
  return applyBatchUnlocked(writes, hookId);
}

bool HookState::applyBatchUnlocked(const std::vector<BatchWrite> &writes,
                                   const std::string &hookId) {
  // Collect touched stores so we flush each at most once.
  std::map<ScopeStore *, bool> touched;

  for (const auto &w : writes) {
    if (!hookId.empty() &&
        !HookRegistry::instance().isStateAccessAllowed(
            hookId, scopeName(w.scope), w.path)) {
      return false;
    }
    bool isAppend = w.append;
    std::string ptrStr = toJsonPointer(w.path, isAppend);
    if (ptrStr.empty()) return false;
    rapidjson::Pointer ptr(ptrStr.c_str());
    if (!ptr.IsValid()) return false;

    auto *store = resolveStore(w.scope, activeThreadId_, hookId);
    if (store == nullptr) return false;

    rapidjson::Document parsed(&store->doc.GetAllocator());
    if (parsed.Parse(w.valueJson.c_str()).HasParseError()) {
      // Treat malformed JSON as a literal string. Hook authors who pass
      // bare strings without quotes get sensible "stringified" semantics.
      parsed.SetString(w.valueJson.c_str(), store->doc.GetAllocator());
    }

    if (isAppend) {
      // Get-or-create the array at the pointer, then push the value.
      rapidjson::Value *existing = ptr.Get(store->doc);
      if (existing == nullptr || !existing->IsArray()) {
        rapidjson::Value arr(rapidjson::kArrayType);
        ptr.Set(store->doc, arr);
        existing = ptr.Get(store->doc);
      }
      if (existing != nullptr && existing->IsArray()) {
        existing->PushBack(parsed.Move(), store->doc.GetAllocator());
      }
    } else {
      ptr.Set(store->doc, parsed);
    }
    store->dirty = true;
    touched[store] = true;
  }

  // Single durable flush per touched store. If a flush fails the dirty
  // bit stays set so the next batch retries — readers always see the
  // in-memory truth regardless.
  for (auto &[store, _] : touched) {
    persistIfDirty(*store);
  }
  return true;
}

} // namespace firmius::core::hooks
