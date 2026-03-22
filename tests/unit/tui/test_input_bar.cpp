#include "components/InputBar.hpp"

#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(InputBar, ShiftEnterSequencesAreRecognizedAsNewlineInput) {
  EXPECT_TRUE(IsShiftEnterInput("\x1b[13;2u"));
  EXPECT_TRUE(IsShiftEnterInput("\x1b\r"));
  EXPECT_TRUE(IsShiftEnterInput("\x1b\n"));
  EXPECT_TRUE(IsShiftEnterInput("\x1b[27;2;13~"));
}

TEST(InputBar, PlainEnterSequenceIsNotTreatedAsShiftEnter) {
  EXPECT_FALSE(IsShiftEnterInput("\r"));
  EXPECT_FALSE(IsShiftEnterInput("\n"));
  EXPECT_FALSE(IsShiftEnterInput(""));
}

TEST(InputBar, DetectsFileReferenceAutocompleteStateForAtPrefix) {
  const std::string buffer = "inspect @src/file.ts";
  const auto state = DetectAtReferenceAutocompleteState(
      buffer, static_cast<int>(buffer.size()));
  EXPECT_TRUE(state.active);
  EXPECT_FALSE(state.is_artifact);
  EXPECT_EQ(state.token_prefix, "@");
  EXPECT_EQ(state.query, "src/file.ts");
}

TEST(InputBar, DetectsArtifactReferenceAutocompleteStateForArtifactPrefix) {
  const std::string buffer = "review @artifact:planner/REPORT.md";
  const auto state = DetectAtReferenceAutocompleteState(
      buffer, static_cast<int>(buffer.size()));
  EXPECT_TRUE(state.active);
  EXPECT_TRUE(state.is_artifact);
  EXPECT_EQ(state.token_prefix, "@artifact:");
  EXPECT_EQ(state.query, "planner/REPORT.md");
}

TEST(InputBar, DoesNotActivateAutocompleteInsideWord) {
  const std::string buffer = "email test@example.com";
  const auto state = DetectAtReferenceAutocompleteState(
      buffer, static_cast<int>(buffer.size()));
  EXPECT_FALSE(state.active);
}
