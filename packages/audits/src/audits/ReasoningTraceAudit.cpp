#include "audits/ReasoningTraceAudit.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

std::string ReasoningTraceAudit::getId() const {
  return "reasoning_trace_continuity";
}

std::string ReasoningTraceAudit::getDescription() const {
  return "Verifies reasoning traces (ThinkingChunks) appear on all turns for "
         "Gemini 3 / Antigravity.";
}

shared::AuditResult ReasoningTraceAudit::run(const std::vector<std::string> &) {
  shared::AuditResult result;
  result.auditId = getId();

  Panic::init();
  EnvLoader::load(".env.local");

  auto &harness = Harness::instance();
  harness.init();
  harness.debugLogging = false;

  HostCreationOptions opts;
  opts.type = HostType::Local;
  opts.deleteOnExit = true;

  std::string threadId = harness.newThread(opts, "/work", "aster");
  if (threadId.empty()) {
    result.exitCode = 1;
    result.passed = false;
    result.output = "Failed to create thread.";
    harness.shutdown();
    return result;
  }

  harness.switchModel("antigravity", "gemini-3-flash", "max");

  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;
  std::vector<int> thinkingChunksPerTurn;
  std::vector<int> textChunksPerTurn;

  const int subId = harness.subscribe([&](const AppEvent &ev) {
    std::visit(
        [&](auto &&e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, AgentTurnCompleted>) {
            std::lock_guard<std::mutex> lock(mtx);
            done = true;
            cv.notify_one();
          } else if constexpr (std::is_same_v<T, AgentThinking>) {
            std::lock_guard<std::mutex> lock(mtx);
            if (!thinkingChunksPerTurn.empty()) {
              thinkingChunksPerTurn.back()++;
            }
          } else if constexpr (std::is_same_v<T, AgentText>) {
            std::lock_guard<std::mutex> lock(mtx);
            if (!textChunksPerTurn.empty()) {
              textChunksPerTurn.back()++;
            }
          }
        },
        ev);
  });

  auto runTurn = [&](const std::string &prompt) {
    {
      std::lock_guard<std::mutex> lock(mtx);
      thinkingChunksPerTurn.push_back(0);
      textChunksPerTurn.push_back(0);
      done = false;
    }

    harness.send(prompt);

    std::unique_lock<std::mutex> lk(mtx);
    cv.wait(lk, [&] { return done; });
  };

  // 3 turns; the bug was: thinking appears on turn 1, disappears on 2+.
  runTurn("Compute 2+2 and explain your reasoning step by step.");
  runTurn("Compute 3+3 and explain your reasoning step by step.");
  runTurn("Compute 5+5 and explain your reasoning step by step.");

  harness.unsubscribe(subId);
  harness.shutdown();

  bool allTurnsHadThinking = true;
  std::ostringstream out;
  out << "Reasoning trace continuity results:\n";
  for (size_t i = 0; i < thinkingChunksPerTurn.size(); ++i) {
    out << "- Turn " << (i + 1) << ": " << thinkingChunksPerTurn[i]
        << " thinking chunks, " << textChunksPerTurn[i] << " text chunks\n";
    if (thinkingChunksPerTurn[i] == 0) {
      allTurnsHadThinking = false;
    }
  }

  result.output = out.str();
  result.passed = allTurnsHadThinking;
  result.exitCode = allTurnsHadThinking ? 0 : 1;
  return result;
}

} // namespace firmius::audits
