#include "TUIHotkeys.hpp"
#include "UserPreferences.hpp"
#include "utils/PlatformPaths.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

class UserPreferencesSkinTest : public ::testing::Test {
protected:
  std::filesystem::path temp_home_;
  std::optional<std::string> original_home_;

  void SetUp() override {
    if (const char *home = std::getenv("HOME")) {
      original_home_ = std::string(home);
    }
    temp_home_ = std::filesystem::temp_directory_path() /
                 ("firmius_tui_prefs_" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_home_);
    std::filesystem::create_directories(temp_home_ / ".firmius");
    setenv("HOME", temp_home_.c_str(), 1);
  }

  void TearDown() override {
    if (original_home_.has_value()) {
      setenv("HOME", original_home_->c_str(), 1);
    } else {
      unsetenv("HOME");
    }
    std::filesystem::remove_all(temp_home_);
  }
};

TEST_F(UserPreferencesSkinTest, SkinConfigRoundTripsExpandedSchema) {
  using namespace firmius::tui;

  UserPreferences prefs;
  prefs.skin_kind = SkinKind::Claudex;

  SkinConfig claudex = defaultSkinConfig(SkinKind::Claudex);
  claudex.show_turn_numbers = true;
  claudex.show_turn_footers = false;
  claudex.tool_display = SkinToolDisplayMode::Minimal;
  claudex.tool_results = SkinToolResultsMode::Collapsed;
  claudex.quick_tools_display = SkinQuickToolsDisplayMode::Hidden;
  claudex.status_bar_mode = SkinStatusBarMode::Hidden;
  claudex.glint_speed = SkinGlintSpeed::Fast;
  claudex.spinner_style = SkinSpinnerStyle::Braille;
  claudex.show_plan_inline = true;
  claudex.show_todo_inline = true;
  claudex.live_row_phrase_bank = "cheeky";
  claudex.live_row_mode = "persistent";
  claudex.live_row_cycle_seconds = 9;
  claudex.tool_result_preview_lines = 7;
  claudex.process_output_lines = 6;
  prefs.claudex_skin = claudex;

  SkinConfig firmius = defaultSkinConfig(SkinKind::Firmius);
  firmius.show_title_bar = false;
  firmius.show_agent_strip = false;
  firmius.show_work_panel = false;
  firmius.show_turn_timing = false;
  firmius.show_turn_tokens = false;
  firmius.show_live_footer = false;
  firmius.blank_lines_after_tools = false;
  firmius.show_tool_borders = false;
  firmius.show_tool_headers = false;
  firmius.show_tool_body = false;
  firmius.show_tool_icons = false;
  firmius.dim_tool_metadata = true;
  firmius.glint_enabled = false;
  firmius.glint_status_bar = false;
  firmius.diffs_default = SkinDiffDefaultMode::Collapsed;
  prefs.firmius_skin = firmius;

  saveUserPreferences(prefs);
  const auto loaded = loadUserPreferences();

  ASSERT_TRUE(loaded.skin_kind.has_value());
  EXPECT_EQ(*loaded.skin_kind, SkinKind::Claudex);

  ASSERT_TRUE(loaded.claudex_skin.has_value());
  EXPECT_EQ(loaded.claudex_skin->tool_display, SkinToolDisplayMode::Minimal);
  EXPECT_EQ(loaded.claudex_skin->tool_results, SkinToolResultsMode::Collapsed);
  EXPECT_EQ(loaded.claudex_skin->quick_tools_display,
            SkinQuickToolsDisplayMode::Hidden);
  EXPECT_EQ(loaded.claudex_skin->status_bar_mode, SkinStatusBarMode::Hidden);
  EXPECT_EQ(loaded.claudex_skin->glint_speed, SkinGlintSpeed::Fast);
  EXPECT_EQ(loaded.claudex_skin->spinner_style, SkinSpinnerStyle::Braille);
  EXPECT_TRUE(loaded.claudex_skin->show_plan_inline);
  EXPECT_TRUE(loaded.claudex_skin->show_todo_inline);
  EXPECT_EQ(loaded.claudex_skin->live_row_phrase_bank, "cheeky");
  EXPECT_EQ(loaded.claudex_skin->live_row_mode, "persistent");
  EXPECT_EQ(loaded.claudex_skin->live_row_cycle_seconds, 9);
  EXPECT_EQ(loaded.claudex_skin->tool_result_preview_lines, 7);
  EXPECT_EQ(loaded.claudex_skin->process_output_lines, 6);

  ASSERT_TRUE(loaded.firmius_skin.has_value());
  EXPECT_FALSE(loaded.firmius_skin->show_title_bar);
  EXPECT_FALSE(loaded.firmius_skin->show_agent_strip);
  EXPECT_FALSE(loaded.firmius_skin->show_work_panel);
  EXPECT_FALSE(loaded.firmius_skin->show_turn_timing);
  EXPECT_FALSE(loaded.firmius_skin->show_turn_tokens);
  EXPECT_FALSE(loaded.firmius_skin->show_live_footer);
  EXPECT_FALSE(loaded.firmius_skin->blank_lines_after_tools);
  EXPECT_FALSE(loaded.firmius_skin->show_tool_borders);
  EXPECT_FALSE(loaded.firmius_skin->show_tool_headers);
  EXPECT_FALSE(loaded.firmius_skin->show_tool_body);
  EXPECT_FALSE(loaded.firmius_skin->show_tool_icons);
  EXPECT_TRUE(loaded.firmius_skin->dim_tool_metadata);
  EXPECT_FALSE(loaded.firmius_skin->glint_enabled);
  EXPECT_FALSE(loaded.firmius_skin->glint_status_bar);
  EXPECT_EQ(loaded.firmius_skin->diffs_default,
            SkinDiffDefaultMode::Collapsed);
}

} // namespace

TEST_F(UserPreferencesSkinTest, ValidationRejectsImpossiblePanelValues) {
  firmius::tui::UserPreferences prefs;
  prefs.agent_strip_rows = 0;
  prefs.work_panel_height = 2;
  const auto warnings = firmius::tui::validateUserPreferences(prefs);
  EXPECT_EQ(warnings.size(), 2u);
}

TEST_F(UserPreferencesSkinTest, DefaultHotkeyConfigIsSeededUnderFirmiusHome) {
  const auto bindings = firmius::tui::DefaultHotkeyBindings();
  EXPECT_FALSE(bindings.empty());
}
