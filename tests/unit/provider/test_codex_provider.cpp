#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "providers/CodexProvider.hpp"

using firmius::provider::CodexProvider;
using firmius::shared::AgentMetrics;
using firmius::shared::OAuthAccount;
using firmius::shared::StreamEvent;
using firmius::shared::ToolCall;
using firmius::shared::ToolCallChunk;

namespace firmius::provider {

class CodexProviderTestAccessor {
public:
  using ToolCallTracker = CodexProvider::ToolCallTracker;

  static void processSseLine(CodexProvider &provider, const std::string &line,
                             std::function<void(const StreamEvent &)> &onEvent,
                             AgentMetrics &metrics, bool &metricsReceived,
                             bool &doneReceived, ToolCallTracker &tracker) {
    provider.processSseLine(line, onEvent, metrics, metricsReceived,
                            doneReceived, tracker);
  }
};

} // namespace firmius::provider

namespace {

using firmius::provider::CodexProviderTestAccessor;

bool containsModel(const std::vector<firmius::shared::ModelInfo> &models,
                   const std::string &id) {
  for (const auto &model : models) {
    if (model.id == id) {
      return true;
    }
  }
  return false;
}

std::string base64UrlEncode(const std::string &input) {
  static const char *chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  int val = 0;
  int valb = -6;
  for (unsigned char c : input) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(chars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) {
    out.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
  }
  return out;
}

std::string makeJwt(const std::string &payloadJson) {
  const std::string header = R"({"alg":"none","typ":"JWT"})";
  return base64UrlEncode(header) + "." + base64UrlEncode(payloadJson) + ".sig";
}

class ScopedHomeOverride {
public:
  explicit ScopedHomeOverride(const std::filesystem::path &home)
      : hadHome_(std::getenv("HOME") != nullptr),
        originalHome_(hadHome_ ? std::getenv("HOME") : "") {
    setenv("HOME", home.c_str(), 1);
  }

