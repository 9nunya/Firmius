#include "Context.hpp"
#include "HeuristicTokenizer.hpp"
#include "agents/working_memory/DeflationArchive.hpp"
#include "agents/working_memory/Deflator.hpp"
#include "agents/working_memory/PinPolicy.hpp"
#include "agents/working_memory/WorkingMemory.hpp"
#include "agents/working_memory/WorkingMemoryWorker.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>

namespace wm = firmius::core::working_memory;
namespace fs = std::filesystem;
using firmius::shared::AgentContext;
using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::HeuristicTokenizer;
using firmius::shared::ImageContent;
using firmius::shared::Message;
using firmius::shared::MessagePart;
using firmius::shared::Role;
using firmius::shared::TextContent;
using firmius::shared::ToolCallContent;
using firmius::shared::ToolResultContent;

namespace {

AgentTurn userTurn(const std::string &id, const std::string &text) {
  AgentTurn t;
  t.turnId = id;
  Message m;
  m.role = Role::User;
  m.timestamp = 1;
  m.content.push_back(TextContent{text});
  t.messages.push_back(std::move(m));
  return t;
}

AgentTurn assistantText(const std::string &id, const std::string &text) {
  AgentTurn t;
  t.turnId = id;
  Message m;
  m.role = Role::Assistant;
  m.timestamp = 1;
  m.content.push_back(TextContent{text});
  t.messages.push_back(std::move(m));
  return t;
}

AgentTurn assistantToolCall(const std::string &id, const std::string &callId,
                            const std::string &toolName,
                            const std::string &args) {
  AgentTurn t;
  t.turnId = id;
  Message m;
  m.role = Role::Assistant;
  m.timestamp = 1;
  m.content.push_back(ToolCallContent{callId, toolName, args});
  t.messages.push_back(std::move(m));
  return t;
}

AgentTurn toolResult(const std::string &id, const std::string &callId,
                     const std::string &result, bool success = true) {
  AgentTurn t;
  t.turnId = id;
  Message m;
  m.role = Role::ToolResult;
  m.timestamp = 1;
  m.content.push_back(ToolResultContent{callId, result, success, "", ""});
  t.messages.push_back(std::move(m));
  return t;
}

AgentTurn imageTurn(const std::string &id) {
  AgentTurn t;
  t.turnId = id;
  Message m;
  m.role = Role::User;
  m.timestamp = 1;
  m.content.push_back(TextContent{"see image"});
  m.content.push_back(ImageContent{"data:image/png;base64,abc", "image/png", "auto"});
  t.messages.push_back(std::move(m));
  return t;
}

std::string longBody(std::size_t bytes) {
  std::string out;
  out.reserve(bytes);
  for (std::size_t i = 0; i < bytes; ++i) {
    out.push_back(static_cast<char>('a' + (i % 26)));
    if (i % 80 == 79) out.push_back('\n');
  }
  return out;
}

AgentContext makeContext() {
  AgentContext ctx;
  ctx.identity.id = "agent-1";
  ctx.history = std::make_shared<AgentHistory>();
  ctx.history->threadId = "test-thread";
  ctx.config.workingMemory.enabled = true;
  ctx.config.workingMemory.bufferOccupancyRatio = 0.10f;
  ctx.config.workingMemory.targetOccupancyRatio = 0.20f;
  ctx.config.workingMemory.emergencyOccupancyRatio = 0.30f;
  ctx.config.workingMemory.recencyTailRatio = 0.05f;
  ctx.config.workingMemory.minimumRecencyTailTokens = 200;
  ctx.config.workingMemory.deflationMinPartTokens = 100;
  ctx.config.workingMemory.defaultDeflationTurnHorizon = 2;
  ctx.config.workingMemory.embeddingsEnabled = true;
  ctx.config.workingMemory.embeddingTopK = 3;
  return ctx;
}

} // namespace

// ---------- PinPolicy ----------

