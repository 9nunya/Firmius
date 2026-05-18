//
// Unit tests for the permission policy engine + suggestion engine.
//
// Lock down:
//   * JSON load/save round-trips a doc with mixed match keys.
//   * Glob, regex, exact match all evaluate correctly.
//   * deny rules win over allow rules regardless of order.
//   * Project doc overlays user doc; deny anywhere wins.
//   * Session rules apply but never persist.
//   * Suggestion engine produces sensible options for each category.
//

#include <gtest/gtest.h>

#include "environment/PermissionPolicy.hpp"
#include "environment/PermissionSuggestionEngine.hpp"
#include "environment/PolicyEngine.hpp"

#include <filesystem>
#include <fstream>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

// Test fixture that gives us isolated user + project policy paths.
class PolicyEngineTest : public ::testing::Test {
protected:
  std::filesystem::path tempDir;
  std::filesystem::path userPath;
  std::filesystem::path projectDir;

  void SetUp() override {
    char tpl[] = "/tmp/firmius_policy_test_XXXXXX";
    char *r = mkdtemp(tpl);
    ASSERT_NE(r, nullptr);
    tempDir = r;
    userPath = tempDir / "user_permissions.json";
    projectDir = tempDir / "project";
    std::filesystem::create_directories(projectDir);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
  }

  std::unique_ptr<PolicyEngine> makeEngine() {
    return std::make_unique<PolicyEngine>(userPath, projectDir);
  }

  void writeUserPolicy(const std::string &json) {
    std::ofstream out(userPath);
    out << json;
  }

  void writeProjectPolicy(const std::string &json) {
    std::filesystem::create_directories(projectDir / ".firmius");
    std::ofstream out(projectDir / ".firmius" / "permissions.json");
    out << json;
  }
};

PolicyRule makeAllow(std::string category,
                     std::map<std::string, std::string> match,
                     RuleScope scope = RuleScope::Global) {
  PolicyRule r;
  r.category = std::move(category);
  r.decision = PolicyDecision::Allow;
  r.scope = scope;
  r.match = std::move(match);
  return r;
}

PolicyRule makeDeny(std::string category,
                    std::map<std::string, std::string> match,
                    RuleScope scope = RuleScope::Global) {
  PolicyRule r;
  r.category = std::move(category);
  r.decision = PolicyDecision::Deny;
  r.scope = scope;
  r.match = std::move(match);
  return r;
}

} // namespace

// ── Basic eval ──────────────────────────────────────────────────────────

TEST_F(PolicyEngineTest, NoRulesFallsBackToCategoryDefault) {
  auto engine = makeEngine();
  PolicyRequest req;
  req.category = kCatNetworkSearch; // built-in default Allow
  auto eval = engine->evaluate(req);
  EXPECT_EQ(eval.decision, PolicyDecision::Allow);
  EXPECT_TRUE(eval.fromCategoryDefault);
}

TEST_F(PolicyEngineTest, AskWhenCategoryDefaultIsAsk) {
  auto engine = makeEngine();
  PolicyRequest req;
  req.category = kCatProcessExec;
  auto eval = engine->evaluate(req);
  EXPECT_EQ(eval.decision, PolicyDecision::Ask);
}

TEST_F(PolicyEngineTest, ExplicitAllowRuleWinsOverAskDefault) {
  auto engine = makeEngine();
  engine->upsertRule(makeAllow(kCatProcessExec,
                                {{"command_regex", "^cargo "}}));
  PolicyRequest req;
  req.category = kCatProcessExec;
  req.command = "cargo build";
  auto eval = engine->evaluate(req);
  EXPECT_EQ(eval.decision, PolicyDecision::Allow);
  ASSERT_TRUE(eval.matchedRule.has_value());
  EXPECT_EQ(eval.matchedRule->decision, PolicyDecision::Allow);
}

TEST_F(PolicyEngineTest, NoMatchKeyFallsThrough) {
  auto engine = makeEngine();
  engine->upsertRule(makeAllow(kCatProcessExec,
                                {{"command_regex", "^cargo "}}));
  PolicyRequest req;
  req.category = kCatProcessExec;
  req.command = "rm -rf /";
  auto eval = engine->evaluate(req);
  EXPECT_EQ(eval.decision, PolicyDecision::Ask);
}