  ~ScopedHomeOverride() {
    if (hadHome_) {
      setenv("HOME", originalHome_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
  }

private:
  bool hadHome_ = false;
  std::string originalHome_;
};

std::string readFile(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

} // namespace

TEST(CodexProvider, StaticModelCatalogIncludesNewCodexUiModels) {
  CodexProvider provider;
  const auto models = provider.listModels();

  EXPECT_EQ(models.size(), 9u);
  EXPECT_TRUE(containsModel(models, "gpt-5.4"));
  EXPECT_TRUE(containsModel(models, "gpt-5.4-mini"));
  EXPECT_TRUE(containsModel(models, "gpt-5.3-codex"));
  EXPECT_TRUE(containsModel(models, "gpt-5.2-codex"));
  EXPECT_TRUE(containsModel(models, "gpt-5.2"));
  EXPECT_TRUE(containsModel(models, "gpt-5.1-codex-max"));
  EXPECT_TRUE(containsModel(models, "gpt-5.1-codex-mini"));
}

TEST(CodexProvider, ModelInfoNormalizesAndPreservesVariantMetadata) {
  CodexProvider provider;

  const auto gpt54 = provider.getModelInfo("openai/gpt-5.4");
  EXPECT_EQ(gpt54.id, "gpt-5.4");
  EXPECT_EQ(gpt54.contextWindow, 1000000u);
  ASSERT_EQ(gpt54.variants.size(), 5u);
  EXPECT_EQ(gpt54.variants.front().variantName, "none");
  EXPECT_EQ(gpt54.variants.back().variantName, "xhigh");

  const auto gpt54Mini = provider.getModelInfo("codex/gpt-5.4-mini");
  EXPECT_EQ(gpt54Mini.id, "gpt-5.4-mini");
  ASSERT_EQ(gpt54Mini.variants.size(), 2u);
  EXPECT_EQ(gpt54Mini.variants.front().variantName, "medium");
  EXPECT_EQ(gpt54Mini.variants.back().variantName, "high");

  const auto gpt53Codex = provider.getModelInfo("chatgpt/gpt-5.3-codex");
  EXPECT_EQ(gpt53Codex.id, "gpt-5.3-codex");
  ASSERT_EQ(gpt53Codex.variants.size(), 3u);
  EXPECT_EQ(gpt53Codex.variants.front().variantName, "low");
  EXPECT_EQ(gpt53Codex.variants.back().variantName, "high");
}

TEST(CodexProvider,
     MigratesUuidIdentifiersToEmailsAndPrunesLegacyModelQuotaArtifacts) {
  const auto tempHome =
      std::filesystem::temp_directory_path() / "firmius_codex_test_home";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  constexpr auto kAccountId = "42eac8d8-0048-4446-8a5c-28e10d1d7284";
  constexpr auto kEmail = "blackwingbro@gmail.com";
  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::string accessToken = makeJwt(
      std::string("{\"https://api.openai.com/profile\":{\"email\":\"") +
      kEmail +
      "\"},\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"" +
      kAccountId + "\"}}");

  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"codex":[{"identifier":")" << kAccountId
      << R"(","refreshToken":"refresh-token","accessToken":")" << accessToken
      << R"(","tokenExpiration":)" << futureSeconds
      << R"(,"lastQuotaRefresh":)" << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":")" << kAccountId
      << R"(","quota:codex":"0.4","quota:gpt-5.4":"0.1","quota:gpt-5.4-mini":"0.2","quota_reset:codex":"1774834810","quota_reset:gpt-5.4":"1774834810"}}]})";
  out.close();

  CodexProvider provider;
  const auto &accounts = provider.getAccounts();
  ASSERT_EQ(accounts.size(), 1u);
  const OAuthAccount &account = accounts.front();

  EXPECT_EQ(account.identifier, kEmail);
  ASSERT_TRUE(account.metadata.count("email"));
  EXPECT_EQ(account.metadata.at("email"), kEmail);
  EXPECT_FALSE(account.metadata.count("quota:gpt-5.4"));
  EXPECT_FALSE(account.metadata.count("quota:gpt-5.4-mini"));
  EXPECT_FALSE(account.metadata.count("quota_reset:gpt-5.4"));
  ASSERT_TRUE(account.metadata.count("quota_reset:codex"));
  EXPECT_NE(account.metadata.at("quota_reset:codex").find('T'),
            std::string::npos);

  const auto quotas = provider.getAllQuotas();
  ASSERT_EQ(quotas.size(), 1u);
  auto quotaIt = quotas.find(kEmail);
  ASSERT_NE(quotaIt, quotas.end());
  ASSERT_EQ(quotaIt->second.size(), 1u);
  EXPECT_EQ(quotaIt->second.front().name, "codex");
  EXPECT_NE(quotaIt->second.front().resetTime.find('T'), std::string::npos);

  const std::string saved = readFile(oauthPath);
  EXPECT_NE(saved.find(kEmail), std::string::npos);
  EXPECT_EQ(saved.find("quota:gpt-5.4"), std::string::npos);
  EXPECT_EQ(saved.find("quota:gpt-5.4-mini"), std::string::npos);
}

TEST(CodexProvider, AvailableAccountUsesHighestPositiveCodexQuota) {
  const auto tempHome =
      std::filesystem::temp_directory_path() / "firmius_codex_quota_pick_home";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"codex":[)"
      << R"({"identifier":"low@example.com","refreshToken":"r1","accessToken":"a1","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:codex":"0.2","chatgpt_account_id":"id-low"}},)"
      << R"({"identifier":"zero@example.com","refreshToken":"r2","accessToken":"a2","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:codex":"0","chatgpt_account_id":"id-zero"}},)"
      << R"({"identifier":"high@example.com","refreshToken":"r3","accessToken":"a3","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:codex":"0.9","chatgpt_account_id":"id-high"}}]})";
  out.close();

  CodexProvider provider;
  auto selected = provider.getAvailableAccount(std::string("gpt-5.2-codex"));
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->getIdentifier(), "high@example.com");
}

