#include "components/FileEditToolBlock.hpp"
#include "ThemeManager.hpp"
#include <gtest/gtest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/screen/screen.hpp>

namespace firmius::tui {

class FileEditToolBlockTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Initialize ThemeManager if needed
  }
};

TEST_F(FileEditToolBlockTest, FailedEditRendersAttemptedLines) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "file_edit";
  view->args = R"({
    "path": "test.cpp",
    "edits": [
      {
        "op": "insert_after",
        "anchor": "1#abc",
        "new_lines": ["// test line"]
      }
    ]
  })";
  view->result = R"({
    "success": false,
    "error": "Failed to find anchor",
    "operations": [
      {
        "op": "insert_after",
        "description": "1#abc",
        "error": "Anchor not found: 1#abc"
      }
    ]
  })";
  view->success = false;
  view->phase = ToolPhase::Finished;

  auto component = FileEditToolBlock(view);
  auto element = component->Render();

  // In a real TUI test we might use a Screen to render and check for strings,
  // but here we just verify it doesn't crash and we could inspect the element tree.
  ftxui::Screen screen(100, 20);
  ftxui::Render(screen, element);
  std::string output = screen.ToString();

  // Check for expected content in error rendering
  EXPECT_TRUE(output.find("test.cpp") != std::string::npos);
  EXPECT_TRUE(output.find("failed") != std::string::npos);
  EXPECT_TRUE(output.find("Failed to find anchor") != std::string::npos);
  EXPECT_TRUE(output.find("insert_after 1#abc") != std::string::npos);
  EXPECT_TRUE(output.find("Anchor not found: 1#abc") != std::string::npos);
  EXPECT_TRUE(output.find("// test line") != std::string::npos);
}

TEST_F(FileEditToolBlockTest, HydratedFailedEditRendersAttemptedLines) {
  // Simulate reloaded state where phase might be Error or Finished with success=false
  auto view = std::make_shared<ToolCallView>();
  view->name = "file_edit";
  view->args = R"({
    "path": "test.cpp",
    "edits": [
      {
        "op": "replace_range",
        "start_anchor": "1#abc",
        "end_anchor": "2#def",
        "new_lines": ["// replacement"]
      }
    ]
  })";
  view->result = "Historical error text"; // Sometimes result is just a string in reloads if not careful
  view->success = false;
  view->phase = ToolPhase::Finished;

  auto component = FileEditToolBlock(view);
  auto element = component->Render();

  ftxui::Screen screen(100, 20);
  ftxui::Render(screen, element);
  std::string output = screen.ToString();

  EXPECT_TRUE(output.find("test.cpp") != std::string::npos);
  EXPECT_TRUE(output.find("// replacement") != std::string::npos);
}

TEST_F(FileEditToolBlockTest, SuccessfulReplaceRendersActualOldAndNewLines) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "file_edit";
  view->args = R"({
    "path": "test.cpp",
    "edits": [
      {
        "op": "replace_range",
        "start_anchor": "2#abc",
        "end_anchor": "2#abc",
        "new_lines": ["new_value();"]
      }
    ]
  })";
  view->result = R"({
    "path": "test.cpp",
    "mode": "hashline_edits",
    "operations": [
      {
        "op": "replace_range",
        "description": "replace 2#abc...2#abc",
        "start_line": 2,
        "end_line": 2,
        "relocated": false,
        "old_lines": ["old_value();"],
        "new_lines": ["new_value();"]
      }
    ]
  })";
  view->success = true;
  view->phase = ToolPhase::Finished;

  auto component = FileEditToolBlock(view);
  auto element = component->Render();

  ftxui::Screen screen(100, 20);
  ftxui::Render(screen, element);
  std::string output = screen.ToString();

  EXPECT_TRUE(output.find("old_value") != std::string::npos);
  EXPECT_TRUE(output.find("new_value") != std::string::npos);
  EXPECT_TRUE(output.find("line 2 removed") == std::string::npos);
}

} // namespace firmius::tui
