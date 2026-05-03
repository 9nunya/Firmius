// Smoke test for the persona-scoped sub-mode set in `prompts/modes/`.
// Each persona that carries internal phases gets its sub-mode set loaded
// here. If a sub-mode file is renamed or its frontmatter is malformed,
// the registry will silently skip it — this test catches that.
//
// The test points the loader at the workspace's `prompts/` directory via
// `FIRMIUS_PROMPTS_DIR` and then asserts:
//   - every expected persona has *all* of its sub-modes loaded,
//   - bare-name resolution works ("apply" + persona "forge" → "forge:apply"),
//   - qualified-name resolution works ("aster:route" → "aster:route").

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

TEST_F(ModeRegistryTest, ForgeSubmodesLoaded) {
  expectPersonaHasSubmodes(
      "forge",
      {"prime", "diagnose", "orchestrate", "apply", "verify", "return"});
}

TEST_F(ModeRegistryTest, AsterSubmodesLoaded) {
  expectPersonaHasSubmodes("aster", {"route", "synthesize", "intervene"});
}

TEST_F(ModeRegistryTest, FastSubmodesLoaded) {
  expectPersonaHasSubmodes("fast", {"probe", "apply", "verify", "escalate"});
}

TEST_F(ModeRegistryTest, GlimmerSubmodesLoaded) {
  expectPersonaHasSubmodes("glimmer", {"isolate", "penetrate", "label"});
}

TEST_F(ModeRegistryTest, HarborSubmodesLoaded) {
  expectPersonaHasSubmodes("harbor",
                           {"diagnose", "excise", "reanchor", "verify"});
}

TEST_F(ModeRegistryTest, LoomSubmodesLoaded) {
  expectPersonaHasSubmodes("loom", {"scan", "sift", "weave"});
}

TEST_F(ModeRegistryTest, MeridianSubmodesLoaded) {
  expectPersonaHasSubmodes("meridian", {"recon", "gates", "cuts"});
}

TEST_F(ModeRegistryTest, VellumSubmodesLoaded) {
  expectPersonaHasSubmodes("vellum", {"pathology", "joint", "load"});
}

TEST_F(ModeRegistryTest, WitnessSubmodesLoaded) {
  expectPersonaHasSubmodes("witness", {"charge", "forensics", "verdict"});
}

TEST_F(ModeRegistryTest, BareNameResolvesPersonaScopedFirst) {
  // "apply" without persona scope is ambiguous — but with persona "forge"
  // it must resolve to "forge:apply", not the system "execute" or any
  // other persona's "apply".
  const Mode *m =
      ModeRegistry::instance().resolveForPersona("apply", "forge");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ("forge:apply", m->qualifiedName());
}

TEST_F(ModeRegistryTest, ExploreAliasResolvesToPersonaReconMode) {
  const Mode *aster =
      ModeRegistry::instance().resolveForPersona("explore", "aster");
  ASSERT_NE(nullptr, aster);
  EXPECT_EQ("aster:route", aster->qualifiedName());

  const Mode *loom =
      ModeRegistry::instance().resolveForPersona("explore", "loom");
  ASSERT_NE(nullptr, loom);
  EXPECT_EQ("loom:scan", loom->qualifiedName());
}

TEST_F(ModeRegistryTest, QualifiedNameResolvesVerbatim) {
  const Mode *m =
      ModeRegistry::instance().resolveForPersona("aster:route", "forge");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ("aster:route", m->qualifiedName());
  // The persona scope on the file must match its directory.
  ASSERT_TRUE(m->personaScope.has_value());
  EXPECT_EQ("aster", *m->personaScope);
}

TEST_F(ModeRegistryTest, SystemModeFallsBackWhenNoPersonaSubmode) {
  // Persona "loom" has no "execute" sub-mode; resolution must fall back
  // to the system-level "execute" mode.
  const Mode *m =
      ModeRegistry::instance().resolveForPersona("execute", "loom");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ("execute", m->qualifiedName());
  EXPECT_FALSE(m->personaScope.has_value())
      << "System modes must have no personaScope.";
}

} // namespace
