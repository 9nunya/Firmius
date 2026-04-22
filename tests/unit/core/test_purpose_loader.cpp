#include "agents/PurposeLoader.hpp"
#include "Context.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

std::filesystem::path repoRootFromSourceFile() {
  auto path = std::filesystem::path(__FILE__).lexically_normal();
  for (int i = 0; i < 4; ++i) {
    path = path.parent_path();
  }
  return path;
}

std::string readRepoFile(const std::filesystem::path &path) {
  std::ifstream in(path);
  EXPECT_TRUE(in.good()) << path;
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

TEST(PromptContractsTest, basePromptRequiresNarrativeTextBetweenToolEpisodes) {
  const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "base.md");

  EXPECT_NE(prompt.find("Between tool-call episodes, emit concise plain-text progress or decision updates"),
            std::string::npos);
  EXPECT_NE(prompt.find("send it in a separate plain-text message between tool-call messages"),
            std::string::npos);
  EXPECT_NE(prompt.find("Only tools that exist in the current Firmius tool list are real."),
            std::string::npos);
  EXPECT_NE(prompt.find("`apply_patch` is not a Firmius tool and not a shell command in this harness."),
            std::string::npos);
  EXPECT_NE(prompt.find("Do not call `apply_patch` through `Process` with `action: \"Execute\"`."),
            std::string::npos);
  EXPECT_NE(prompt.find("Patch: Make `Edit` Feel Like Home"), std::string::npos);
  EXPECT_NE(prompt.find("mix `content` with line-range `edits` in one `Edit` call"),
            std::string::npos);
  EXPECT_NE(prompt.find("Mode Selection Heuristics"), std::string::npos);
}

TEST(PromptContractsTest, asterPromptRequiresAcceptanceBeforeDone) {
  const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "aster.md");

  EXPECT_NE(prompt.find("Not every task deserves a thread plan."), std::string::npos);
  EXPECT_NE(prompt.find("Use direct/todo lane when the task is:"), std::string::npos);
  EXPECT_NE(prompt.find("do not create a plan just to continue discovery"), std::string::npos);
  EXPECT_NE(prompt.find("if the next direct change is yours, do it directly instead of manufacturing ceremony"),
            std::string::npos);
  EXPECT_NE(prompt.find("Executor self-report is never acceptance."), std::string::npos);
  EXPECT_NE(prompt.find("routes must compile into continuation-fit work units"), std::string::npos);
  EXPECT_NE(prompt.find("after `Delegate` `Spawn`, you still own follow-through: `Wait`, review, accept/retry/recover"),
            std::string::npos);
  EXPECT_NE(prompt.find("stop only when user-facing control is resolved, todo is closed, and no runtime-owned work or review obligation remains"),
            std::string::npos);
}

TEST(PromptContractsTest, forgePromptRequiresVerificationEvidenceAndLeadAcceptance) {
  const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "forge.md");

  EXPECT_NE(prompt.find("you do not mark the chunk complete; Aster accepts, and Witness may challenge"),
            std::string::npos);
  EXPECT_NE(prompt.find("If it is not verified, it is not finished."),
            std::string::npos);
  EXPECT_NE(prompt.find("reread touched files and rerun needed verification after Ember returns"),
            std::string::npos);
  EXPECT_NE(prompt.find("Stop condition:"), std::string::npos);
  EXPECT_NE(prompt.find("verification state is named"), std::string::npos);
}

TEST(PromptContractsTest, basePromptDefinesContinuationTodoDoctrine) {
  const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "base.md");

  EXPECT_NE(prompt.find("Continuation-fit todo items are preferred:"), std::string::npos);
  EXPECT_NE(prompt.find("a good todo item can be advanced in one tight tool episode or short sequence"),
            std::string::npos);
  EXPECT_NE(prompt.find("the first item should be the next concrete action"),
            std::string::npos);
}