TEST(PinPolicy, UserMessagesAlwaysHardPinned) {
  AgentHistory history;
  history.threadId = "t";
  history.turns.push_back(userTurn("u-1", "the original task"));
  history.turns.push_back(assistantText("a-1", "exploring..."));
  history.turns.push_back(assistantText("a-2", "more thinking"));
  history.turns.push_back(assistantText("a-3", "yet more"));

  HeuristicTokenizer tok;
  AgentContext ctx;
  wm::PinPolicyInputs pi;
  pi.recencyTailTokens = 0;
  auto cls = wm::classifyPins(history, ctx, pi, tok);

  ASSERT_EQ(cls.decisions.size(), 4u);
  EXPECT_EQ(cls.decisions[0].kind, wm::PinKind::HardPin);
  EXPECT_EQ(cls.decisions[0].reason, "user_message");
}

TEST(PinPolicy, ImagesAlwaysHardPinned) {
  AgentHistory history;
  history.turns.push_back(userTurn("u-1", "task"));
  history.turns.push_back(imageTurn("img-1"));
  for (int i = 0; i < 10; ++i) {
    history.turns.push_back(
        assistantText("a-" + std::to_string(i), "filler text " + std::to_string(i)));
  }

  HeuristicTokenizer tok;
  AgentContext ctx;
  wm::PinPolicyInputs pi;
  pi.recencyTailTokens = 0;
  auto cls = wm::classifyPins(history, ctx, pi, tok);

  // The image turn (index 1) is a Role::User turn anyway, so it'd be pinned
  // either way. To prove the image-specific path we synthesize a non-user
  // image-bearing turn and check.
  AgentHistory h2;
  AgentTurn t;
  t.turnId = "asst-img";
  Message m;
  m.role = Role::Assistant;
  m.timestamp = 1;
  m.content.push_back(TextContent{"here's the diagram"});
  m.content.push_back(ImageContent{"x", "image/png", "auto"});
  t.messages.push_back(m);
  h2.turns.push_back(std::move(t));
  for (int i = 0; i < 5; ++i) {
    h2.turns.push_back(assistantText("a-" + std::to_string(i), "filler"));
  }
  auto cls2 = wm::classifyPins(h2, ctx, pi, tok);
  EXPECT_EQ(cls2.decisions[0].kind, wm::PinKind::HardPin);
  EXPECT_EQ(cls2.decisions[0].reason, "image_part");
}

TEST(PinPolicy, RecencyTailHardPinned) {
  AgentHistory history;
  history.turns.push_back(userTurn("u-1", "task"));
  for (int i = 0; i < 20; ++i) {
    history.turns.push_back(assistantText("a-" + std::to_string(i),
                                          std::string(60, 'x')));
  }

  HeuristicTokenizer tok;
  AgentContext ctx;
  wm::PinPolicyInputs pi;
  // Each assistant turn ~ 15 tokens (60 chars / 4). Set tail budget so the
  // last few turns are pinned.
  pi.recencyTailTokens = 60;
  auto cls = wm::classifyPins(history, ctx, pi, tok);

  // Verify tail end is HardPin.
  EXPECT_EQ(cls.decisions.back().kind, wm::PinKind::HardPin);
  // Verify older turns in the middle are Evictable.
  bool sawEvictable = false;
  for (std::size_t i = 1; i + 5 < cls.decisions.size(); ++i) {
    if (cls.decisions[i].kind == wm::PinKind::Evictable) {
      sawEvictable = true;
      break;
    }
  }
  EXPECT_TRUE(sawEvictable);
}