TEST(CodexProvider, AvailableAccountRequiresPositiveCodexQuota) {
  const auto tempHome =
      std::filesystem::temp_directory_path() / "firmius_codex_quota_none_home";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"codex":[)"
      << R"({"identifier":"none@example.com","refreshToken":"r1","accessToken":"a1","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-none"}},)"
      << R"({"identifier":"zero@example.com","refreshToken":"r2","accessToken":"a2","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:codex":"0","chatgpt_account_id":"id-zero"}}]})";
  out.close();

  CodexProvider provider;
  auto selected = provider.getAvailableAccount(std::string("gpt-5.2-codex"));
  EXPECT_FALSE(selected.has_value());
}

TEST(CodexProvider, StructuredWindowsExposePlanTierAndBlockWeeklyExhaustion) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_codex_structured_windows_home";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::string accessToken = makeJwt(
      R"({"https://api.openai.com/profile":{"email":"window@example.com"},"https://api.openai.com/auth":{"chatgpt_account_id":"id-window"},"chatgpt_plan_type":"pro"})");
  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"codex":[)"
      << R"({"identifier":"window@example.com","refreshToken":"r1","accessToken":")"
      << accessToken << R"(","tokenExpiration":)" << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-window","quota:codex":"1","quota:codex:primary":"1","quota_reset:codex:primary":"1774834810","quota_window_minutes:codex:primary":"300","quota:codex:secondary":"0","quota_reset:codex:secondary":"1775000000","quota_window_minutes:codex:secondary":"10080"}}]})";
  out.close();

  CodexProvider provider;
  const auto &accounts = provider.getAccounts();
  ASSERT_EQ(accounts.size(), 1u);
  ASSERT_TRUE(accounts.front().metadata.count("chatgpt_plan_type"));
  EXPECT_EQ(accounts.front().metadata.at("chatgpt_plan_type"), "pro");

  const auto quotas = provider.getAllQuotas();
  auto quotaIt = quotas.find("window@example.com");
  ASSERT_NE(quotaIt, quotas.end());
  ASSERT_EQ(quotaIt->second.size(), 2u);
  EXPECT_EQ(quotaIt->second[0].name, "5h limit");
  EXPECT_EQ(quotaIt->second[1].name, "weekly limit");
  EXPECT_EQ(quotaIt->second[0].note, "Plan: Pro");
  EXPECT_TRUE(quotaIt->second[1].note.empty());

  auto selected = provider.getAvailableAccount(std::string("gpt-5.2-codex"));
  EXPECT_FALSE(selected.has_value());
}

TEST(CodexProvider, AvailableAccountUsesControllingWindowAcrossAccounts) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_codex_controlling_window_home";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"codex":[)"
      << R"({"identifier":"blocked@example.com","refreshToken":"r1","accessToken":"a1","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-blocked","quota:codex":"1","quota:codex:primary":"1","quota_window_minutes:codex:primary":"300","quota:codex:secondary":"0","quota_window_minutes:codex:secondary":"10080"}},)"
      << R"({"identifier":"usable@example.com","refreshToken":"r2","accessToken":"a2","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-usable","quota:codex:primary":"0.4","quota_window_minutes:codex:primary":"300","quota:codex:secondary":"0.3","quota_window_minutes:codex:secondary":"10080"}}]})";
  out.close();

  CodexProvider provider;
  auto selected = provider.getAvailableAccount(std::string("gpt-5.2-codex"));
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->getIdentifier(), "usable@example.com");
}

