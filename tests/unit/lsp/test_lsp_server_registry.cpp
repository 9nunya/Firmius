#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "lsp/LspServerRegistry.hpp"
#include "lsp/LspServerSpec.hpp"

namespace fs = std::filesystem;

namespace {

class ScopedTempDir {
public:
  ScopedTempDir() {
    const auto unique = std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count());
    path_ = fs::temp_directory_path() / ("firmius_lsp_registry_test_" + unique);
    fs::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  const fs::path &path() const { return path_; }

private:
  fs::path path_;
};

} // namespace

using firmius::core::LspServerRegistry;
using firmius::core::LspServerSpec;

TEST(LspServerRegistryTest, BuiltinLookupByExtensionAndPathForSupportedLanguages) {
  auto &registry = LspServerRegistry::instance();

  const LspServerSpec *python = registry.findByExtension(".py");
  ASSERT_NE(python, nullptr);
  EXPECT_EQ(python->id, "python");

  const LspServerSpec *clangd = registry.findByPath("/tmp/project/main.cpp");
  ASSERT_NE(clangd, nullptr);
  EXPECT_EQ(clangd->id, "clangd");

  const LspServerSpec *typescript = registry.findByPath("/tmp/project/app.tsx");
  ASSERT_NE(typescript, nullptr);
  EXPECT_EQ(typescript->id, "typescript");

  const LspServerSpec *rust = registry.findByExtension(".rs");
  ASSERT_NE(rust, nullptr);
  EXPECT_EQ(rust->id, "rust");

  const LspServerSpec *go = registry.findByExtension(".go");
  ASSERT_NE(go, nullptr);
  EXPECT_EQ(go->id, "go");
}

TEST(LspServerRegistryTest, FindByIdReturnsKnownBuiltinSpecs) {
  auto &registry = LspServerRegistry::instance();

  EXPECT_NE(registry.findById("python"), nullptr);
  EXPECT_NE(registry.findById("clangd"), nullptr);
  EXPECT_NE(registry.findById("typescript"), nullptr);
  EXPECT_NE(registry.findById("rust"), nullptr);
  EXPECT_NE(registry.findById("go"), nullptr);
  EXPECT_NE(registry.findById("java"), nullptr);
  EXPECT_NE(registry.findById("bash"), nullptr);
}

TEST(LspServerRegistryTest, LanguageIdForPathUsesMappingsAndDefaults) {
  auto &registry = LspServerRegistry::instance();

  const LspServerSpec *typescript = registry.findById("typescript");
  ASSERT_NE(typescript, nullptr);
  EXPECT_EQ(typescript->languageIdForPath("index.ts"), "typescript");
  EXPECT_EQ(typescript->languageIdForPath("component.tsx"), "typescriptreact");
  EXPECT_EQ(typescript->languageIdForPath("legacy.js"), "javascript");

  const LspServerSpec *clangd = registry.findById("clangd");
  ASSERT_NE(clangd, nullptr);
  EXPECT_EQ(clangd->languageIdForPath("main.c"), "c");
  EXPECT_EQ(clangd->languageIdForPath("main.cpp"), "cpp");
  EXPECT_EQ(clangd->languageIdForPath("header.hpp"), "cpp");
}

TEST(LspServerRegistryTest, RegisterCustomSpecAndOverrideById) {
  auto &registry = LspServerRegistry::instance();

  const std::string customId = "unit_test_registry_override_custom";

  LspServerSpec custom;
  custom.id = customId;
  custom.extensions = {".unita"};
  custom.markers = {".unita-root"};
  custom.commands = {{"unit-a-ls", "--stdio"}};
  custom.defaultLanguageId = "unita";
  registry.registerCustomSpec(custom);

  const LspServerSpec *registered = registry.findById(customId);
  ASSERT_NE(registered, nullptr);
  EXPECT_TRUE(registered->isCustom);
  EXPECT_EQ(registered->defaultLanguageId, "unita");
  const LspServerSpec *byUnita = registry.findByExtension(".unita");
  ASSERT_NE(byUnita, nullptr);
  EXPECT_EQ(byUnita->id, customId);

  LspServerSpec override;
  override.id = customId;
  override.extensions = {".unitb"};
  override.markers = {".unitb-root"};
  override.commands = {{"unit-b-ls", "--stdio"}};
  override.defaultLanguageId = "unitb";
  override.languageIds = {{".unitb", "unitb"}};
  registry.registerCustomSpec(override);

  const LspServerSpec *overridden = registry.findById(customId);
  ASSERT_NE(overridden, nullptr);
  EXPECT_TRUE(overridden->isCustom);
  EXPECT_EQ(overridden->defaultLanguageId, "unitb");
  EXPECT_EQ(overridden->languageIdForPath("file.unitb"), "unitb");

  const LspServerSpec *byUnitb = registry.findByExtension(".unitb");
  ASSERT_NE(byUnitb, nullptr);
  EXPECT_EQ(byUnitb->id, customId);
  EXPECT_EQ(registry.findByExtension(".unita"), nullptr);
}

TEST(LspServerRegistryTest, UnknownExtensionReturnsNull) {
  auto &registry = LspServerRegistry::instance();

  constexpr const char *kUnknownExt = ".firmius_unknown_ext_for_lsp_registry_test";
  EXPECT_EQ(registry.findByExtension(kUnknownExt), nullptr);
  EXPECT_EQ(registry.findByPath(std::string("/tmp/unknown") + kUnknownExt), nullptr);
}

TEST(LspServerRegistryTest, DetectRootMarkerTraversalInTempDirs) {
  ScopedTempDir temp;

  const fs::path root = temp.path() / "workspace";
  const fs::path nested = root / "a" / "b" / "c";
  fs::create_directories(nested);

  {
    std::ofstream marker(root / "package.json");
    marker << "{}";
  }

  const fs::path startFile = nested / "main.ts";
  {
    std::ofstream source(startFile);
    source << "export {};";
  }

  const std::string detected =
      LspServerRegistry::detectRoot(startFile.string(), {"package.json", ".git"});
  EXPECT_EQ(fs::path(detected), root);
}