TEST(PinPolicy, ToolCallResultPairingPreservedWhenOnePartnerIsPinned) {
  AgentHistory history;
  history.turns.push_back(userTurn("u-1", "task"));
  history.turns.push_back(assistantToolCall("a-1", "call-1", "grep", "{}"));
  history.turns.push_back(toolResult("tr-1", "call-1", "result body"));
  // Several filler turns to force the older pair into the evictable region.
  for (int i = 0; i < 5; ++i) {
    history.turns.push_back(assistantText("filler-" + std::to_string(i), "x"));
  }
  // Add a final tool_result whose tool_call partner sits older. We pin
  // this final turn via the recency tail.
  history.turns.push_back(assistantToolCall("a-2", "call-2", "read", "{}"));
  history.turns.push_back(toolResult("tr-2", "call-2", "result-2"));

  HeuristicTokenizer tok;
  AgentContext ctx;
  wm::PinPolicyInputs pi;
  pi.recencyTailTokens = 100; // covers the final two turns

  auto cls = wm::classifyPins(history, ctx, pi, tok);

  // Find decisions for "a-1" and "tr-1" — they should NOT be Evictable
  // because pairing semantics propagate when one side is pinned. Here both
  // are mutually pinning each other; with no external pinning of either,
  // they remain Evictable. We verify the more interesting case: the
  // recency-tail-pinned tool_result (tr-2) drags its partner a-2 with it.
  std::size_t idxA2 = 0, idxTr2 = 0;
  for (std::size_t i = 0; i < cls.decisions.size(); ++i) {
    if (cls.decisions[i].turnId == "a-2") idxA2 = i;
    if (cls.decisions[i].turnId == "tr-2") idxTr2 = i;
  }
  EXPECT_NE(cls.decisions[idxTr2].kind, wm::PinKind::Evictable);
  EXPECT_NE(cls.decisions[idxA2].kind, wm::PinKind::Evictable);
}

// ---------- DeflationArchive ----------

TEST(DeflationArchive, RoundTripsBodies) {
  const std::string base = (fs::temp_directory_path() /
                            ("firmius_def_arch_" +
                             std::to_string(std::random_device{}())))
                               .string();
  fs::create_directories(base);
  wm::DeflationArchive arch(base);
  const std::string id = arch.mintId("thread-x");
  const std::string body = longBody(2048);
  arch.put(id, body);
  EXPECT_TRUE(arch.has(id));
  auto roundTrip = arch.get(id);
  ASSERT_TRUE(roundTrip.has_value());
  EXPECT_EQ(*roundTrip, body);
  EXPECT_GT(arch.totalBytesOnDisk(), 0u);
  arch.remove(id);
  EXPECT_FALSE(arch.has(id));
  fs::remove_all(base);
}

// ---------- Deflator ----------

TEST(Deflator, SkipsHardPinnedTurns) {
  AgentHistory history;
  history.threadId = "t";
  for (int i = 0; i < 5; ++i) {
    history.turns.push_back(assistantToolCall(
        "a-" + std::to_string(i), "c-" + std::to_string(i), "grep", "{}"));
    history.turns.push_back(
        toolResult("tr-" + std::to_string(i), "c-" + std::to_string(i),
                   longBody(2000)));
  }

  HeuristicTokenizer tok;
  std::vector<bool> hardPinMask(history.turns.size(), false);
  // Mark all turns hard-pinned.
  for (std::size_t i = 0; i < hardPinMask.size(); ++i) hardPinMask[i] = true;

  wm::DeflationSelectorInputs sel;
  sel.minPartTokens = 50;
  sel.defaultHorizon = 1;
  sel.hardPinMask = &hardPinMask;
  auto cands = wm::selectDeflationCandidates(history, tok, sel);
  EXPECT_TRUE(cands.empty()) << "Hard-pinned turns must never produce candidates";
}

TEST(Deflator, DeflatesLargeOldToolResults) {
  AgentHistory history;
  history.threadId = "t";
  for (int i = 0; i < 8; ++i) {
    history.turns.push_back(assistantToolCall(
        "a-" + std::to_string(i), "c-" + std::to_string(i), "grep", "{}"));
    history.turns.push_back(
        toolResult("tr-" + std::to_string(i), "c-" + std::to_string(i),
                   longBody(2000)));
  }

  HeuristicTokenizer tok;
  std::vector<bool> hardPinMask(history.turns.size(), false);
  // Pin only the very newest pair (the recency tail).
  hardPinMask[history.turns.size() - 1] = true;
  hardPinMask[history.turns.size() - 2] = true;

  wm::DeflationSelectorInputs sel;
  sel.minPartTokens = 50;
  sel.defaultHorizon = 2;
  sel.hardPinMask = &hardPinMask;

  auto cands = wm::selectDeflationCandidates(history, tok, sel);
  EXPECT_FALSE(cands.empty());

  const std::string base = (fs::temp_directory_path() /
                            ("firmius_def_test_" +
                             std::to_string(std::random_device{}())))
                               .string();
  fs::create_directories(base);
  wm::DeflationArchive archive(base);

  const auto result = wm::deflateCandidates(history, archive, cands, tok);
  EXPECT_GT(result.deflatedPartCount, 0u);
  EXPECT_GT(result.tokensSaved, 0u);

  // After deflation, those tool_result bodies must be stubs.
  bool sawStub = false;
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (const auto *tr = std::get_if<ToolResultContent>(&part)) {
          if (wm::isDeflatedStub(tr->result)) {
            sawStub = true;
            // Archive id must be present and resolvable.
            const std::string aid = wm::extractArchiveId(tr->result);
            EXPECT_FALSE(aid.empty());
            EXPECT_TRUE(archive.has(aid));
          }
        }
      }
    }
  }
  EXPECT_TRUE(sawStub);
  fs::remove_all(base);
}