TEST(CodexProvider, ProcessSseLineEmitsToolChunkFromOutputItemDone) {
  CodexProvider provider;
  std::vector<StreamEvent> events;
  AgentMetrics metrics;
  bool metricsReceived = false;
  bool doneReceived = false;
  CodexProviderTestAccessor::ToolCallTracker tracker;
  std::function<void(const StreamEvent &)> onEvent =
      [&](const StreamEvent &event) { events.push_back(event); };

  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.output_item.done","output_index":2,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"run_command"}})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);

  ASSERT_EQ(events.size(), 1u);
  const auto *chunk = std::get_if<ToolCallChunk>(&events.front());
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->index, 2u);
  EXPECT_EQ(chunk->id, "call_1");
  EXPECT_EQ(chunk->nameDelta, "run_command");
}

TEST(CodexProvider,
     ProcessSseLineEmitsFunctionArgumentsFromDoneEventWhenNoDeltaArrived) {
  CodexProvider provider;
  std::vector<StreamEvent> events;
  AgentMetrics metrics;
  bool metricsReceived = false;
  bool doneReceived = false;
  CodexProviderTestAccessor::ToolCallTracker tracker;
  std::function<void(const StreamEvent &)> onEvent =
      [&](const StreamEvent &event) { events.push_back(event); };

  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_2","call_id":"call_2","name":"write_file"}})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);
  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.function_call_arguments.done","output_index":0,"arguments":"{\"path\":\"notes.md\"}"})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);

  ASSERT_EQ(events.size(), 3u);
  const auto *chunk = std::get_if<ToolCallChunk>(&events[1]);
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->id, "call_2");
  EXPECT_EQ(chunk->index, 0u);
  EXPECT_EQ(chunk->argsDelta, R"({"path":"notes.md"})");
  const auto *finalCall = std::get_if<ToolCall>(&events.back());
  ASSERT_NE(finalCall, nullptr);
  EXPECT_EQ(finalCall->id, "call_2");
  EXPECT_EQ(finalCall->name, "write_file");
  EXPECT_EQ(finalCall->args, R"({"path":"notes.md"})");
}

TEST(CodexProvider, ProcessSseLineDoesNotDuplicateDonePayloadAfterDeltas) {
  CodexProvider provider;
  std::vector<StreamEvent> events;
  AgentMetrics metrics;
  bool metricsReceived = false;
  bool doneReceived = false;
  CodexProviderTestAccessor::ToolCallTracker tracker;
  std::function<void(const StreamEvent &)> onEvent =
      [&](const StreamEvent &event) { events.push_back(event); };

  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.output_item.added","output_index":1,"item":{"type":"function_call","id":"fc_3","call_id":"call_3","name":"artifact_list"}})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);
  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.function_call_arguments.delta","output_index":1,"delta":"{\"path\":\"src\""}})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);
  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.function_call_arguments.done","output_index":1,"arguments":"{\"path\":\"src\"}"})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);

  ASSERT_EQ(events.size(), 3u);
  const auto *chunk = std::get_if<ToolCallChunk>(&events[1]);
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->id, "call_3");
  EXPECT_EQ(chunk->argsDelta, R"({"path":"src"})");
  const auto *finalCall = std::get_if<ToolCall>(&events.back());
  ASSERT_NE(finalCall, nullptr);
  EXPECT_EQ(finalCall->id, "call_3");
  EXPECT_EQ(finalCall->name, "artifact_list");
  EXPECT_EQ(finalCall->args, R"({"path":"src"})");
}

