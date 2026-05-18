#include "audits/WorkingMemoryAudit.hpp"

#include "Context.hpp"
#include "HeuristicTokenizer.hpp"
#include "agents/working_memory/DeflationArchive.hpp"
#include "agents/working_memory/Deflator.hpp"
#include "agents/working_memory/PinPolicy.hpp"
#include "agents/working_memory/WorkingMemory.hpp"
#include "agents/working_memory/WorkingMemoryWorker.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>

namespace firmius::audits {

namespace fs = std::filesystem;
namespace wm = firmius::core::working_memory;

namespace {

using firmius::shared::AgentContext;
using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::HeuristicTokenizer;
using firmius::shared::ImageContent;
using firmius::shared::Message;
using firmius::shared::Role;
using firmius::shared::TextContent;
using firmius::shared::ToolCallContent;
using firmius::shared::ToolResultContent;

const char *const kFilePool[] = {
    "/work/parser/Parser.cpp",
    "/work/parser/Lexer.cpp",
    "/work/parser/Tokens.hpp",
    "/work/runtime/Engine.cpp",
    "/work/runtime/State.cpp",
    "/work/util/Hash.cpp",
};

std::string syntheticGrepResult(const std::string &needle, std::size_t bytes) {
  std::string out;
  out.reserve(bytes);
  std::mt19937 rng(static_cast<std::uint32_t>(std::hash<std::string>{}(needle)));
  std::uniform_int_distribution<int> lineLen(40, 120);
  while (out.size() < bytes) {
    out += "match: " + needle + " ";
    int n = lineLen(rng);
    for (int i = 0; i < n; ++i) {
      out.push_back(static_cast<char>('a' + (rng() % 26)));
    }
    out += '\n';
  }
  return out;
}

std::string syntheticReadResult(const std::string &path, std::size_t bytes) {
  std::string out;
  out.reserve(bytes);
  std::mt19937 rng(static_cast<std::uint32_t>(std::hash<std::string>{}(path)));
  while (out.size() < bytes) {
    out += "// " + path + "\n";
    int n = 60 + static_cast<int>(rng() % 40);
    for (int i = 0; i < n; ++i) {
      out.push_back(static_cast<char>('a' + (rng() % 26)));
    }
    out += '\n';
  }
  return out;
}

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

AgentTurn imageTurn(const std::string &id) {
  AgentTurn t;
  t.turnId = id;
  Message m;
  m.role = Role::User;
  m.timestamp = 1;
  m.content.push_back(TextContent{"the schema diagram"});
  m.content.push_back(ImageContent{"data:image/png;base64,abcdef", "image/png", "auto"});
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

AgentTurn assistantCall(const std::string &id, const std::string &callId,
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
                     const std::string &result) {
  AgentTurn t;
  t.turnId = id;
  Message m;
  m.role = Role::ToolResult;
  m.timestamp = 1;
  m.content.push_back(ToolResultContent{callId, result, true, "", ""});
  t.messages.push_back(std::move(m));
  return t;
}

struct Workload {
  AgentHistory history;
  std::uint32_t totalTurns = 0;
  std::uint32_t userPrompts = 0;
  std::uint32_t imageParts = 0;
  std::uint32_t toolPairs = 0;
};

Workload buildSyntheticWorkload(int turnTarget) {
  Workload w;
  w.history.threadId = "audit-thread";
  std::mt19937 rng(0xfeedface);
  int counter = 0;

  auto append = [&](AgentTurn turn) {
    w.history.turns.push_back(std::move(turn));
    w.totalTurns += 1;
  };

  // Bootstrap + first user prompt set the original task.
  append(assistantText("bootstrap-system", "You are the agent."));
  append(userTurn("u-1",
                  "Investigate the parser failure on triple-quoted strings "
                  "and ship a fix. Files of interest live under /work/parser."));
  w.userPrompts += 1;
  append(imageTurn("u-2-img"));
  w.userPrompts += 1;
  w.imageParts += 1;

  while (w.totalTurns < static_cast<std::uint32_t>(turnTarget)) {
    counter += 1;
    const std::string idCall = "a-" + std::to_string(counter);
    const std::string idResult = "tr-" + std::to_string(counter);
    const std::string callId = "c-" + std::to_string(counter);
    const std::string toolName = (rng() % 3 == 0) ? "Grep" : "Read";
    const std::string filePath = kFilePool[counter % 6];
    std::string args;
    std::string body;
    if (toolName == "Grep") {
      args = std::string("{\"pattern\":\"triple\",\"path\":\"") + filePath +
             "\"}";
      body = syntheticGrepResult("triple", 800 + (rng() % 1200));
    } else {
      args = std::string("{\"path\":\"") + filePath + "\"}";
      body = syntheticReadResult(filePath, 600 + (rng() % 1400));
    }
    append(assistantCall(idCall, callId, toolName, args));
    append(toolResult(idResult, callId, body));
    w.toolPairs += 1;

    // Sprinkle in a follow-up user prompt every ~30 turns to mimic real chats.
    if (counter % 15 == 0) {
      const std::string uid = "u-" + std::to_string(100 + counter);
      append(userTurn(uid, "Anything new on the parser? Try a different angle."));
      w.userPrompts += 1;
    }
    if (w.totalTurns >= static_cast<std::uint32_t>(turnTarget)) break;
    // Assistant text turn for narrative variety.
    append(assistantText("at-" + std::to_string(counter),
                         "Reviewing the result and planning the next step."));
  }
  return w;
}

struct Scenario {
  std::string label;
  std::uint32_t actorContextWindow;
};

std::string formatRow(const std::string &label, std::uint32_t window,
                      const wm::WorkingMemoryReport &r,
                      std::uint32_t userPromptsTotal,
                      std::uint32_t imagePartsTotal) {
  std::ostringstream out;
  out << std::left << std::setw(14) << label
      << " window=" << std::setw(7) << window
      << " raw=" << std::setw(8) << r.rawHistoryTokens
      << " set=" << std::setw(8) << r.workingSetTokens;

  const double ratio =
      r.rawHistoryTokens > 0
          ? static_cast<double>(r.workingSetTokens) / r.rawHistoryTokens
          : 1.0;
  out << " ratio=" << std::fixed << std::setprecision(3) << ratio
      << " hardPin=" << std::setw(4) << r.pinnedTurnCount
      << " evict=" << std::setw(4) << r.evictedTurnCount
      << " recall=" << std::setw(3) << r.recalledTurnCount
      << " defl=" << std::setw(3) << r.deflatedPartCount
      << " saveDefl=" << std::setw(7) << r.tokensSavedByDeflation
      << " saveEvict=" << std::setw(7) << r.tokensSavedByEviction
      << " spendSum=" << std::setw(6) << r.tokensSpentOnSummaries
      << " spendEmb=" << std::setw(5) << r.tokensSpentOnEmbeddings;

  out << " userKeep=" << r.userPromptsRetained << '/' << userPromptsTotal;
  out << " imgKeep=" << r.imagePartsRetained << '/' << imagePartsTotal;
  out << " latUs=" << r.hotPathLatencyMicros;
  if (r.aboveEmergencyThreshold) {
    out << "  [EMERG]";
  } else if (r.aboveTargetThreshold) {
    out << "  [TGT]";
  } else if (r.aboveBufferThreshold) {
    out << "  [BUF]";
  } else {
    out << "  [PASS]";
  }
  return out.str();
}

} // namespace

std::string WorkingMemoryAudit::getId() const { return "working_memory"; }

std::string WorkingMemoryAudit::getDescription() const {
  return "Synthetic 200-turn workload exercising rolling-memory v2: "
         "verifies user prompt + image retention, measures token "
         "savings/spend, deflation + recall behavior, and hot-path latency "
         "across a sweep of context-window sizes.";
}

shared::AuditResult WorkingMemoryAudit::run(
    const std::vector<std::string> &args) {
  shared::AuditResult result;
  result.auditId = getId();
  result.passed = true;
  std::ostringstream log;

  int turnTarget = 200;
  if (!args.empty()) {
    try {
      turnTarget = std::max(20, std::stoi(args[0]));
    } catch (...) {
      // ignore
    }
  }

  log << "WorkingMemoryAudit: synthesizing " << turnTarget
      << "-turn workload\n";

  const Workload wl = buildSyntheticWorkload(turnTarget);
  log << "  total turns:   " << wl.totalTurns << "\n";
  log << "  user prompts:  " << wl.userPrompts << "\n";
  log << "  image parts:   " << wl.imageParts << "\n";
  log << "  tool pairs:    " << wl.toolPairs << "\n\n";

  HeuristicTokenizer tok;
  std::uint32_t totalRawTokens = 0;
  for (const auto &turn : wl.history.turns) {
    for (const auto &m : turn.messages) {
      for (const auto &p : m.content) {
        if (const auto *t = std::get_if<TextContent>(&p)) {
          totalRawTokens += tok.count(t->text);
        } else if (const auto *tc = std::get_if<ToolCallContent>(&p)) {
          totalRawTokens += tok.count(tc->args);
          totalRawTokens += tok.count(tc->name);
        } else if (const auto *tr = std::get_if<ToolResultContent>(&p)) {
          totalRawTokens += tok.count(tr->result);
        }
      }
    }
  }
  log << "  raw tokens:    " << totalRawTokens << "\n\n";

  // Build a working-memory worker so embedding retrieval is exercised. Use
  // a temp directory for the archive + embedding cache.
  const fs::path base = fs::temp_directory_path() /
                        ("firmius_wm_audit_" +
                         std::to_string(std::random_device{}()));
  fs::create_directories(base);
  auto worker = std::make_shared<wm::ThreadWorkingMemoryWorker>(
      "audit-thread", base.string(), wm::deterministicEmbedFn(64));

  // Pre-embed every turn so query-time retrieval can fire.
  for (const auto &turn : wl.history.turns) {
    std::string queryable;
    for (const auto &m : turn.messages) {
      for (const auto &p : m.content) {
        if (const auto *t = std::get_if<TextContent>(&p)) {
          queryable += t->text;
          queryable += ' ';
        } else if (const auto *tc = std::get_if<ToolCallContent>(&p)) {
          queryable += tc->name;
          queryable += ' ';
          queryable += tc->args;
          queryable += ' ';
        } else if (const auto *tr = std::get_if<ToolResultContent>(&p)) {
          queryable.append(tr->result, 0,
                           std::min<std::size_t>(512, tr->result.size()));
          queryable += ' ';
        }
      }
    }
    worker->enqueueEmbedding(turn.turnId, std::move(queryable));
  }
  worker->drainEmbedding(std::chrono::seconds(5));
  log << "  embedded turns: " << worker->embeddedTurnCount() << "\n\n";

  // Sweep across a few actor context windows. We deliberately use small
  // windows so the same workload exercises the full ladder (pass-through,
  // buffer, target, emergency).
  const std::vector<Scenario> scenarios = {
      {"huge", 1'000'000},
      {"large", 200'000},
      {"medium", 64'000},
      {"small", 16'000},
      {"tiny", 4'000},
  };

  log << std::left << std::setw(14) << "scenario"
      << " window         raw         set         ratio   hardPin  "
         "evict  recall  defl  saveDefl  saveEvict  spendSum  spendEmb  "
         "userKeep  imgKeep  latUs  flag\n";

  std::uint32_t worstUserShortfall = 0;
  std::uint32_t worstImageShortfall = 0;
  std::uint32_t maxLatencyMicros = 0;

  for (const auto &sc : scenarios) {
    AgentContext ctx;
    ctx.identity.id = "audit-agent";
    ctx.history = std::make_shared<AgentHistory>();
    ctx.history->threadId = "audit-thread";
    ctx.config.workingMemory.enabled = true;
    ctx.config.workingMemory.bufferOccupancyRatio = 0.47f;
    ctx.config.workingMemory.targetOccupancyRatio = 0.57f;
    ctx.config.workingMemory.emergencyOccupancyRatio = 0.66f;
    ctx.config.workingMemory.recencyTailRatio = 0.18f;
    ctx.config.workingMemory.minimumRecencyTailTokens = 1024;
    ctx.config.workingMemory.deflationMinPartTokens = 100;
    ctx.config.workingMemory.defaultDeflationTurnHorizon = 3;
    ctx.config.workingMemory.embeddingsEnabled = true;
    ctx.config.workingMemory.embeddingTopK = 5;

    wm::WorkingMemoryInputs in;
    in.tokenizer = &tok;
    in.actorContextWindow = sc.actorContextWindow;
    in.relevanceQuery = [worker](const std::string &q, std::size_t k) {
      return worker->queryRelevant(q, k);
    };
    in.archive = &worker->archive();

    // Re-clone a history each iteration since deflation mutates in place.
    AgentHistory clone = wl.history;

    wm::WorkingMemoryReport report;
    auto out = wm::assembleWorkingSet(ctx, clone, in, report);
    (void)out;

    log << formatRow(sc.label, sc.actorContextWindow, report, wl.userPrompts,
                     wl.imageParts)
        << "\n";

    if (report.userPromptsRetained < wl.userPrompts) {
      const auto shortfall = wl.userPrompts - report.userPromptsRetained;
      if (shortfall > worstUserShortfall) worstUserShortfall = shortfall;
    }
    if (report.imagePartsRetained < wl.imageParts) {
      const auto shortfall = wl.imageParts - report.imagePartsRetained;
      if (shortfall > worstImageShortfall) worstImageShortfall = shortfall;
    }
    if (report.hotPathLatencyMicros > maxLatencyMicros) {
      maxLatencyMicros = static_cast<std::uint32_t>(report.hotPathLatencyMicros);
    }
  }

  log << "\n";
  log << "INVARIANT CHECKS\n";
  log << "  user prompts always retained: "
      << (worstUserShortfall == 0 ? "PASS" : "FAIL")
      << " (worst shortfall: " << worstUserShortfall << ")\n";
  log << "  image parts always retained:  "
      << (worstImageShortfall == 0 ? "PASS" : "FAIL")
      << " (worst shortfall: " << worstImageShortfall << ")\n";
  log << "  max hot-path latency:         " << maxLatencyMicros
      << " us (target: <50000)\n";

  if (worstUserShortfall > 0 || worstImageShortfall > 0) {
    result.passed = false;
    result.exitCode = 1;
  }

  worker->shutdown();
  std::error_code ec;
  fs::remove_all(base, ec);

  result.output = log.str();
  return result;
}

} // namespace firmius::audits