// ---------- WorkingMemory assembleWorkingSet ----------

TEST(WorkingMemory, BelowBufferIsPassThrough) {
  auto ctx = makeContext();
  ctx.config.workingMemory.bufferOccupancyRatio = 0.99f; // never crossed

  AgentHistory history;
  history.threadId = "t";
  history.turns.push_back(userTurn("u-1", "task"));
  history.turns.push_back(assistantText("a-1", "ok"));

  HeuristicTokenizer tok;
  wm::WorkingMemoryInputs in;
  in.tokenizer = &tok;
  in.actorContextWindow = 128000;

  wm::WorkingMemoryReport report;
  auto out = wm::assembleWorkingSet(ctx, history, in, report);
  EXPECT_EQ(out.turns.size(), history.turns.size());
  EXPECT_EQ(report.evictedTurnCount, 0u);
  EXPECT_FALSE(report.aboveBufferThreshold);
}

TEST(WorkingMemory, UserPromptsRetainedAtEverySize) {
  auto ctx = makeContext();
  AgentHistory history;
  history.threadId = "t";
  // Many user prompts scattered through the history.
  for (int i = 0; i < 30; ++i) {
    if (i % 5 == 0) {
      history.turns.push_back(userTurn("u-" + std::to_string(i),
                                       "user prompt " + std::to_string(i)));
    } else {
      history.turns.push_back(
          assistantText("a-" + std::to_string(i),
                        std::string(400, 'x')));
    }
  }

  HeuristicTokenizer tok;
  wm::WorkingMemoryInputs in;
  in.tokenizer = &tok;
  in.actorContextWindow = 8000;

  wm::WorkingMemoryReport report;
  auto out = wm::assembleWorkingSet(ctx, history, in, report);

  // Count user prompts in the assembled history.
  int userInOut = 0;
  for (const auto &t : out.turns) {
    for (const auto &m : t.messages) {
      if (m.role == Role::User) {
        userInOut += 1;
        break;
      }
    }
  }
  EXPECT_EQ(userInOut, 6) << "All 6 user prompts must survive";
  EXPECT_EQ(report.userPromptsRetained, report.userPromptsTotal);
}

TEST(WorkingMemory, ImagePartsRetainedAtEverySize) {
  auto ctx = makeContext();
  AgentHistory history;
  history.threadId = "t";
  history.turns.push_back(userTurn("u-1", "context"));
  history.turns.push_back(imageTurn("img-1"));
  for (int i = 0; i < 30; ++i) {
    history.turns.push_back(
        assistantText("a-" + std::to_string(i), std::string(400, 'x')));
  }

  HeuristicTokenizer tok;
  wm::WorkingMemoryInputs in;
  in.tokenizer = &tok;
  in.actorContextWindow = 8000;

  wm::WorkingMemoryReport report;
  auto out = wm::assembleWorkingSet(ctx, history, in, report);

  int images = 0;
  for (const auto &t : out.turns) {
    for (const auto &m : t.messages) {
      for (const auto &p : m.content) {
        if (std::holds_alternative<ImageContent>(p)) {
          images += 1;
        }
      }
    }
  }
  EXPECT_EQ(images, 1);
  EXPECT_EQ(report.imagePartsRetained, report.imagePartsTotal);
}