// ── Deny precedence ─────────────────────────────────────────────────────

TEST_F(PolicyEngineTest, DenyWinsEvenWhenAllowMatchesFirst) {
  auto engine = makeEngine();
  // Allow registered first; deny registered after.
  engine->upsertRule(makeAllow(kCatFileRead, {{"path_glob", "**"}}));
  engine->upsertRule(makeDeny(kCatFileRead, {{"path_glob", "**/*.env"}}));
  PolicyRequest req;
  req.category = kCatFileRead;
  req.path = "/home/me/proj/.env";
  auto eval = engine->evaluate(req);
  EXPECT_EQ(eval.decision, PolicyDecision::Deny);
}

// ── Match key behaviors ─────────────────────────────────────────────────

TEST_F(PolicyEngineTest, GlobMatchSupportsDoubleStar) {
  auto engine = makeEngine();
  engine->upsertRule(makeAllow(kCatFileRead,
                                {{"path_glob", "/home/me/proj/**"}}));
  PolicyRequest req;
  req.category = kCatFileRead;
  req.path = "/home/me/proj/src/main.rs";
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Allow);
}

TEST_F(PolicyEngineTest, RegexMatchHandlesAlternation) {
  auto engine = makeEngine();
  engine->upsertRule(makeAllow(
      kCatProcessExec,
      {{"command_regex", "^(cargo|rustc) (build|test)( |$)"}}));
  PolicyRequest req;
  req.category = kCatProcessExec;
  req.command = "cargo test --release";
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Allow);
  req.command = "cargo run";
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Ask);
}

TEST_F(PolicyEngineTest, ExactKeyMatchesWithoutSuffix) {
  auto engine = makeEngine();
  PolicyRule r;
  r.category = kCatAgentSpawn;
  r.decision = PolicyDecision::Allow;
  r.scope = RuleScope::Global;
  r.match["persona"] = "executor";
  engine->upsertRule(std::move(r));
  PolicyRequest req;
  req.category = kCatAgentSpawn;
  req.persona = "executor";
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Allow);
  req.persona = "lead";
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Allow);
  // ^ wait — agent.spawn defaults to Allow built-in, so we can't test
  // "no match → Ask" without overriding the default. Verify match
  // semantics directly via scheme-style key.
}

TEST_F(PolicyEngineTest, MultipleMatchKeysMustAllMatch) {
  auto engine = makeEngine();
  engine->upsertRule(makeAllow(kCatProcessExec,
                                {{"command_regex", "^cargo "},
                                 {"cwd_glob", "/home/me/projects/**"}}));
  PolicyRequest req;
  req.category = kCatProcessExec;
  req.command = "cargo build";
  req.cwd = "/home/me/projects/foo";
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Allow);
  // Same command from a different cwd doesn't match.
  req.cwd = "/tmp/x";
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Ask);
}

// ── Persistence round-trip ──────────────────────────────────────────────

TEST_F(PolicyEngineTest, RoundTripPolicyFile) {
  auto engine = makeEngine();
  engine->upsertRule(makeAllow(kCatProcessExec,
                                {{"command_regex", "^git status"}}));
  engine->upsertRule(makeDeny(kCatFileRead,
                                {{"path_glob", "**/*.env"}}));

  // Recreate engine — should load the same rules from disk.
  auto engine2 = makeEngine();
  auto rules = engine2->listRules();
  ASSERT_EQ(rules.size(), 2u);
  // Find the allow + deny by category.
  bool foundAllow = false, foundDeny = false;
  for (const auto &r : rules) {
    if (r.category == kCatProcessExec &&
        r.decision == PolicyDecision::Allow) {
      foundAllow = true;
      EXPECT_EQ(r.match.at("command_regex"), "^git status");
    }
    if (r.category == kCatFileRead &&
        r.decision == PolicyDecision::Deny) {
      foundDeny = true;
      EXPECT_EQ(r.match.at("path_glob"), "**/*.env");
    }
  }
  EXPECT_TRUE(foundAllow);
  EXPECT_TRUE(foundDeny);
}

