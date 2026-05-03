#include "providers/WindsurfModels.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>

namespace firmius::provider::windsurf {

namespace {

// ============================================================================
// Windsurf protobuf model enum (extracted from extension.js, verbatim from
// rsvedant/opencode-windsurf-auth pr #9 /src/plugin/types.ts).
// ============================================================================

// Each row is (ENUM_NAME, ENUM_VALUE). Drives both the int constants
// (`E::CLAUDE_4_5_SONNET = 353`) and the int→string reverse lookup the wire
// layer needs (Windsurf LSP/cloud expect model UIDs in SCREAMING_CAPS form
// matching the proto enum names — e.g. `CLAUDE_4_5_SONNET`,
// `GPT_4O_2024_08_06` — verified live against the running language_server
// and cross-checked against opencode-windsurf-auth's published API constants).
#define FIRMIUS_WINDSURF_MODEL_ENUMS(X)                                        \
  X(CLAUDE_3_OPUS_20240229, 63)                                                \
  X(CLAUDE_3_SONNET_20240229, 64)                                              \
  X(CLAUDE_3_HAIKU_20240307, 172)                                              \
  X(CLAUDE_3_5_SONNET_20240620, 80)                                            \
  X(CLAUDE_3_5_SONNET_20241022, 166)                                           \
  X(CLAUDE_3_5_HAIKU_20241022, 171)                                            \
  X(CLAUDE_3_7_SONNET_20250219, 226)                                           \
  X(CLAUDE_3_7_SONNET_20250219_THINKING, 227)                                  \
  X(CLAUDE_4_OPUS, 290)                                                        \
  X(CLAUDE_4_OPUS_THINKING, 291)                                               \
  X(CLAUDE_4_SONNET, 281)                                                      \
  X(CLAUDE_4_SONNET_THINKING, 282)                                             \
  X(CLAUDE_4_1_OPUS, 328)                                                      \
  X(CLAUDE_4_1_OPUS_THINKING, 329)                                             \
  X(CLAUDE_4_5_SONNET, 353)                                                    \
  X(CLAUDE_4_5_SONNET_THINKING, 354)                                           \
  X(CLAUDE_4_5_OPUS, 391)                                                      \
  X(CLAUDE_4_5_OPUS_THINKING, 392)                                             \
  X(CLAUDE_CODE, 344)                                                          \
  X(GPT_4, 30)                                                                 \
  X(GPT_4_1106_PREVIEW, 37)                                                    \
  X(GPT_4O_2024_08_06, 109)                                                    \
  X(GPT_4O_MINI_2024_07_18, 113)                                               \
  X(GPT_4_1_2025_04_14, 259)                                                   \
  X(GPT_4_1_MINI_2025_04_14, 260)                                              \
  X(GPT_4_1_NANO_2025_04_14, 261)                                              \
  X(GPT_5_NANO, 337)                                                           \
  X(GPT_5_LOW, 339)                                                            \
  X(GPT_5, 340)                                                                \
  X(GPT_5_HIGH, 341)                                                           \
  X(GPT_5_CODEX, 346)                                                          \
  X(GPT_5_1_CODEX_MINI_LOW, 385)                                               \
  X(GPT_5_1_CODEX_MINI_MEDIUM, 386)                                            \
  X(GPT_5_1_CODEX_MINI_HIGH, 387)                                              \
  X(GPT_5_1_CODEX_LOW, 388)                                                    \
  X(GPT_5_1_CODEX_MEDIUM, 389)                                                 \
  X(GPT_5_1_CODEX_HIGH, 390)                                                   \
  X(GPT_5_1_CODEX_MAX_LOW, 395)                                                \
  X(GPT_5_1_CODEX_MAX_MEDIUM, 396)                                             \
  X(GPT_5_1_CODEX_MAX_HIGH, 397)                                               \
  X(GPT_5_2_LOW, 400)                                                          \
  X(GPT_5_2_MEDIUM, 401)                                                       \
  X(GPT_5_2_HIGH, 402)                                                         \
  X(GPT_5_2_XHIGH, 403)                                                        \
  X(GPT_5_2_LOW_PRIORITY, 405)                                                 \
  X(GPT_5_2_MEDIUM_PRIORITY, 406)                                              \
  X(GPT_5_2_HIGH_PRIORITY, 407)                                                \
  X(GPT_5_2_XHIGH_PRIORITY, 408)                                               \
  X(O3, 218)                                                                   \
  X(O3_MINI, 207)                                                              \
  X(O3_LOW, 262)                                                               \
  X(O3_HIGH, 263)                                                              \
  X(O3_PRO, 294)                                                               \
  X(O3_PRO_LOW, 295)                                                           \
  X(O3_PRO_HIGH, 296)                                                          \
  X(O4_MINI, 264)                                                              \
  X(O4_MINI_LOW, 265)                                                          \
  X(O4_MINI_HIGH, 266)                                                         \
  X(GEMINI_2_0_FLASH, 184)                                                     \
  X(GEMINI_2_5_PRO, 246)                                                       \
  X(GEMINI_2_5_FLASH, 312)                                                     \
  X(GEMINI_2_5_FLASH_THINKING, 313)                                            \
  X(GEMINI_2_5_FLASH_LITE, 343)                                                \
  X(GEMINI_3_0_PRO_LOW, 378)                                                   \
  X(GEMINI_3_0_PRO_HIGH, 379)                                                  \
  X(GEMINI_3_0_PRO_MINIMAL, 411)                                               \
  X(GEMINI_3_0_PRO_MEDIUM, 412)                                                \
  X(GEMINI_3_0_FLASH_MINIMAL, 413)                                             \
  X(GEMINI_3_0_FLASH_LOW, 414)                                                 \
  X(GEMINI_3_0_FLASH_MEDIUM, 415)                                              \
  X(GEMINI_3_0_FLASH_HIGH, 416)                                                \
  X(DEEPSEEK_V3, 205)                                                          \
  X(DEEPSEEK_R1, 206)                                                          \
  X(DEEPSEEK_V3_2, 409)                                                        \
  X(QWEN_3_CODER_480B_INSTRUCT, 325)                                           \
  X(QWEN_3_CODER_480B_INSTRUCT_FAST, 327)                                      \
  X(GROK_3, 217)                                                               \
  X(GROK_3_MINI_REASONING, 234)                                                \
  X(GROK_CODE_FAST, 345)                                                       \
  X(KIMI_K2, 323)                                                              \
  X(KIMI_K2_THINKING, 394)                                                     \
  X(GLM_4_5, 342)                                                              \
  X(GLM_4_6, 356)                                                              \
  X(GLM_4_7, 417)                                                              \
  X(MINIMAX_M2, 368)                                                           \
  X(MINIMAX_M2_1, 419)                                                         \
  X(SWE_1_5, 359)                                                              \
  X(SWE_1_5_THINKING, 369)

namespace E {
#define FIRMIUS_DECL_INT(name, value) [[maybe_unused]] constexpr int name = value;
FIRMIUS_WINDSURF_MODEL_ENUMS(FIRMIUS_DECL_INT)
#undef FIRMIUS_DECL_INT
} // namespace E

// Reverse-lookup: enum int → SCREAMING_CAPS wire-name string. Returns empty
// view when the value is unknown (cloud-discovered models that don't appear
// in the static catalog hit this path; their wireUid comes from the live
// `ClientModelConfig.model_uid` in fetchAndMergeModels instead).
std::string_view wireUidFromEnumValue(int enumValue) {
  switch (enumValue) {
#define FIRMIUS_CASE(name, value)                                              \
  case value:                                                                  \
    return #name;
    FIRMIUS_WINDSURF_MODEL_ENUMS(FIRMIUS_CASE)
#undef FIRMIUS_CASE
    default:
      return {};
  }
}

// ============================================================================
// Variant catalog: canonical id → default enum + optional variants
// ============================================================================

struct VariantEntry {
  std::string key;          // e.g. "thinking", "high"
  std::string description;  // human-readable description
  int enumValue = 0;
};

struct CatalogEntry {
  std::string canonicalId;
  int defaultEnum = 0;
  std::uint32_t contextWindow = 200000;
  std::uint32_t maxOutput = 8192;
  bool supportsReasoning = false;
  std::vector<std::string> aliases;
  std::vector<VariantEntry> variants;
};

const std::vector<CatalogEntry> &catalog() {
  static const std::vector<CatalogEntry> kCatalog = {
      // ---- Claude family ----
      {"claude-3.5-sonnet",
       E::CLAUDE_3_5_SONNET_20241022,
       200000,
       8192,
       false,
       {"claude-3-5-sonnet"},
       {}},
      {"claude-3.5-haiku",
       E::CLAUDE_3_5_HAIKU_20241022,
       200000,
       8192,
       false,
       {"claude-3-5-haiku"},
       {}},
      {"claude-3.7-sonnet",
       E::CLAUDE_3_7_SONNET_20250219,
       200000,
       8192,
       true,
       {"claude-3-7-sonnet"},
       {{"thinking", "Extended thinking",
         E::CLAUDE_3_7_SONNET_20250219_THINKING}}},
      {"claude-4-sonnet",
       E::CLAUDE_4_SONNET,
       200000,
       8192,
       true,
       {},
       {{"thinking", "Extended thinking", E::CLAUDE_4_SONNET_THINKING}}},
      {"claude-4-opus",
       E::CLAUDE_4_OPUS,
       200000,
       8192,
       true,
       {},
       {{"thinking", "Extended thinking", E::CLAUDE_4_OPUS_THINKING}}},
      {"claude-4.1-opus",
       E::CLAUDE_4_1_OPUS,
       200000,
       8192,
       true,
       {"claude-4-1-opus"},
       {{"thinking", "Extended thinking", E::CLAUDE_4_1_OPUS_THINKING}}},
      {"claude-4.5-sonnet",
       E::CLAUDE_4_5_SONNET,
       200000,
       8192,
       true,
       {"claude-4-5-sonnet"},
       {{"thinking", "Extended thinking", E::CLAUDE_4_5_SONNET_THINKING}}},
      {"claude-4.5-opus",
       E::CLAUDE_4_5_OPUS,
       200000,
       8192,
       true,
       {"claude-4-5-opus"},
       {{"thinking", "Extended thinking", E::CLAUDE_4_5_OPUS_THINKING}}},
      {"claude-code", E::CLAUDE_CODE, 200000, 8192, false, {}, {}},

      // ---- GPT family ----
      {"gpt-4o", E::GPT_4O_2024_08_06, 128000, 16384, false, {}, {}},
      {"gpt-4o-mini", E::GPT_4O_MINI_2024_07_18, 128000, 16384, false, {}, {}},
      {"gpt-4.1",
       E::GPT_4_1_2025_04_14,
       1047576,
       32768,
       false,
       {"gpt-4-1"},
       {}},
      {"gpt-4.1-mini",
       E::GPT_4_1_MINI_2025_04_14,
       1047576,
       32768,
       false,
       {"gpt-4-1-mini"},
       {}},
      {"gpt-4.1-nano",
       E::GPT_4_1_NANO_2025_04_14,
       1047576,
       32768,
       false,
       {"gpt-4-1-nano"},
       {}},
      {"gpt-5",
       E::GPT_5,
       400000,
       32768,
       true,
       {},
       {{"low", "Lower reasoning effort", E::GPT_5_LOW},
        {"high", "Higher reasoning effort", E::GPT_5_HIGH},
        {"nano", "Smallest GPT-5 variant", E::GPT_5_NANO}}},
      {"gpt-5-codex", E::GPT_5_CODEX, 400000, 32768, true, {}, {}},
      {"gpt-5.1-codex-mini",
       E::GPT_5_1_CODEX_MINI_MEDIUM,
       400000,
       32768,
       true,
       {"gpt-5-1-codex-mini"},
       {{"low", "Low effort", E::GPT_5_1_CODEX_MINI_LOW},
        {"medium", "Balanced (default)", E::GPT_5_1_CODEX_MINI_MEDIUM},
        {"high", "High effort", E::GPT_5_1_CODEX_MINI_HIGH}}},
      {"gpt-5.1-codex",
       E::GPT_5_1_CODEX_MEDIUM,
       400000,
       32768,
       true,
       {"gpt-5-1-codex"},
       {{"low", "Low effort", E::GPT_5_1_CODEX_LOW},
        {"medium", "Balanced (default)", E::GPT_5_1_CODEX_MEDIUM},
        {"high", "High effort", E::GPT_5_1_CODEX_HIGH}}},
      {"gpt-5.1-codex-max",
       E::GPT_5_1_CODEX_MAX_MEDIUM,
       400000,
       32768,
       true,
       {"gpt-5-1-codex-max"},
       {{"low", "Low effort", E::GPT_5_1_CODEX_MAX_LOW},
        {"medium", "Balanced (default)", E::GPT_5_1_CODEX_MAX_MEDIUM},
        {"high", "High effort", E::GPT_5_1_CODEX_MAX_HIGH}}},
      {"gpt-5.2",
       E::GPT_5_2_MEDIUM,
       400000,
       32768,
       true,
       {"gpt-5-2"},
       {{"low", "Low effort", E::GPT_5_2_LOW},
        {"medium", "Balanced (default)", E::GPT_5_2_MEDIUM},
        {"high", "High effort", E::GPT_5_2_HIGH},
        {"xhigh", "Maximum effort", E::GPT_5_2_XHIGH},
        {"low-priority", "Priority routing (low)", E::GPT_5_2_LOW_PRIORITY},
        {"priority", "Priority routing (medium)", E::GPT_5_2_MEDIUM_PRIORITY},
        {"high-priority", "Priority routing (high)", E::GPT_5_2_HIGH_PRIORITY},
        {"xhigh-priority", "Priority routing (xhigh)",
         E::GPT_5_2_XHIGH_PRIORITY}}},

      // ---- O-series (reasoning) ----
      {"o3",
       E::O3,
       200000,
       16384,
       true,
       {},
       {{"low", "Low effort", E::O3_LOW},
        {"high", "High effort", E::O3_HIGH}}},
      {"o3-pro",
       E::O3_PRO,
       200000,
       16384,
       true,
       {},
       {{"low", "Low effort", E::O3_PRO_LOW},
        {"high", "High effort", E::O3_PRO_HIGH}}},
      {"o4-mini",
       E::O4_MINI,
       200000,
       16384,
       true,
       {},
       {{"low", "Low effort", E::O4_MINI_LOW},
        {"high", "High effort", E::O4_MINI_HIGH}}},

      // ---- Gemini ----
      {"gemini-2.0-flash",
       E::GEMINI_2_0_FLASH,
       1048576,
       8192,
       false,
       {"gemini-2-0-flash"},
       {}},
      {"gemini-2.5-pro",
       E::GEMINI_2_5_PRO,
       2097152,
       8192,
       true,
       {"gemini-2-5-pro"},
       {}},
      {"gemini-2.5-flash",
       E::GEMINI_2_5_FLASH,
       1048576,
       8192,
       true,
       {"gemini-2-5-flash"},
       {{"thinking", "Thinking budget enabled",
         E::GEMINI_2_5_FLASH_THINKING},
        {"lite", "Lite (lower cost)", E::GEMINI_2_5_FLASH_LITE}}},
      {"gemini-3.0-pro",
       E::GEMINI_3_0_PRO_MEDIUM,
       2097152,
       8192,
       true,
       {"gemini-3-0-pro"},
       {{"minimal", "Cheapest, least reasoning", E::GEMINI_3_0_PRO_MINIMAL},
        {"low", "Lower cost", E::GEMINI_3_0_PRO_LOW},
        {"medium", "Balanced (default)", E::GEMINI_3_0_PRO_MEDIUM},
        {"high", "Higher reasoning budget", E::GEMINI_3_0_PRO_HIGH}}},
      {"gemini-3.0-flash",
       E::GEMINI_3_0_FLASH_MEDIUM,
       1048576,
       8192,
       true,
       {"gemini-3-0-flash"},
       {{"minimal", "Cheapest", E::GEMINI_3_0_FLASH_MINIMAL},
        {"low", "Low budget", E::GEMINI_3_0_FLASH_LOW},
        {"medium", "Balanced (default)", E::GEMINI_3_0_FLASH_MEDIUM},
        {"high", "Higher budget", E::GEMINI_3_0_FLASH_HIGH}}},

      // ---- Other ----
      {"deepseek-v3", E::DEEPSEEK_V3, 64000, 8192, false, {}, {}},
      {"deepseek-v3.2",
       E::DEEPSEEK_V3_2,
       64000,
       8192,
       false,
       {"deepseek-v3-2"},
       {}},
      {"deepseek-r1", E::DEEPSEEK_R1, 64000, 8192, true, {}, {}},
      {"qwen-3-coder",
       E::QWEN_3_CODER_480B_INSTRUCT,
       262144,
       8192,
       false,
       {"qwen-3-coder-480b"},
       {{"fast", "Fast tier", E::QWEN_3_CODER_480B_INSTRUCT_FAST}}},
      {"grok-3", E::GROK_3, 131072, 8192, false, {}, {}},
      {"grok-3-mini",
       E::GROK_3_MINI_REASONING, 131072, 8192, true, {}, {}},
      {"grok-code-fast", E::GROK_CODE_FAST, 131072, 8192, false, {}, {}},
      {"kimi-k2",
       E::KIMI_K2,
       200000,
       8192,
       true,
       {},
       {{"thinking", "Extended thinking", E::KIMI_K2_THINKING}}},
      {"glm-4.5", E::GLM_4_5, 128000, 8192, false, {"glm-4-5"}, {}},
      {"glm-4.6", E::GLM_4_6, 128000, 8192, false, {"glm-4-6"}, {}},
      {"glm-4.7", E::GLM_4_7, 128000, 8192, false, {"glm-4-7"}, {}},
      {"minimax-m2", E::MINIMAX_M2, 200000, 8192, false, {}, {}},
      {"minimax-m2.1",
       E::MINIMAX_M2_1, 200000, 8192, false, {"minimax-m2-1"}, {}},
      {"swe-1.5",
       E::SWE_1_5,
       200000,
       8192,
       true,
       {"swe-1-5"},
       {{"thinking", "Extended thinking", E::SWE_1_5_THINKING}}},
  };
  return kCatalog;
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.pop_back();
  return s;
}

// alias → canonical id (lowercased keys)
const std::unordered_map<std::string, std::string> &aliasMap() {
  static const auto kMap = []() {
    std::unordered_map<std::string, std::string> m;
    for (const auto &e : catalog()) {
      m[toLower(e.canonicalId)] = e.canonicalId;
      for (const auto &a : e.aliases) {
        m[toLower(a)] = e.canonicalId;
      }
    }
    return m;
  }();
  return kMap;
}

const CatalogEntry *findByCanonical(const std::string &canonicalId) {
  for (const auto &e : catalog()) {
    if (e.canonicalId == canonicalId) {
      return &e;
    }
  }
  return nullptr;
}

// Split user-supplied id into base + variant. Accepts "x:variant" or
// "x-variant" only when "variant" matches a known variant key for x.
std::pair<std::string, std::string> splitVariant(const std::string &raw) {
  std::string lower = toLower(trim(raw));

  // colon syntax wins
  auto colon = lower.find(':');
  if (colon != std::string::npos) {
    return {lower.substr(0, colon), trim(lower.substr(colon + 1))};
  }

  // Try suffix-as-variant: walk from rightmost dash and see if any prefix is a
  // known canonical id with that suffix as a registered variant.
  std::size_t dash = lower.size();
  while ((dash = lower.find_last_of('-', dash - 1)) != std::string::npos) {
    std::string base = lower.substr(0, dash);
    std::string suffix = lower.substr(dash + 1);
    auto it = aliasMap().find(base);
    if (it == aliasMap().end()) {
      if (dash == 0) break;
      continue;
    }
    const auto *entry = findByCanonical(it->second);
    if (!entry) {
      if (dash == 0) break;
      continue;
    }
    for (const auto &v : entry->variants) {
      if (v.key == suffix) {
        return {base, suffix};
      }
    }
    // some compound suffixes like "low-priority" — try two-segment suffix
    auto innerDash = lower.find_last_of('-', dash - 1);
    if (innerDash != std::string::npos) {
      std::string base2 = lower.substr(0, innerDash);
      std::string suffix2 = lower.substr(innerDash + 1);
      auto it2 = aliasMap().find(base2);
      if (it2 != aliasMap().end()) {
        const auto *entry2 = findByCanonical(it2->second);
        if (entry2) {
          for (const auto &v : entry2->variants) {
            if (v.key == suffix2) {
              return {base2, suffix2};
            }
          }
        }
      }
    }
    if (dash == 0) break;
  }

  return {lower, ""};
}

} // namespace

std::optional<ResolvedModel> resolveModel(
    const std::string &modelId,
    const std::optional<std::string> &variantHint) {
  auto [base, variant] = splitVariant(modelId);
  if (variantHint && !variantHint->empty()) {
    variant = toLower(trim(*variantHint));
  }

  auto it = aliasMap().find(base);
  if (it == aliasMap().end()) {
    return std::nullopt;
  }
  const auto *entry = findByCanonical(it->second);
  if (!entry) {
    return std::nullopt;
  }

  // The Windsurf LSP/cloud expects model UIDs in SCREAMING_CAPS form matching
  // the `Model` enum names (e.g. `CLAUDE_4_5_SONNET`, `GPT_4O_2024_08_06`,
  // `CLAUDE_4_5_SONNET_THINKING` for the thinking variant). The variant's own
  // enum value already encodes the variant suffix, so we look up the wire UID
  // off whichever enum value we're going to send (default vs variant-specific).
  auto wireUidFor = [&](int enumValue) -> std::string {
    auto sv = wireUidFromEnumValue(enumValue);
    return std::string(sv);
  };

  ResolvedModel r;
  r.canonicalId = entry->canonicalId;
  r.supportsThinkingVariant = false;
  for (const auto &v : entry->variants) {
    if (v.key == "thinking") {
      r.supportsThinkingVariant = true;
      break;
    }
  }

  if (!variant.empty()) {
    for (const auto &v : entry->variants) {
      if (v.key == variant) {
        r.variant = v.key;
        r.enumValue = v.enumValue;
        r.wireModelName = wireUidFor(v.enumValue);
        if (r.wireModelName.empty()) {
          r.wireModelName = entry->canonicalId + ":" + v.key;
        }
        return r;
      }
    }
    // Unknown variant — fall through to default but keep variant string for
    // wire transmission so the server can decide what to do.
    r.variant = "";
    r.enumValue = entry->defaultEnum;
    r.wireModelName = wireUidFor(entry->defaultEnum);
    if (r.wireModelName.empty()) r.wireModelName = entry->canonicalId;
    return r;
  }

  r.enumValue = entry->defaultEnum;
  r.wireModelName = wireUidFor(entry->defaultEnum);
  if (r.wireModelName.empty()) r.wireModelName = entry->canonicalId;
  return r;
}

std::optional<firmius::shared::ModelInfo> getModelInfo(
    const std::string &canonicalId) {
  auto it = aliasMap().find(toLower(canonicalId));
  if (it == aliasMap().end()) {
    return std::nullopt;
  }
  const auto *entry = findByCanonical(it->second);
  if (!entry) {
    return std::nullopt;
  }

  firmius::shared::ModelInfo info;
  info.id = entry->canonicalId;
  info.provider = "windsurf";
  info.contextWindow = entry->contextWindow;
  info.maxOutputTokens = entry->maxOutput;
  info.modalities = {"text"};
  info.supportsReasoning = entry->supportsReasoning;
  for (const auto &v : entry->variants) {
    firmius::shared::ModelVariant mv;
    mv.variantName = v.key;
    mv.extraMetadataJson = "";
    info.variants.push_back(std::move(mv));
  }
  return info;
}

std::vector<firmius::shared::ModelInfo> listAllModels() {
  std::vector<firmius::shared::ModelInfo> out;
  out.reserve(catalog().size());
  for (const auto &entry : catalog()) {
    if (auto info = getModelInfo(entry.canonicalId)) {
      out.push_back(*info);
    }
  }
  return out;
}

std::string getDefaultModel() { return "claude-4.5-sonnet"; }

std::string canonicalIdFromEnum(int enumValue) {
  for (const auto &e : catalog()) {
    if (e.defaultEnum == enumValue) return e.canonicalId;
    for (const auto &v : e.variants) {
      if (v.enumValue == enumValue) return e.canonicalId;
    }
  }
  return "";
}

} // namespace firmius::provider::windsurf
