#include "utils/ModelUtil.hpp"
#include <gtest/gtest.h>

using namespace firmius::shared;

TEST(ModelUtilTest, PrettifyNames) {
  // Basic
  EXPECT_EQ(PrettifyModelName("gemini-3-flash"), "Gemini 3 Flash");
  EXPECT_EQ(PrettifyModelName("claude-3-opus"), "Claude 3 Opus");
  EXPECT_EQ(PrettifyModelName("gpt-4o"), "GPT 4o");
  EXPECT_EQ(PrettifyModelName("gpt-4-turbo"), "GPT 4 Turbo");

  // Organization prefix
  EXPECT_EQ(PrettifyModelName("stepfun-ai/step-3.5-flash"), "Step 3.5 Flash");
  EXPECT_EQ(PrettifyModelName("google/gemini-1.5-pro"), "Gemini 1.5 Pro");
  EXPECT_EQ(PrettifyModelName("openai/gpt-4o-mini"), "GPT 4o Mini");
  EXPECT_EQ(PrettifyModelName("anthropic/claude-3.5-sonnet"),
            "Claude 3.5 Sonnet");

  // Complex versions and thinking models
  EXPECT_EQ(PrettifyModelName("claude-opus-4-6-thinking"),
            "Claude Opus 4.6 Thinking"); // Based on prompt request
  EXPECT_EQ(PrettifyModelName("gemini-3.1-pro"), "Gemini 3.1 Pro");
  EXPECT_EQ(PrettifyModelName("gpt-5.4"), "GPT 5.4");
  EXPECT_EQ(PrettifyModelName("gpt-5.4-mini"), "GPT 5.4 Mini");
  EXPECT_EQ(PrettifyModelName("gpt-5.3-codex"), "GPT 5.3 Codex");
  EXPECT_EQ(PrettifyModelName("gpt-5.2-codex"), "GPT 5.2 Codex");
  EXPECT_EQ(PrettifyModelName("gpt-5.1-codex-max"), "GPT 5.1 Codex Max");
  EXPECT_EQ(PrettifyModelName("gpt-5.1-codex-mini"), "GPT 5.1 Codex Mini");

  // Various 30+ tests
  EXPECT_EQ(PrettifyModelName("qwen-max"), "Qwen Max");
  EXPECT_EQ(PrettifyModelName("deepseek-chat"), "Deepseek Chat");
  EXPECT_EQ(PrettifyModelName("mistral-large-latest"), "Mistral Large Latest");
  EXPECT_EQ(PrettifyModelName("llama-3-70b-instruct"), "Llama 3.70b Instruct");
  EXPECT_EQ(PrettifyModelName("mixtral-8x7b-v0.1"), "Mixtral 8x7b V0.1");
  EXPECT_EQ(PrettifyModelName("command-r-plus"), "Command R Plus");
  EXPECT_EQ(PrettifyModelName("stable-diffusion-xl-1.0"),
            "Stable Diffusion Xl 1.0");
  EXPECT_EQ(PrettifyModelName("codellama-34b-python"), "Codellama 34b Python");
  EXPECT_EQ(PrettifyModelName("phi-3-mini-4k-instruct"),
            "Phi 3 Mini 4k Instruct");
  EXPECT_EQ(PrettifyModelName("gemma-7b-it"), "Gemma 7b It");
  EXPECT_EQ(PrettifyModelName("falcon-180b-chat"), "Falcon 180b Chat");
  EXPECT_EQ(PrettifyModelName("yi-34b-chat"), "Yi 34b Chat");
  EXPECT_EQ(PrettifyModelName("baichuan-2-13b-chat"), "Baichuan 2.13b Chat");
  EXPECT_EQ(PrettifyModelName("glm-4"), "Glm 4");
  EXPECT_EQ(PrettifyModelName("solar-10.7b-instruct"), "Solar 10.7b Instruct");
  EXPECT_EQ(PrettifyModelName("starCoder-2"), "StarCoder 2");
  EXPECT_EQ(PrettifyModelName("openhermes-2.5-mistral-7b"),
            "Openhermes 2.5 Mistral 7b");
  EXPECT_EQ(PrettifyModelName("dolphin-2.6-mixtral-8x7b"),
            "Dolphin 2.6 Mixtral 8x7b");
  EXPECT_EQ(PrettifyModelName("nous-hermes-llama-2-7b"),
            "Nous Hermes Llama 2.7b");
  EXPECT_EQ(PrettifyModelName("zephyr-7b-beta"), "Zephyr 7b Beta");
  EXPECT_EQ(PrettifyModelName("neural-chat-7b-v3.3"), "Neural Chat 7b V3.3");
  EXPECT_EQ(PrettifyModelName("open-adams-1.0"), "Open Adams 1.0");
  EXPECT_EQ(PrettifyModelName("tiny-llama-1.1b"), "Tiny Llama 1.1b");
  EXPECT_EQ(PrettifyModelName("stable-code-3b"), "Stable Code 3b");
  EXPECT_EQ(PrettifyModelName("granite-3.0-code"), "Granite 3.0 Code");
  EXPECT_EQ(PrettifyModelName("super-model-v9000-ultra"),
            "Super Model V9000 Ultra");
}