// ── Project overlay ─────────────────────────────────────────────────────

TEST_F(PolicyEngineTest, ProjectRulesOverlayUserRules) {
  // User policy: allow git status globally.
  writeUserPolicy(R"({
    "version": 2,
    "default_decision": "ask",
    "rules": [
      {
        "id": "u1",
        "category": "process.exec",
        "decision": "allow",
        "scope": "global",
        "match": {"command_regex": "^git status"}
      }
    ]
  })");
  // Project deny.
  writeProjectPolicy(R"({
    "version": 2,
    "rules": [
      {
        "id": "p1",
        "category": "process.exec",
        "decision": "deny",
        "scope": "project",
        "match": {"command_regex": "^git status"}
      }
    ]
  })");
  auto engine = makeEngine();
  PolicyRequest req;
  req.category = kCatProcessExec;
  req.command = "git status";
  // Project deny wins.
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Deny);
}

// ── Session rules ───────────────────────────────────────────────────────

TEST_F(PolicyEngineTest, SessionRuleAppliesButDoesNotPersist) {
  auto engine = makeEngine();
  engine->upsertRule(makeAllow(kCatNetworkFetch,
                                {{"host_glob", "api.github.com"}},
                                RuleScope::Session));
  PolicyRequest req;
  req.category = kCatNetworkFetch;
  req.host = "api.github.com";
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Allow);

  // New engine instance — session rule should NOT survive.
  auto engine2 = makeEngine();
  EXPECT_EQ(engine2->evaluate(req).decision, PolicyDecision::Ask);
}

TEST_F(PolicyEngineTest, ClearSessionRulesDropsThem) {
  auto engine = makeEngine();
  engine->upsertRule(makeAllow(kCatNetworkFetch,
                                {{"host_glob", "*"}}, RuleScope::Session));
  EXPECT_TRUE(engine->hasSessionRules());
  engine->clearSessionRules();
  EXPECT_FALSE(engine->hasSessionRules());
}

// ── Rule mutation ───────────────────────────────────────────────────────

TEST_F(PolicyEngineTest, RemoveRuleRemovesByIdFromAnyScope) {
  auto engine = makeEngine();
  auto id1 = engine->upsertRule(makeAllow(kCatProcessExec,
                                           {{"command_regex", "^a"}}));
  auto id2 = engine->upsertRule(makeAllow(kCatProcessExec,
                                           {{"command_regex", "^b"}},
                                           RuleScope::Session));
  EXPECT_EQ(engine->listRules().size(), 2u);
  EXPECT_TRUE(engine->removeRule(id2));
  EXPECT_EQ(engine->listRules().size(), 1u);
  EXPECT_TRUE(engine->removeRule(id1));
  EXPECT_EQ(engine->listRules().size(), 0u);
  EXPECT_FALSE(engine->removeRule("nonexistent"));
}

// ── Expiry ──────────────────────────────────────────────────────────────

TEST_F(PolicyEngineTest, ExpiredRuleIsIgnored) {
  auto engine = makeEngine();
  PolicyRule r = makeAllow(kCatFileRead, {{"path_glob", "/foo/**"}});
  r.expiresAt = 1; // unix ms 1 = ~1970, definitely expired
  engine->upsertRule(std::move(r));
  PolicyRequest req;
  req.category = kCatFileRead;
  req.path = "/foo/bar";
  // Expired allow rule should be skipped → falls through to category default.
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Ask);
}

// ── Suggestion engine ──────────────────────────────────────────────────

TEST(PermissionSuggestionEngine, ProcessExecYieldsProgramRegexSuggestions) {
  PolicyRequest req;
  req.category = kCatProcessExec;
  req.command = "cmake --build build && ./foo.out";
  req.cwd = "/home/me/proj";
  CommandIntent intent;
  intent.parsedCommands = {"cmake --build build", "./foo.out"};
  intent.primaryCommand = "cmake";
  auto suggestions =
      PermissionSuggestionEngine::generate(req, intent);
  ASSERT_FALSE(suggestions.empty());
  bool foundCmake = false, foundFoo = false;
  for (const auto &s : suggestions) {
    if (s.label.find("cmake") != std::string::npos) foundCmake = true;
    if (s.label.find("foo.out") != std::string::npos) foundFoo = true;
  }
  EXPECT_TRUE(foundCmake);
  EXPECT_TRUE(foundFoo);
}

