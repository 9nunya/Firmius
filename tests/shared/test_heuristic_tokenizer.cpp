#include "HeuristicTokenizer.hpp"

#include <gtest/gtest.h>

using firmius::shared::HeuristicTokenizer;

TEST(HeuristicTokenizer, EmptyStringReturnsZero) {
  const HeuristicTokenizer tok;
  EXPECT_EQ(tok.count(""), 0u);
}

TEST(HeuristicTokenizer, SingleCharReturnsOne) {
  const HeuristicTokenizer tok;
  EXPECT_EQ(tok.count("a"), 1u);
}

TEST(HeuristicTokenizer, FourBytesReturnsOne) {
  const HeuristicTokenizer tok;
  EXPECT_EQ(tok.count("abcd"), 1u);
}

TEST(HeuristicTokenizer, FiveBytesReturnsTwo) {
  const HeuristicTokenizer tok;
  EXPECT_EQ(tok.count("abcde"), 2u);
}

TEST(HeuristicTokenizer, KnownLengths) {
  const HeuristicTokenizer tok;
  EXPECT_EQ(tok.count("hello world"), 3u);        // 11 bytes -> ceil(11/4) = 3
  EXPECT_EQ(tok.count(std::string(100, 'x')), 25u); // 100 bytes -> 25
  EXPECT_EQ(tok.count(std::string(1024, 'x')), 256u); // 1024 bytes -> 256
}

TEST(HeuristicTokenizer, IdReturnsHeuristic) {
  const HeuristicTokenizer tok;
  EXPECT_EQ(tok.id(), "heuristic");
}

TEST(HeuristicTokenizer, PolymorphicUse) {
  const firmius::shared::ITokenizer &tok = HeuristicTokenizer{};
  EXPECT_EQ(tok.count("test"), 1u);
  EXPECT_EQ(tok.id(), "heuristic");
}
