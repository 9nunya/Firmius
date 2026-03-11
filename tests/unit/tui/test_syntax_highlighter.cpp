#include <gtest/gtest.h>
#include "components/SyntaxHighlighter.hpp"
#include <ftxui/dom/elements.hpp>
#include <algorithm>

namespace firmius::tui {

class SyntaxHighlighterTest : public ::testing::Test {
protected:
  void SetUp() override {
    highlighter_.initialize();
  }
  
  void TearDown() override {}
  
  SyntaxHighlighter& highlighter_ = SyntaxHighlighter::instance();
};

TEST_F(SyntaxHighlighterTest, InstanceSingleton) {
  auto& inst1 = SyntaxHighlighter::instance();
  auto& inst2 = SyntaxHighlighter::instance();
  EXPECT_EQ(&inst1, &inst2);
}

TEST_F(SyntaxHighlighterTest, DetectLanguageFromExtension) {
  EXPECT_EQ(highlighter_.detectLanguage("test.cpp"), "cpp");
  EXPECT_EQ(highlighter_.detectLanguage("main.rs"), "rust");
  EXPECT_EQ(highlighter_.detectLanguage("script.py"), "python");
  EXPECT_EQ(highlighter_.detectLanguage("app.js"), "javascript");
  EXPECT_EQ(highlighter_.detectLanguage("config.json"), "json");
}

TEST_F(SyntaxHighlighterTest, DetectLanguageUnknown) {
  EXPECT_EQ(highlighter_.detectLanguage("test.xyz"), "");
  EXPECT_EQ(highlighter_.detectLanguage(""), "");
}

TEST_F(SyntaxHighlighterTest, HasGrammar) {
  EXPECT_TRUE(highlighter_.hasGrammar("cpp"));
  EXPECT_TRUE(highlighter_.hasGrammar("rust"));
  EXPECT_TRUE(highlighter_.hasGrammar("python"));
  EXPECT_TRUE(highlighter_.hasGrammar("javascript"));
  EXPECT_FALSE(highlighter_.hasGrammar("nonexistent"));
}

TEST_F(SyntaxHighlighterTest, GetAvailableLanguages) {
  auto langs = highlighter_.getAvailableLanguages();
  EXPECT_FALSE(langs.empty());
  
  bool has_cpp = false;
  bool has_python = false;
  for (const auto& lang : langs) {
    if (lang == "cpp") has_cpp = true;
    if (lang == "python") has_python = true;
  }
  EXPECT_TRUE(has_cpp);
  EXPECT_TRUE(has_python);
}

TEST_F(SyntaxHighlighterTest, HighlightCppKeywords) {
  std::string code = "int main() { return 0; }";
  auto tokens = highlighter_.highlight(code, "cpp");
  
  EXPECT_FALSE(tokens.empty());
  
  // Check that 'int' is highlighted as a type
  bool foundInt = false;
  for (const auto& token : tokens) {
    if (token.text == "int") {
      foundInt = true;
      EXPECT_EQ(token.type, TokenType::Type);
    }
  }
  EXPECT_TRUE(foundInt);
}

TEST_F(SyntaxHighlighterTest, HighlightCppStrings) {
  std::string code = "const char* msg = \"Hello World\";";
  auto tokens = highlighter_.highlight(code, "cpp");
  
  bool foundString = false;
  for (const auto& token : tokens) {
    if (token.text == "\"Hello World\"") {
      foundString = true;
      EXPECT_EQ(token.type, TokenType::String);
    }
  }
  EXPECT_TRUE(foundString);
}

TEST_F(SyntaxHighlighterTest, HighlightCppComments) {
  std::string code = "// This is a comment\nint x = 5;";
  auto tokens = highlighter_.highlight(code, "cpp");
  
  bool foundComment = false;
  for (const auto& token : tokens) {
    if (token.text.find("//") != std::string::npos) {
      foundComment = true;
      EXPECT_EQ(token.type, TokenType::Comment);
    }
  }
  EXPECT_TRUE(foundComment);
}

TEST_F(SyntaxHighlighterTest, HighlightPythonKeywords) {
  std::string code = "def hello():\n    return True";
  auto tokens = highlighter_.highlight(code, "python");
  
  bool foundDef = false;
  bool foundTrue = false;
  
  for (const auto& token : tokens) {
    if (token.text == "def") {
      foundDef = true;
      EXPECT_EQ(token.type, TokenType::Keyword);
    }
    // True might be highlighted as Keyword or Constant depending on implementation
    if (token.text == "True") {
      foundTrue = true;
      EXPECT_TRUE(token.type == TokenType::Constant || token.type == TokenType::Keyword);
    }
  }
  
  EXPECT_TRUE(foundDef);
  EXPECT_TRUE(foundTrue);
}

TEST_F(SyntaxHighlighterTest, HighlightRustKeywords) {
  std::string code = "fn main() -> Result<(), Error> { Ok(()) }";
  auto tokens = highlighter_.highlight(code, "rust");
  
  bool foundFn = false;
  for (const auto& token : tokens) {
    if (token.text == "fn") {
      foundFn = true;
      EXPECT_EQ(token.type, TokenType::Keyword);
    }
  }
  EXPECT_TRUE(foundFn);
}

TEST_F(SyntaxHighlighterTest, HighlightNumbers) {
  std::string code = "let x = 42; let y = 3.14;";
  auto tokens = highlighter_.highlight(code, "rust");
  
  bool found42 = false;
  bool foundPi = false;
  
  for (const auto& token : tokens) {
    if (token.text == "42") {
      found42 = true;
      EXPECT_EQ(token.type, TokenType::Number);
    }
    if (token.text == "3.14") {
      foundPi = true;
      EXPECT_EQ(token.type, TokenType::Number);
    }
  }
  
  EXPECT_TRUE(found42);
  EXPECT_TRUE(foundPi);
}

TEST_F(SyntaxHighlighterTest, HighlightRender) {
  std::string code = "int main() { return 0; }";
  auto element = highlighter_.highlightRender(code, "cpp", false);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(SyntaxHighlighterTest, HighlightRenderWithLineNumbers) {
  std::string code = "line 1\nline 2\nline 3";
  auto element = highlighter_.highlightRender(code, "cpp", true);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(SyntaxHighlighterTest, ColorScheme) {
  auto& scheme = highlighter_.getColorScheme();
  EXPECT_TRUE(scheme.keyword != ftxui::Color::Default);
  EXPECT_TRUE(scheme.string != ftxui::Color::Default);
  
  SyntaxColorScheme custom;
  custom.keyword = ftxui::Color::Red;
  highlighter_.setColorScheme(custom);
  EXPECT_EQ(highlighter_.getColorScheme().keyword, ftxui::Color::Red);
}

TEST_F(SyntaxHighlighterTest, HighlightEmptyCode) {
  std::string code = "";
  auto tokens = highlighter_.highlight(code, "cpp");
  // Empty code returns empty token list - verify it doesn't crash
  // and the vector is in valid state
  EXPECT_NO_THROW({
    for (const auto& token : tokens) {
      // Verify each token has valid type
      auto type = static_cast<int>(token.type);
      EXPECT_GE(type, 0);
      EXPECT_LE(type, 12); // Max TokenType value
    }
  });
}

TEST_F(SyntaxHighlighterTest, HighlightUnknownLanguage) {
  std::string code = "some code here";
  auto tokens = highlighter_.highlight(code, "unknown");
  // Should fallback to plain text
  EXPECT_FALSE(tokens.empty());
}

} // namespace firmius::tui
