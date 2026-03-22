#include "ThemeManager.hpp"
#include "components/ErrorDisplay.hpp"
#include <gtest/gtest.h>

namespace {

using firmius::tui::ParseErrorDetails;

std::string renderToString(ftxui::Element element, int width = 100,
                           int height = 18) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  Render(screen, element);
  return screen.ToString();
}

TEST(ErrorDisplayTest, ParsesSummaryAndMetadataAndRawBodyJson) {
  const auto parsed = ParseErrorDetails(
      "Provider stream error: Quota exhausted or rate limited. Switching to next account... (HTTP 429)\n"
      "Provider: qwen\n"
      "Raw provider body:\n"
      "{\"error\":{\"code\":\"insufficient_quota\",\"message\":\"You exceeded your current quota\"}}");

  EXPECT_EQ(parsed.headline,
            "Provider stream error: Quota exhausted or rate limited. Switching to next account... (HTTP 429)");
  EXPECT_FALSE(parsed.metadata.empty());
  EXPECT_EQ(parsed.metadata[0].label, "Provider");
  EXPECT_EQ(parsed.metadata[0].content, "qwen");
  EXPECT_TRUE(parsed.has_json);
  EXPECT_NE(parsed.pretty_json.find("\"insufficient_quota\""), std::string::npos);
}

TEST(ErrorDisplayTest, ParsesErrorWithExtraTrailingDetails) {
  const auto parsed = ParseErrorDetails(
      "Provider stream error: request failed.\n"
      "Provider: openrouter\n"
      "Model: gpt-4\n"
      "Raw provider body:\n"
      "{\"error\":{\"type\":\"invalid_request_error\",\"message\":\"Bad JSON\"}}\n"
      "Model: gpt-4 No alternate account available after failure.");

  EXPECT_EQ(parsed.headline, "Provider stream error: request failed.");
  EXPECT_EQ(parsed.metadata.size(), 2u);
  EXPECT_EQ(parsed.metadata[0].label, "Provider");
  EXPECT_EQ(parsed.metadata[0].content, "openrouter");
  EXPECT_EQ(parsed.metadata[1].label, "Model");
  EXPECT_EQ(parsed.metadata[1].content, "gpt-4");
  EXPECT_TRUE(parsed.has_json);
  EXPECT_NE(parsed.pretty_json.find("\"invalid_request_error\""), std::string::npos);
  EXPECT_FALSE(parsed.trailing_details.empty());
}

TEST(ErrorDisplayTest, LeavesPlaintextUntouchedWhenJsonMissing) {
  const auto parsed = ParseErrorDetails(
      "Provider stream error: No alternate account available after failure.");

  EXPECT_FALSE(parsed.has_json);
  EXPECT_EQ(parsed.headline,
            "Provider stream error: No alternate account available after failure.");
  EXPECT_TRUE(parsed.metadata.empty());
  EXPECT_TRUE(parsed.raw_body_content.empty());
}

TEST(ErrorDisplayTest, PrettyRendersEmbeddedJsonBlock) {
  const auto parsed = ParseErrorDetails(
      "Something went wrong\n"
      "Raw provider body:\n"
      "{\"error\":{\"code\":\"rate_limit\",\"message\":\"Try later\",\"param\":null}}");

  EXPECT_TRUE(parsed.has_json);
  EXPECT_NE(parsed.pretty_json.find("\"rate_limit\""), std::string::npos);
  EXPECT_NE(parsed.pretty_json.find('\n'), std::string::npos);
}

TEST(ErrorDisplayTest, RendersStructuredBlockWithPrettyPayload) {
  firmius::shared::ErrorContent error;
  error.errorName = "Provider Runtime Error";
  error.description = "The request failed upstream.";
  error.details =
      "HTTP 429 from provider\n"
      "Provider: qwen\n"
      "Raw provider body:\n"
      "{\"error\":{\"type\":\"rate_limit\",\"message\":\"Try later\"}}";

  auto theme = firmius::tui::ThemeManager::instance().getCurrentTheme();
  auto element = firmius::tui::RenderErrorDisplay(theme, error);
  ASSERT_TRUE(static_cast<bool>(element));
}

TEST(ErrorDisplayTest, RendersNoticeCardWithoutErrorPrefix) {
  firmius::shared::NoticeContent notice;
  notice.title = "Agent Cancelled";
  notice.message = "The agent execution was interrupted.";
  notice.details = "Execution stopped before completion and can be resumed.";
  notice.severity = firmius::shared::NoticeSeverity::Warning;

  auto theme = firmius::tui::ThemeManager::instance().getCurrentTheme();
  auto output = renderToString(firmius::tui::RenderNoticeDisplay(theme, notice));
  EXPECT_NE(output.find("Agent Cancelled"), std::string::npos);
  EXPECT_EQ(output.find("* "), std::string::npos);
}

TEST(ErrorDisplayTest, HandlesEmptyDetails) {
  const auto parsed = ParseErrorDetails("");
  EXPECT_TRUE(parsed.headline.empty());
  EXPECT_FALSE(parsed.has_json);
  EXPECT_TRUE(parsed.metadata.empty());
}

TEST(ErrorDisplayTest, HandlesNonJsonRawBody) {
  const auto parsed = ParseErrorDetails(
      "Connection failed\n"
      "Provider: openrouter\n"
      "Raw provider body:\n"
      "Internal Server Error");

  EXPECT_EQ(parsed.headline, "Connection failed");
  EXPECT_EQ(parsed.metadata[0].label, "Provider");
  EXPECT_FALSE(parsed.has_json);
  EXPECT_EQ(parsed.raw_body_content, "Internal Server Error");
}

} // namespace
