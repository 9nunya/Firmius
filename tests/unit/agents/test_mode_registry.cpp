// Smoke test for the persona-scoped sub-mode set in `prompts/modes/`.
// Each persona that carries internal phases gets its sub-mode set loaded
// here. If a sub-mode file is renamed or its frontmatter is malformed,
// the registry will silently skip it — this test catches that.
//
// The test points the loader at the workspace's `prompts/` directory via
// `FIRMIUS_PROMPTS_DIR` and then asserts:
//   - every expected persona has *all* of its sub-modes loaded,
//   - bare-name resolution works against the new plain personas,
//   - qualified-name resolution works ("lead:plan" → "lead:plan").

#include <gtest/gtest.h>

#include "agents/modes/Mode.hpp"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

using firmius::core::modes::Mode;
using firmius::core::modes::ModeRegistry;

// Walk up from CWD to find the workspace root (the directory that
// contains `prompts/`). Tests can be run from `build/` or the repo root,
// so we tolerate both.
std::filesystem::path findPromptsDir() {
  auto cur = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    auto candidate = cur / "prompts";
    if (std::filesystem::exists(candidate / "modes")) {
      return candidate;
    }
    if (cur.has_parent_path() && cur != cur.parent_path()) {
      cur = cur.parent_path();
      continue;
    }
    break;
  }
  return {};
}

class ModeRegistryTest : public ::testing::Test {
protected:
  void SetUp() override {
    promptsDir_ = findPromptsDir();
    ASSERT_FALSE(promptsDir_.empty())
        << "Could not locate workspace prompts/ directory from "
        << std::filesystem::current_path();

    // Force the loader to read the workspace prompts (not the user
    // cache, which may be stale during dev or absent in CI).
    setenv("FIRMIUS_PROMPTS_DIR", promptsDir_.c_str(), /*overwrite=*/1);
    ModeRegistry::instance().reload();
  }

  std::filesystem::path promptsDir_;
};

void expectPersonaHasSubmodes(const std::string &persona,
                              const std::vector<std::string> &submodes) {
  auto names = ModeRegistry::instance().listForPersona(persona);
  std::set<std::string> got(names.begin(), names.end());
  for (const auto &sub : submodes) {
    const std::string qualified = persona + ":" + sub;
    EXPECT_TRUE(got.count(qualified) > 0)
        << "Expected sub-mode '" << qualified << "' to be loaded; got: ["
        << [&] {
             std::string joined;
             for (const auto &n : names) {
               if (!joined.empty())
                 joined += ", ";
               joined += n;
             }
             return joined;
           }()
        << "]";
  }
}

TEST_F(ModeRegistryTest, SystemModesLoaded) {
  EXPECT_NE(nullptr, ModeRegistry::instance().find("diagnose"));
  EXPECT_NE(nullptr, ModeRegistry::instance().find("execute"));
}

TEST_F(ModeRegistryTest, LeadSubmodesLoaded) {
  expectPersonaHasSubmodes("lead", {"code", "plan"});
}

TEST_F(ModeRegistryTest, CoderSubmodesLoaded) {
  expectPersonaHasSubmodes("coder", {"code", "verify"});
}

TEST_F(ModeRegistryTest, ExplorerSubmodesLoaded) {
  expectPersonaHasSubmodes("explorer", {"scan"});
}

TEST_F(ModeRegistryTest, ReviewerSubmodesLoaded) {
  expectPersonaHasSubmodes("reviewer", {"review"});
}

TEST_F(ModeRegistryTest, BareNameResolvesPersonaScopedFirst) {
  // "code" without persona scope should resolve to the active persona's
  // sub-mode first.
  const Mode *m =
      ModeRegistry::instance().resolveForPersona("code", "lead");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ("lead:code", m->qualifiedName());
}

TEST_F(ModeRegistryTest, ExploreAliasResolvesToPersonaReconMode) {
  const Mode *explorer =
      ModeRegistry::instance().resolveForPersona("explore", "explorer");
  ASSERT_NE(nullptr, explorer);
  EXPECT_EQ("explorer:scan", explorer->qualifiedName());
}

TEST_F(ModeRegistryTest, QualifiedNameResolvesVerbatim) {
  const Mode *m =
      ModeRegistry::instance().resolveForPersona("lead:plan", "coder");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ("lead:plan", m->qualifiedName());
  // The persona scope on the file must match its directory.
  ASSERT_TRUE(m->personaScope.has_value());
  EXPECT_EQ("lead", *m->personaScope);
}

TEST_F(ModeRegistryTest, SystemModeFallsBackWhenNoPersonaSubmode) {
  // Persona "reviewer" has no "execute" sub-mode; resolution must fall back
  // to the system-level "execute" mode.
  const Mode *m =
      ModeRegistry::instance().resolveForPersona("execute", "reviewer");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ("execute", m->qualifiedName());
  EXPECT_FALSE(m->personaScope.has_value())
      << "System modes must have no personaScope.";
}

} // namespace