TEST(PromptContractsTest, glimmerPromptDefinesScoutLoop) {
  const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "glimmer.md");

  EXPECT_NE(prompt.find("# Scout Loop"), std::string::npos);
  EXPECT_NE(prompt.find("restate the bounded question in one sentence"),
            std::string::npos);
  EXPECT_NE(prompt.find("stop when the bounded uncertainty is actually reduced"),
            std::string::npos);
}

TEST(PromptContractsTest, harborPromptIsSwitchableRecoveryLead) {
  const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "harbor.md");

  EXPECT_NE(prompt.find("switchable: true"), std::string::npos);
  EXPECT_NE(prompt.find("canSpawn: true"), std::string::npos);
  EXPECT_NE(prompt.find("You are `Harbor`, keeper of continuity."), std::string::npos);
  EXPECT_NE(prompt.find("# Recovery Loop"), std::string::npos);
  EXPECT_NE(prompt.find("if cleanup is required, do the cleanup rather than writing about it"),
            std::string::npos);
}

TEST(PromptContractsTest, alternateLeadPromptsDefineTodoVsPlanTransitions) {
  const auto fastPrompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "fast.md");
  const auto plannerPrompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "meridian.md");
  const auto checkerPrompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "vellum.md");

  EXPECT_NE(fastPrompt.find("do NOT create plan/chunks for pure discovery or diagnosis"),
            std::string::npos);
  EXPECT_NE(fastPrompt.find("bring in Forge when the cut is real and executor-owned"),
            std::string::npos);
  EXPECT_NE(plannerPrompt.find("do not emit a route while key edit points or verification surfaces are still vibes"),
            std::string::npos);
  EXPECT_NE(checkerPrompt.find("reject routes that would predictably stall, loop, or summarize around unresolved work"),
            std::string::npos);
}

TEST(HintingContractsTest, basePromptDefendsAgainstAskingAndPrematureCompletion) {
  const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "base.md");

  EXPECT_NE(prompt.find("Critical rules:"),
            std::string::npos);
  EXPECT_NE(prompt.find("Runtime nudges are control signals, not decorative reminders."),
            std::string::npos);
  EXPECT_NE(prompt.find("`active-work-continuation` means runtime-owned work is still live"), std::string::npos);
  EXPECT_NE(prompt.find("`Files` with `action: \"Read\"`"), std::string::npos);
  EXPECT_NE(prompt.find("Only tools that exist in the current Firmius tool list are real."),
            std::string::npos);
  EXPECT_NE(prompt.find("`apply_patch` is not a Firmius tool and not a shell command in this harness."),
            std::string::npos);
  EXPECT_NE(prompt.find("Never bypass via `Process` with `action: \"Execute\"`"),
            std::string::npos);
  EXPECT_NE(prompt.find("Never mix `content` with line-range `edits`"),
            std::string::npos);
}

TEST(HintingContractsTest, basePromptDefendsAgainstOptimismAndNarration) {
  const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "base.md");

  EXPECT_NE(prompt.find("todo is not a notebook; it is a runtime contract"), std::string::npos);
  EXPECT_NE(prompt.find("if the same incomplete todo snapshot survives a runtime nudge, shrink or rewrite the item"),
            std::string::npos);
  EXPECT_NE(prompt.find("`Work` with `action: \"CreatePlan\" | \"ListPlans\" | \"GetPlan\" | \"UpdatePlan\" | \"ActivatePlan\" | \"AddChunk\" | \"ListChunks\" | \"GetChunk\" | \"UpdateChunk\" | \"ReadyChunk\"`"), std::string::npos);
  EXPECT_NE(prompt.find("a good todo item can be advanced in one tight tool episode or short sequence"),
            std::string::npos);
}

TEST(PromptContractsTest, basePromptRestrictsCategoryOverridesToUserRequests) {
  const auto prompt = readRepoFile(repoRootFromSourceFile() / "prompts" / "base.md");

  EXPECT_NE(prompt.find("Optional model routing category override."), std::string::npos);
  EXPECT_NE(prompt.find("Use it only when the user explicitly requested a specific route category."),
            std::string::npos);
  EXPECT_NE(prompt.find("Otherwise omit it so purpose/default routing applies."),
            std::string::npos);
}

} // namespace
