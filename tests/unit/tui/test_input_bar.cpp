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
