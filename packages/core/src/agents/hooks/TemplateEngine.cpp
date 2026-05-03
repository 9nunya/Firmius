#include "agents/hooks/TemplateEngine.hpp"

#include <rapidjson/document.h>
#include <rapidjson/pointer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace firmius::core::hooks {

namespace {

// ─── Dotted path → RFC 6901 JSON Pointer ──────────────────────────────────
// Mirrors HookState's resolver but kept here to avoid leaking that header
// into the public TemplateEngine surface. Trailing `[]` (append) is not
// meaningful in a read context and is rejected.

std::string toPointer(std::string_view path) {
  if (path.empty()) return {};
  std::string out;
  out.reserve(path.size() + 2);
  out.push_back('/');
  for (std::size_t i = 0; i < path.size(); ++i) {
    const char c = path[i];
    if (c == '.') {
      out.push_back('/');
    } else if (c == '[') {
      const auto close = path.find(']', i);
      if (close == std::string_view::npos) return {};
      const auto inner = path.substr(i + 1, close - i - 1);
      if (inner.empty()) return {};  // append meaningless when reading
      out.push_back('/');
      out.append(inner.data(), inner.size());
      i = close;
    } else if (c == '/' || c == '~') {
      out.push_back('~');
      out.push_back(c == '/' ? '1' : '0');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// ─── Builtin generators ───────────────────────────────────────────────────

std::string makeUlid() {
  // Crockford base32, 26 chars: 10 from time (ms since epoch), 16 random.
  static const char *kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  using namespace std::chrono;
  const std::uint64_t ms = static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());

  std::string out(26, '0');
  std::uint64_t t = ms;
  for (int i = 9; i >= 0; --i) {
    out[i] = kAlphabet[t & 0x1F];
    t >>= 5;
  }

  static thread_local std::mt19937_64 rng{
      std::random_device{}() ^
      static_cast<std::uint64_t>(
          system_clock::now().time_since_epoch().count())};
  std::uint64_t lo = rng();
  std::uint64_t hi = rng();
  for (int i = 25; i >= 18; --i) {
    out[i] = kAlphabet[lo & 0x1F];
    lo >>= 5;
  }
  for (int i = 17; i >= 10; --i) {
    out[i] = kAlphabet[hi & 0x1F];
    hi >>= 5;
  }
  return out;
}

std::string makeUuid() {
  static thread_local std::mt19937_64 rng{
      std::random_device{}() ^
      static_cast<std::uint64_t>(
          std::chrono::system_clock::now().time_since_epoch().count())};
  const std::uint64_t a = rng();
  const std::uint64_t b = rng();
  std::ostringstream ss;
  ss << std::hex << std::setfill('0') << std::setw(8) << (a >> 32) << "-"
     << std::setw(4) << ((a >> 16) & 0xFFFF) << "-"
     << std::setw(4) << (0x4000 | (a & 0x0FFF)) << "-"
     << std::setw(4) << (0x8000 | ((b >> 48) & 0x3FFF)) << "-"
     << std::setw(12) << (b & 0xFFFFFFFFFFFFULL);
  return ss.str();
}

std::string makeNowIso() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  std::tm gm{};
#if defined(_WIN32)
  gmtime_s(&gm, &t);
#else
  gmtime_r(&t, &gm);
#endif
  std::ostringstream ss;
  ss << std::put_time(&gm, "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

// ─── JSON value → templated string ────────────────────────────────────────

std::string valueToString(const rapidjson::Value &v) {
  if (v.IsString()) return std::string(v.GetString(), v.GetStringLength());
  if (v.IsBool()) return v.GetBool() ? "true" : "false";
  if (v.IsInt64()) return std::to_string(v.GetInt64());
  if (v.IsDouble()) return std::to_string(v.GetDouble());
  if (v.IsNull()) return {};
  // For arrays/objects, emit JSON.
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  v.Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

// Best-effort JSON → indented YAML serializer for `| yaml` filter. The
// output is meant for human inspection in injected reminders, not
// round-trip parsing.
std::string toYaml(const rapidjson::Value &v, int indent = 0) {
  const std::string pad(indent * 2, ' ');
  if (v.IsObject()) {
    if (v.MemberCount() == 0) return "{}";
    std::string out;
    bool first = true;
    for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
      if (!first) out.push_back('\n');
      first = false;
      out += pad;
      out.append(it->name.GetString(), it->name.GetStringLength());
      out += ": ";
      if (it->value.IsObject() || it->value.IsArray()) {
        out += "\n" + toYaml(it->value, indent + 1);
      } else {
        out += valueToString(it->value);
      }
    }
    return out;
  }
  if (v.IsArray()) {
    if (v.Empty()) return "[]";
    std::string out;
    bool first = true;
    for (const auto &elem : v.GetArray()) {
      if (!first) out.push_back('\n');
      first = false;
      out += pad + "- ";
      if (elem.IsObject() || elem.IsArray()) {
        out += "\n" + toYaml(elem, indent + 1);
      } else {
        out += valueToString(elem);
      }
    }
    return out;
  }
  return valueToString(v);
}

// ─── Resolver: variable path → string ─────────────────────────────────────

const rapidjson::Value *lookup(const rapidjson::Document &doc,
                               const std::string &path) {
  const std::string ptrStr = toPointer(path);
  if (ptrStr.empty()) return nullptr;
  rapidjson::Pointer ptr(ptrStr.c_str());
  if (!ptr.IsValid()) return nullptr;
  return ptr.Get(doc);
}

bool startsWith(const std::string &s, const char *prefix) {
  const std::size_t n = std::strlen(prefix);
  return s.size() >= n && std::strncmp(s.c_str(), prefix, n) == 0;
}

std::string resolveVariable(const std::string &expr,
                            const rapidjson::Document &state,
                            const rapidjson::Document &event,
                            const rapidjson::Document &toolArgs,
                            const rapidjson::Document &subagent,
                            const TemplateContext &ctx,
                            std::string *yamlOverride) {
  // Builtins.
  if (expr == "ulid()") return makeUlid();
  if (expr == "uuid()") return makeUuid();
  if (expr == "now")    return makeNowIso();

  // Flat extras (persona, thread_id, tool, etc.).
  if (auto it = ctx.extras.find(expr); it != ctx.extras.end()) {
    return it->second;
  }

  // Dotted-path lookups against one of the four documents.
  const rapidjson::Value *v = nullptr;
  if (startsWith(expr, "state.")) {
    v = lookup(state, expr.substr(6));
  } else if (startsWith(expr, "event.")) {
    v = lookup(event, expr.substr(6));
  } else if (startsWith(expr, "tool.args.")) {
    v = lookup(toolArgs, expr.substr(10));
  } else if (expr == "tool.args") {
    if (!toolArgs.IsNull()) v = &toolArgs;
  } else if (startsWith(expr, "subagent.return.")) {
    v = lookup(subagent, expr.substr(16));
  } else if (expr == "subagent.return") {
    if (!subagent.IsNull()) v = &subagent;
  }

  if (v == nullptr) return {};
  if (yamlOverride != nullptr) {
    // Caller asked for the raw value pointer for filter-aware rendering.
    *yamlOverride = toYaml(*v);
  }
  return valueToString(*v);
}

// ─── Filter pipeline ──────────────────────────────────────────────────────

std::string applyFilter(std::string current, const std::string &filterExpr,
                        const std::string &yamlForm) {
  // Trim leading/trailing whitespace.
  auto trim = [](std::string &s) {
    const auto l = s.find_first_not_of(" \t");
    const auto r = s.find_last_not_of(" \t");
    if (l == std::string::npos) {
      s.clear();
    } else {
      s = s.substr(l, r - l + 1);
    }
  };
  std::string f = filterExpr;
  trim(f);

  // `default: 'fallback'`
  if (startsWith(f, "default")) {
    if (!current.empty()) return current;
    const auto colon = f.find(':');
    if (colon == std::string::npos) return {};
    std::string tail = f.substr(colon + 1);
    trim(tail);
    if (tail.size() >= 2 &&
        ((tail.front() == '\'' && tail.back() == '\'') ||
         (tail.front() == '"' && tail.back() == '"'))) {
      tail = tail.substr(1, tail.size() - 2);
    }
    return tail;
  }

  // `yaml` — render the source value as indented YAML.
  if (f == "yaml") {
    return yamlForm.empty() ? current : yamlForm;
  }

  // `length` — string/array length.
  if (f == "length") {
    return std::to_string(current.size());
  }

  // Unknown filter — pass through.
  return current;
}

} // namespace

TemplateContext makeTemplateContext(const std::string &stateJson,
                                    const std::string &eventJson,
                                    const std::string &toolArgsJson,
                                    const std::string &subagentReturnJson,
                                    std::map<std::string, std::string> extras) {
  TemplateContext ctx;
  ctx.stateJson = stateJson.empty() ? "{}" : stateJson;
  ctx.eventJson = eventJson.empty() ? "{}" : eventJson;
  ctx.toolArgsJson = toolArgsJson.empty() ? "{}" : toolArgsJson;
  ctx.subagentReturnJson =
      subagentReturnJson.empty() ? "{}" : subagentReturnJson;
  ctx.extras = std::move(extras);
  return ctx;
}

std::string renderTemplate(const std::string &body, const TemplateContext &ctx) {
  if (body.empty()) return body;

  rapidjson::Document state, event, toolArgs, subagent;
  state.Parse(ctx.stateJson.c_str());
  event.Parse(ctx.eventJson.c_str());
  toolArgs.Parse(ctx.toolArgsJson.c_str());
  subagent.Parse(ctx.subagentReturnJson.c_str());

  std::string out;
  out.reserve(body.size());
  std::size_t i = 0;
  while (i < body.size()) {
    if (i + 1 < body.size() && body[i] == '{' && body[i + 1] == '{') {
      const auto end = body.find("}}", i + 2);
      if (end == std::string::npos) {
        out.append(body.substr(i));
        break;
      }
      std::string inner = body.substr(i + 2, end - (i + 2));
      // Trim.
      const auto l = inner.find_first_not_of(" \t");
      const auto r = inner.find_last_not_of(" \t");
      if (l == std::string::npos) {
        i = end + 2;
        continue;
      }
      inner = inner.substr(l, r - l + 1);

      // Split by `|` for filters.
      std::string varExpr = inner;
      std::vector<std::string> filters;
      const auto pipe = inner.find('|');
      if (pipe != std::string::npos) {
        varExpr = inner.substr(0, pipe);
        // Trim
        const auto vl = varExpr.find_first_not_of(" \t");
        const auto vr = varExpr.find_last_not_of(" \t");
        if (vl != std::string::npos) {
          varExpr = varExpr.substr(vl, vr - vl + 1);
        }
        std::string rest = inner.substr(pipe + 1);
        // Split by remaining `|`.
        std::size_t cursor = 0;
        while (cursor <= rest.size()) {
          const auto next = rest.find('|', cursor);
          const std::string seg = rest.substr(
              cursor, next == std::string::npos ? std::string::npos
                                                : next - cursor);
          filters.push_back(seg);
          if (next == std::string::npos) break;
          cursor = next + 1;
        }
      }

      std::string yamlForm;
      std::string value = resolveVariable(varExpr, state, event, toolArgs,
                                          subagent, ctx, &yamlForm);
      for (const auto &f : filters) {
        value = applyFilter(value, f, yamlForm);
      }
      out.append(value);
      i = end + 2;
    } else {
      out.push_back(body[i]);
      ++i;
    }
  }
  return out;
}

} // namespace firmius::core::hooks
