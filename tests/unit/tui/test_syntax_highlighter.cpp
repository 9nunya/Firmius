#include "components/SyntaxHighlighter.hpp"
#include <algorithm>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace firmius::tui {

class SyntaxHighlighterTest : public ::testing::Test {
protected:
  void SetUp() override { highlighter_.initialize(); }

  void TearDown() override {}

  SyntaxHighlighter &highlighter_ = SyntaxHighlighter::instance();
};

TEST_F(SyntaxHighlighterTest, InstanceSingleton) {
  auto &inst1 = SyntaxHighlighter::instance();
  auto &inst2 = SyntaxHighlighter::instance();
  EXPECT_EQ(&inst1, &inst2);
}

TEST_F(SyntaxHighlighterTest, DetectLanguageFromExtension) {
  EXPECT_EQ(highlighter_.detectLanguage("test.cpp"), "cpp");
  EXPECT_EQ(highlighter_.detectLanguage("main.rs"), "rust");
  EXPECT_EQ(highlighter_.detectLanguage("script.py"), "python");
  EXPECT_EQ(highlighter_.detectLanguage("app.js"), "javascript");
  EXPECT_EQ(highlighter_.detectLanguage("config.json"), "json");
  EXPECT_EQ(highlighter_.detectLanguage("file.ts"), "typescript");
  EXPECT_EQ(highlighter_.detectLanguage("data.yaml"), "yaml");
  EXPECT_EQ(highlighter_.detectLanguage("config.toml"), "toml");
  EXPECT_EQ(highlighter_.detectLanguage("test.c"), "c");
  EXPECT_EQ(highlighter_.detectLanguage("Main.java"), "java");
  EXPECT_EQ(highlighter_.detectLanguage("script.sh"), "bash");
}

TEST_F(SyntaxHighlighterTest, DetectLanguageUnknown) {
  EXPECT_EQ(highlighter_.detectLanguage("test.xyz"), "");
  EXPECT_EQ(highlighter_.detectLanguage(""), "");
}

TEST_F(SyntaxHighlighterTest, GetAvailableLanguages) {
  auto langs = highlighter_.getAvailableLanguages();
  // All grammars are built-in, so list should not be empty
  EXPECT_FALSE(langs.empty());
  EXPECT_TRUE(std::find(langs.begin(), langs.end(), "cpp") != langs.end());
  EXPECT_TRUE(std::find(langs.begin(), langs.end(), "bash") != langs.end());
}

TEST_F(SyntaxHighlighterTest, HasGrammarInitiallyTrue) {
  // All grammars are built-in, so they should be available immediately
  EXPECT_TRUE(highlighter_.hasGrammar("cpp"));
  EXPECT_TRUE(highlighter_.hasGrammar("rust"));
  EXPECT_TRUE(highlighter_.hasGrammar("python"));
  EXPECT_TRUE(highlighter_.hasGrammar("javascript"));
  EXPECT_TRUE(highlighter_.hasGrammar("bash"));
  EXPECT_FALSE(highlighter_.hasGrammar("nonexistent"));
}

TEST_F(SyntaxHighlighterTest, GetGrammarInfo) {
  const auto *info = highlighter_.getGrammarInfo("cpp");
  EXPECT_NE(info, nullptr);
  EXPECT_EQ(info->name, "C++");
  // The function pointer for the TSLanguage should be set
  EXPECT_NE(info->languageFn, nullptr);
  EXPECT_TRUE(info->fileExtensions.size() > 0);
}

TEST_F(SyntaxHighlighterTest, HighlightRenderUnknownLanguage) {
  std::string code = "int main() { return 0; }";
  auto element = highlighter_.highlightRender(code, "unknown_lang", false);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(SyntaxHighlighterTest, HighlightRenderKnownLanguage) {
  // With built-in grammar, it should parse and render using AST
  std::string code = "fn main() { println!(\"Hello\"); }";
  auto element = highlighter_.highlightRender(code, "rust", false);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(SyntaxHighlighterTest, BashHighlighting) {
  std::string code = "if [ $x -eq 1 ]; then echo \"true\"; fi";
  auto element = highlighter_.highlightRenderWrappedLine(code, "bash");
  EXPECT_TRUE(element != nullptr);
  
  ftxui::Screen screen(80, 1);
  ftxui::Render(screen, element);
  std::string output = screen.ToString();
  EXPECT_NE(output.find("echo"), std::string::npos);
}

TEST_F(SyntaxHighlighterTest, HighlightRenderWithLineNumbers) {
  std::string code = "line 1\nline 2\nline 3";
  auto element = highlighter_.highlightRender(code, "cpp", true);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(SyntaxHighlighterTest, ColorScheme) {
  auto &scheme = highlighter_.getColorScheme();
  EXPECT_TRUE(scheme.keyword != ftxui::Color::Default);
  EXPECT_TRUE(scheme.string != ftxui::Color::Default);

  SyntaxColorScheme custom;
  custom.keyword = ftxui::Color::Red;
  highlighter_.setColorScheme(custom);
  EXPECT_EQ(highlighter_.getColorScheme().keyword, ftxui::Color::Red);
}

TEST_F(SyntaxHighlighterTest, HighlightEmptyCode) {
  std::string code = "";
  auto result = highlighter_.highlight(code, "cpp");
  // Empty code returns empty string
  EXPECT_TRUE(result.empty());
}

TEST_F(SyntaxHighlighterTest, FileExtensionMapping) {
  // Test all supported extensions map correctly
  std::vector<std::pair<std::string, std::string>> testCases = {
      {"test.c", "c"},
      {"header.h", "c"}, // .h defaults to c
      {"test.cpp", "cpp"},
      {"header.hpp", "cpp"},
      {"code.cc", "cpp"},
      {"code.cxx", "cpp"},
      {"test.rs", "rust"},
      {"script.py", "python"},
      {"script.pyw", "python"},
      {"app.js", "javascript"},
      {"module.jsx", "javascript"},
      {"script.mjs", "javascript"},
      {"app.ts", "typescript"},
      {"component.tsx", "typescript"},
      {"config.json", "json"},
      {"data.yaml", "yaml"},
      {"data.yml", "yaml"},
      {"config.toml", "toml"},
      {"Main.java", "java"},
      {"run.sh", "bash"},
      {"init.bash", "bash"},
      {"setup.zsh", "bash"},
  };

  for (const auto &[filename, expected] : testCases) {
    EXPECT_EQ(highlighter_.detectLanguage(filename), expected)
        << "Failed for: " << filename;
  }
}

} // namespace firmius::tui