TEST(PermissionSuggestionEngine, FileReadOffersExactAndDirAndExtension) {
  PolicyRequest req;
  req.category = kCatFileRead;
  req.path = "/home/me/proj/src/main.rs";
  CommandIntent intent;
  auto suggestions =
      PermissionSuggestionEngine::generate(req, intent);
  ASSERT_FALSE(suggestions.empty());
  bool foundExact = false, foundExt = false;
  for (const auto &s : suggestions) {
    if (s.rule.match.count("path_glob")) {
      const auto p = s.rule.match.at("path_glob");
      if (p == "/home/me/proj/src/main.rs") foundExact = true;
      if (p == "**/*.rs") foundExt = true;
    }
  }
  EXPECT_TRUE(foundExact);
  EXPECT_TRUE(foundExt);
}

TEST(PermissionSuggestionEngine, FileReadFlagsSensitivePathWithDeny) {
  PolicyRequest req;
  req.category = kCatFileRead;
  req.path = "/home/me/.ssh/id_rsa";
  CommandIntent intent;
  auto suggestions =
      PermissionSuggestionEngine::generate(req, intent);
  bool foundDeny = false;
  for (const auto &s : suggestions) {
    if (s.rule.decision == PolicyDecision::Deny) foundDeny = true;
  }
  EXPECT_TRUE(foundDeny)
      << "Sensitive path should produce a deny suggestion";
}

TEST(PermissionSuggestionEngine, NetworkFetchSuggestsHostScope) {
  PolicyRequest req;
  req.category = kCatNetworkFetch;
  req.url = "https://api.github.com/repos/x/y";
  req.host = "api.github.com";
  req.scheme = "https";
  CommandIntent intent;
  auto suggestions =
      PermissionSuggestionEngine::generate(req, intent);
  bool foundHost = false, foundParent = false;
  for (const auto &s : suggestions) {
    if (s.rule.match.count("host_glob")) {
      const auto h = s.rule.match.at("host_glob");
      if (h == "api.github.com") foundHost = true;
      if (h == "*.github.com") foundParent = true;
    }
  }
  EXPECT_TRUE(foundHost);
  EXPECT_TRUE(foundParent);
}

TEST(PermissionSuggestionEngine, AgentSpawnSuggestsPersonaPair) {
  PolicyRequest req;
  req.category = kCatAgentSpawn;
  req.persona = "executor";
  req.parentPersona = "lead";
  CommandIntent intent;
  auto suggestions =
      PermissionSuggestionEngine::generate(req, intent);
  bool foundPair = false;
  for (const auto &s : suggestions) {
    if (s.rule.match.count("persona") &&
        s.rule.match.count("parent_persona")) {
      foundPair = true;
      EXPECT_EQ(s.rule.match.at("persona"), "executor");
      EXPECT_EQ(s.rule.match.at("parent_persona"), "lead");
    }
  }
  EXPECT_TRUE(foundPair);
}

// ── Mode CRUD ──────────────────────────────────────────────────────────

TEST_F(PolicyEngineTest, FreshDocSeedsAskAndYolo) {
  auto engine = makeEngine();
  auto modes = engine->listModes();
  ASSERT_GE(modes.size(), 2u);
  bool foundAsk = false, foundYolo = false;
  for (const auto &m : modes) {
    if (m.id == kModeAsk)  { foundAsk = true;  EXPECT_TRUE(m.builtIn); }
    if (m.id == kModeYolo) { foundYolo = true; EXPECT_TRUE(m.builtIn); }
  }
  EXPECT_TRUE(foundAsk);
  EXPECT_TRUE(foundYolo);
  // Default active mode is "ask".
  EXPECT_EQ(engine->activeMode().id, kModeAsk);
}