TEST(CodexProvider, ProcessSseLineFinalizesLongInterleavedToolCalls) {
  CodexProvider provider;
  std::vector<StreamEvent> events;
  AgentMetrics metrics;
  bool metricsReceived = false;
  bool doneReceived = false;
  CodexProviderTestAccessor::ToolCallTracker tracker;
  std::function<void(const StreamEvent &)> onEvent =
      [&](const StreamEvent &event) { events.push_back(event); };

  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_10","call_id":"call_10","name":"artifact_write"}})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);
  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.function_call_arguments.delta","output_index":0,"delta":"{\"path\":\"plans/repair.md\",\"content\":\""})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);
  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.output_item.added","output_index":1,"item":{"type":"function_call","id":"fc_11","call_id":"call_11","name":"search"}})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);
  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.function_call_arguments.done","output_index":1,"arguments":"{\"query\":\"tool finalization\"}"})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);
  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.function_call_arguments.delta","output_index":0,"delta":"draft body"})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);
  CodexProviderTestAccessor::processSseLine(
      provider,
      R"(data: {"type":"response.function_call_arguments.done","output_index":0,"arguments":"{\"path\":\"plans/repair.md\",\"content\":\"draft body\"}"})",
      onEvent, metrics, metricsReceived, doneReceived, tracker);

  ASSERT_EQ(events.size(), 8u);
  const auto *searchFinal = std::get_if<ToolCall>(&events[4]);
  ASSERT_NE(searchFinal, nullptr);
  EXPECT_EQ(searchFinal->id, "call_11");
  EXPECT_EQ(searchFinal->name, "search");
  EXPECT_EQ(searchFinal->args, R"({"query":"tool finalization"})");

  const auto *artifactFinal = std::get_if<ToolCall>(&events.back());
  ASSERT_NE(artifactFinal, nullptr);
  EXPECT_EQ(artifactFinal->id, "call_10");
  EXPECT_EQ(artifactFinal->name, "artifact_write");
  EXPECT_EQ(artifactFinal->args,
            R"({"path":"plans/repair.md","content":"draft body"})");
}


// ---------------------------------------------------------------------------
// Plan tracking tests
//
// The /wham/usage backend response (mirrored in stored metadata) is the
// authoritative source for the user's current plan_type. The JWT
// `chatgpt_plan_type` claim is set at OAuth time and lags reality: a user who
// first signed in on Free and later upgraded to Plus/Pro keeps the original
// "free" claim until a forced re-auth.
//
// These tests pin the policy: when both sources disagree, the metadata value
// wins. When metadata is missing (typical right after first sign-in, before
// the first quota refresh), the JWT is used as a best-effort fallback.
// ---------------------------------------------------------------------------

TEST(CodexProvider,
     MetadataPlanTypeOverridesStaleJwtPlanTypeAfterUpgrade) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_codex_metadata_overrides_jwt";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  // The JWT claims the user is on "free" — this matches the real-world bug
  // where ChatGPT does not refresh the JWT plan claim when a user upgrades to
  // Plus or Pro mid-session.
  const std::string staleJwt = makeJwt(
      R"({"https://api.openai.com/profile":{"email":"upgrader@example.com"},"https://api.openai.com/auth":{"chatgpt_account_id":"id-upgrader","chatgpt_plan_type":"free"}})");

  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  // Stored metadata records the up-to-date plan ("pro") — this is what
  // fetchAndStoreQuotas would have written from a successful /wham/usage call.
  out << R"({"codex":[{"identifier":"upgrader@example.com","refreshToken":"r1","accessToken":")"
      << staleJwt << R"(","tokenExpiration":)" << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-upgrader","chatgpt_plan_type":"pro"}}]})";
  out.close();

  CodexProvider provider;
  const auto &accounts = provider.getAccounts();
  ASSERT_EQ(accounts.size(), 1u);
  ASSERT_TRUE(accounts.front().metadata.count("chatgpt_plan_type"));
  // Without the fix, normalizeCodexAccount() would resolve the plan via the
  // JWT first and overwrite the metadata back to "free".
  EXPECT_EQ(accounts.front().metadata.at("chatgpt_plan_type"), "pro");
}

