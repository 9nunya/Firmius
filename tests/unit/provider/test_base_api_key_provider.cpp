#include "providers/BaseAPIKeyProvider.hpp"

#include <gtest/gtest.h>

using firmius::provider::SimpleAPIKeyWizard;

TEST(SimpleAPIKeyWizardTest, ExposesSecretPromptMetadataForModalRendering) {
  SimpleAPIKeyWizard wizard;

  const auto prompt = wizard.nextPrompt();
  ASSERT_TRUE(prompt.has_value());
  EXPECT_EQ(prompt->message, "Enter your API key:");
  EXPECT_TRUE(prompt->isSecret);
  EXPECT_TRUE(prompt->allowFreeformInput);
  EXPECT_FALSE(prompt->allowEmptyInput);
  EXPECT_TRUE(prompt->choices.empty());
  EXPECT_EQ(prompt->placeholder, "sk-...");
  EXPECT_EQ(prompt->submitLabel, "Save Key");

  EXPECT_FALSE(wizard.nextPrompt().has_value());
}

TEST(SimpleAPIKeyWizardTest, FinalizeRequiresNonEmptyApiKey) {
  SimpleAPIKeyWizard wizard;
  ASSERT_TRUE(wizard.nextPrompt().has_value());

  wizard.submitAnswer("");
  EXPECT_TRUE(wizard.isComplete());

  std::string apiKey;
  std::string error;
  EXPECT_FALSE(wizard.finalizeExchange(apiKey, error));
  EXPECT_TRUE(apiKey.empty());
  EXPECT_EQ(error, "API key cannot be empty.");
}

TEST(SimpleAPIKeyWizardTest, FinalizeReturnsSubmittedApiKey) {
  SimpleAPIKeyWizard wizard;
  ASSERT_TRUE(wizard.nextPrompt().has_value());

  wizard.submitAnswer("sk-test-123");
  EXPECT_TRUE(wizard.isComplete());

  std::string apiKey;
  std::string error;
  EXPECT_TRUE(wizard.finalizeExchange(apiKey, error));
  EXPECT_EQ(apiKey, "sk-test-123");
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(wizard.getFinalMessage(), "API key successfully added!");
}