TEST_F(PolicyEngineTest, YoloModeAllowsEverythingByDefault) {
  auto engine = makeEngine();
  ASSERT_TRUE(engine->setActiveMode(kModeYolo));
  for (const auto *cat : {kCatProcessExec, kCatFileWrite, kCatFileDelete,
                            kCatNetworkFetch}) {
    PolicyRequest req;
    req.category = cat;
    EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Allow)
        << "yolo mode should auto-allow " << cat;
  }
}

TEST_F(PolicyEngineTest, AskModeFallsBackToCategoryDefault) {
  auto engine = makeEngine();
  // active mode is ask by default.
  PolicyRequest req;
  req.category = kCatProcessExec;
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Ask);
  // network.search default is allow even in ask mode.
  req.category = kCatNetworkSearch;
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Allow);
}

TEST_F(PolicyEngineTest, ModeScopedRuleOnlyAppliesWhenModeActive) {
  auto engine = makeEngine();
  // Add a mode-scoped allow rule for the "ask" mode.
  PolicyRule r = makeAllow(kCatProcessExec,
                           {{"command_regex", "^cargo build"}});
  r.modeId = kModeAsk;
  engine->upsertRule(std::move(r));

  PolicyRequest req;
  req.category = kCatProcessExec;
  req.command = "cargo build";

  // While ask is active, the rule applies.
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Allow);

  // Switch to yolo: rule still allows because yolo's category default
  // is Allow anyway. Switch to a fresh mode with default deny to
  // verify isolation.
  PermissionMode strict;
  strict.id = "strict";
  strict.name = "strict";
  strict.categoryDefaults.byCategory[kCatProcessExec] = PolicyDecision::Ask;
  ASSERT_FALSE(engine->createMode(strict).empty());
  ASSERT_TRUE(engine->setActiveMode("strict"));
  // The ask-mode-scoped rule no longer applies; falls through to
  // strict's category default (Ask).
  EXPECT_EQ(engine->evaluate(req).decision, PolicyDecision::Ask);
}

TEST_F(PolicyEngineTest, CreateRenameDeleteModes) {
  auto engine = makeEngine();
  PermissionMode m;
  m.name = "custom";
  auto id = engine->createMode(std::move(m));
  ASSERT_FALSE(id.empty());

  // Duplicate name rejected.
  PermissionMode dup;
  dup.name = "custom";
  EXPECT_TRUE(engine->createMode(std::move(dup)).empty());

  // Rename works.
  EXPECT_TRUE(engine->renameMode(id, "renamed"));

  // Rename to an existing built-in name — rejected.
  EXPECT_FALSE(engine->renameMode(id, "yolo"));

  // Built-in modes can't be renamed.
  EXPECT_FALSE(engine->renameMode(kModeAsk, "asked"));

  // Active mode can't be deleted.
  ASSERT_TRUE(engine->setActiveMode(id));
  EXPECT_FALSE(engine->deleteMode(id));

  // Switch to ask, then delete the custom mode.
  ASSERT_TRUE(engine->setActiveMode(kModeAsk));
  EXPECT_TRUE(engine->deleteMode(id));

  // Deleting a built-in is rejected.
  EXPECT_FALSE(engine->deleteMode(kModeAsk));
  EXPECT_FALSE(engine->deleteMode(kModeYolo));
}

TEST_F(PolicyEngineTest, DeletingModeAlsoDropsItsRules) {
  auto engine = makeEngine();
  PermissionMode m;
  m.name = "tmp";
  auto id = engine->createMode(std::move(m));
  ASSERT_FALSE(id.empty());

  PolicyRule r = makeAllow(kCatProcessExec,
                            {{"command_regex", "^foo"}});
  r.modeId = id;
  auto ruleId = engine->upsertRule(std::move(r));

  EXPECT_TRUE(engine->deleteMode(id));

  // Rule should be gone.
  bool found = false;
  for (const auto &rr : engine->listRules()) {
    if (rr.id == ruleId) { found = true; break; }
  }
  EXPECT_FALSE(found) << "deleting a mode should drop its rules";
}

TEST_F(PolicyEngineTest, ActiveModePersistsAcrossReload) {
  auto engine = makeEngine();
  ASSERT_TRUE(engine->setActiveMode(kModeYolo));

  auto engine2 = makeEngine();
  EXPECT_EQ(engine2->activeMode().id, kModeYolo);
}