TEST(CodexProvider, JwtPlanTypeIsUsedWhenMetadataMissing) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_codex_jwt_fallback";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::string jwt = makeJwt(
      R"({"https://api.openai.com/profile":{"email":"fresh@example.com"},"https://api.openai.com/auth":{"chatgpt_account_id":"id-fresh","chatgpt_plan_type":"plus"}})");

  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  // No chatgpt_plan_type in metadata; mirrors a brand-new sign-in before the
  // first /wham/usage refresh has populated the cached value.
  out << R"({"codex":[{"identifier":"fresh@example.com","refreshToken":"r1","accessToken":")"
      << jwt << R"(","tokenExpiration":)" << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-fresh"}}]})";
  out.close();

  CodexProvider provider;
  const auto &accounts = provider.getAccounts();
  ASSERT_EQ(accounts.size(), 1u);
  ASSERT_TRUE(accounts.front().metadata.count("chatgpt_plan_type"));
  EXPECT_EQ(accounts.front().metadata.at("chatgpt_plan_type"), "plus");
}

TEST(CodexProvider, EmptyMetadataPlanTypeFallsBackToJwt) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_codex_empty_metadata_fallback";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::string jwt = makeJwt(
      R"({"https://api.openai.com/profile":{"email":"blank@example.com"},"https://api.openai.com/auth":{"chatgpt_account_id":"id-blank","chatgpt_plan_type":"pro"}})");

  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  // Metadata key is present but empty/whitespace — should fall through to JWT.
  out << R"({"codex":[{"identifier":"blank@example.com","refreshToken":"r1","accessToken":")"
      << jwt << R"(","tokenExpiration":)" << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-blank","chatgpt_plan_type":"   "}}]})";
  out.close();

  CodexProvider provider;
  const auto &accounts = provider.getAccounts();
  ASSERT_EQ(accounts.size(), 1u);
  ASSERT_TRUE(accounts.front().metadata.count("chatgpt_plan_type"));
  EXPECT_EQ(accounts.front().metadata.at("chatgpt_plan_type"), "pro");
}

TEST(CodexProvider, ProLitePlanTypeShownWithCorrectDisplayName) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_codex_prolite_display";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"codex":[{"identifier":"lite@example.com","refreshToken":"r1","accessToken":"a1","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-lite","chatgpt_plan_type":"prolite","quota:codex":"1","quota:codex:primary":"0.8","quota_window_minutes:codex:primary":"300"}}]})";
  out.close();

  CodexProvider provider;
  const auto quotas = provider.getAllQuotas();
  auto quotaIt = quotas.find("lite@example.com");
  ASSERT_NE(quotaIt, quotas.end());
  ASSERT_FALSE(quotaIt->second.empty());
  // Display name should be "Pro Lite" (matching upstream KnownPlan).
  EXPECT_EQ(quotaIt->second.front().note, "Plan: Pro Lite");
}

TEST(CodexProvider, GuestPlanTypeShownAsGuest) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_codex_guest_display";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"codex":[{"identifier":"guest@example.com","refreshToken":"r1","accessToken":"a1","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-guest","chatgpt_plan_type":"guest","quota:codex":"1","quota:codex:primary":"0.5","quota_window_minutes:codex:primary":"300"}}]})";
  out.close();

  CodexProvider provider;
  const auto quotas = provider.getAllQuotas();
  auto quotaIt = quotas.find("guest@example.com");
  ASSERT_NE(quotaIt, quotas.end());
  ASSERT_FALSE(quotaIt->second.empty());
  EXPECT_EQ(quotaIt->second.front().note, "Plan: Guest");
}

TEST(CodexProvider, FreeWorkspacePlanTypeShownAsFreeWorkspace) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_codex_free_workspace_display";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"codex":[{"identifier":"workspace@example.com","refreshToken":"r1","accessToken":"a1","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"chatgpt_account_id":"id-ws","chatgpt_plan_type":"free_workspace","quota:codex":"1","quota:codex:primary":"0.5","quota_window_minutes:codex:primary":"300"}}]})";
  out.close();

  CodexProvider provider;
  const auto quotas = provider.getAllQuotas();
  auto quotaIt = quotas.find("workspace@example.com");
  ASSERT_NE(quotaIt, quotas.end());
  ASSERT_FALSE(quotaIt->second.empty());
  EXPECT_EQ(quotaIt->second.front().note, "Plan: Free Workspace");
}
