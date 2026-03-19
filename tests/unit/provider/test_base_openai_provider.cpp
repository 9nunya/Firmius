#include "providers/BaseOpenAIProvider.hpp"

#include <gtest/gtest.h>

using firmius::provider::BaseOpenAIProvider;
using firmius::shared::ModelInfo;

namespace {

class TestBaseOpenAIProvider : public BaseOpenAIProvider {
public:
  TestBaseOpenAIProvider()
      : BaseOpenAIProvider("test-openai", "https://example.invalid", "") {}

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = "test-model";
    model.provider = getId();
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }
};

} // namespace

TEST(BaseOpenAIProvider, FormatErrorMessageIncludesContextAndRawBody) {
  const std::string body = R"({"error":"rate_limited"})";

  const std::string message = TestBaseOpenAIProvider::formatErrorMessage(
      "openrouter", "gpt-test", 429, body, "API error");

  EXPECT_NE(message.find("API error (HTTP 429)"), std::string::npos);
  EXPECT_NE(message.find("Provider: openrouter"), std::string::npos);
  EXPECT_NE(message.find("Model: gpt-test"), std::string::npos);
  EXPECT_NE(message.find("Raw provider body:\n" + body), std::string::npos);
}