TEST(WorkingMemory, RelevanceFillRecallsEvictedTurns) {
  auto ctx = makeContext();
  AgentHistory history;
  history.threadId = "t";
  // Lots of small content; one turn far back contains a unique keyword.
  history.turns.push_back(userTurn("u-1", "let's begin"));
  history.turns.push_back(assistantText("a-old", "the magic_word_xyz lives here in turn a-old"));
  for (int i = 0; i < 40; ++i) {
    history.turns.push_back(
        assistantText("a-" + std::to_string(i), std::string(200, 'q')));
  }
  // Recent user prompt asks about the magic word.
  history.turns.push_back(userTurn("u-recent", "what about the magic_word_xyz"));

  HeuristicTokenizer tok;
  wm::WorkingMemoryInputs in;
  in.tokenizer = &tok;
  in.actorContextWindow = 4000;

  // Hand-rolled relevance source: returns the turn containing "magic_word_xyz".
  in.relevanceQuery = [&](const std::string &query, std::size_t k) {
    std::vector<std::string> ids;
    if (query.find("magic_word_xyz") != std::string::npos && k > 0) {
      ids.push_back("a-old");
    }
    return ids;
  };

  wm::WorkingMemoryReport report;
  auto out = wm::assembleWorkingSet(ctx, history, in, report);

  // a-old must have been recalled.
  bool present = false;
  for (const auto &t : out.turns) {
    if (t.turnId == "a-old") {
      present = true;
      break;
    }
  }
  EXPECT_TRUE(present) << "Relevance fill should pull a-old back into the working set";
  EXPECT_GE(report.recalledTurnCount, 1u);
}

TEST(WorkingMemory, AgentPinnedTurnIdsHardPinned) {
  auto ctx = makeContext();
  ctx.state.pinnedTurnIds = {"a-keep"};

  AgentHistory history;
  history.threadId = "t";
  history.turns.push_back(userTurn("u-1", "task"));
  history.turns.push_back(assistantText("a-keep", "the agent declared this important"));
  for (int i = 0; i < 40; ++i) {
    history.turns.push_back(
        assistantText("a-" + std::to_string(i), std::string(400, 'x')));
  }

  HeuristicTokenizer tok;
  wm::WorkingMemoryInputs in;
  in.tokenizer = &tok;
  in.actorContextWindow = 4000;

  wm::WorkingMemoryReport report;
  auto out = wm::assembleWorkingSet(ctx, history, in, report);

  bool present = false;
  for (const auto &t : out.turns) {
    if (t.turnId == "a-keep") {
      present = true;
      break;
    }
  }
  EXPECT_TRUE(present);
}

TEST(WorkingMemoryWorker, DeterministicEmbedderAndQueryRoundTrip) {
  auto embed = wm::deterministicEmbedFn(64);
  ASSERT_TRUE(embed != nullptr);

  const std::string base = (fs::temp_directory_path() /
                            ("firmius_wm_worker_" +
                             std::to_string(std::random_device{}())))
                               .string();
  fs::create_directories(base);

  wm::ThreadWorkingMemoryWorker worker("t-1", base, embed);
  worker.enqueueEmbedding("u-1", "the original task is to fix the parser bug");
  worker.enqueueEmbedding("u-2", "completely unrelated topic about lasagna recipes");
  worker.enqueueEmbedding("u-3", "more parser context, parser bug investigation continues");
  ASSERT_TRUE(worker.drainEmbedding(std::chrono::milliseconds(1000)));
  EXPECT_EQ(worker.embeddedTurnCount(), 3u);

  auto top = worker.queryRelevant("parser bug fix", 2);
  ASSERT_FALSE(top.empty());
  // The two parser-related turns should rank higher than the lasagna one.
  bool sawParser = false;
  for (const auto &id : top) {
    if (id == "u-1" || id == "u-3") sawParser = true;
  }
  EXPECT_TRUE(sawParser);
  worker.shutdown();
  fs::remove_all(base);
}
