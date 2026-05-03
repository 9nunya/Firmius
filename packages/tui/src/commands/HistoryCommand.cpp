#include "commands/HistoryCommand.hpp"

#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <ctime>

namespace {

std::string formatUtcMillis(std::uint64_t ms) {
  using namespace std::chrono;
  std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

} // namespace

namespace firmius::tui {

void HistoryCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)args;

  if (!ctx.state) {
    std::cout << "No TUI state available\n";
    return;
  }

  auto &harness = firmius::core::Harness::instance();
  const std::string threadId = harness.currentThreadId();

  std::cout << "# Transcript Undo/Redo\n\n";
  std::cout << "Thread: " << (threadId.empty() ? "(none)" : threadId) << "\n\n";

  // Session pointers (quick UX path)
  if (ctx.state->last_transcript_undo_action_) {
    const auto &a = *ctx.state->last_transcript_undo_action_;
    std::cout << "## Session (last action)\n";
    std::cout << "Undo action id: " << a.undoActionId << "\n";
    std::cout << "Scope: " << a.scopeType;
    if (!a.scopeArgJson.empty()) std::cout << " " << a.scopeArgJson;
    std::cout << "\n";
    std::cout << "Redo available: " << (a.redoAvailable ? "yes" : "no") << "\n";
    std::cout << "Reason: " << (a.reason.empty() ? "(none)" : a.reason) << "\n";
    if (a.createdAt > 0) {
      std::cout << "Created: " << formatUtcMillis(a.createdAt) << "\n";
    }
  } else {
    std::cout << "## Session (last action)\n";
    std::cout << "Undo action id: (none)\n";
  }

  if (ctx.state->last_transcript_redo_action_) {
    const auto &r = *ctx.state->last_transcript_redo_action_;
    std::cout << "\nRedo action id: " << r.redoActionId << "\n";
    std::cout << "Replayed undo: " << r.undoActionId << "\n";
    if (r.createdAt > 0) {
      std::cout << "Created: " << formatUtcMillis(r.createdAt) << "\n";
    }
    if (!r.resultJson.empty()) {
      std::cout << "Result: " << r.resultJson << "\n";
    }
  } else {
    std::cout << "\nRedo action id: (none)\n";
  }

  // Persisted timeline (richer than just last-id)
  if (!threadId.empty()) {
    firmius::core::ThreadManager tm(firmius::core::ThreadManager::defaultBasePath());
    const int limit = 20;
    auto recent = tm.listTranscriptUndoActions(threadId, limit);
    if (!recent.empty()) {
      std::cout << "\n## Recent transcript undo actions\n";
      for (const auto &a : recent) {
        std::cout << "- " << a.undoActionId;
        std::cout << "  agent=" << (a.agentId.empty() ? "(unknown)" : a.agentId);
        std::cout << "  scope=" << a.scopeType;
        if (!a.scopeArgJson.empty()) std::cout << " " << a.scopeArgJson;
        std::cout << "  redo=" << (a.redoAvailable ? "yes" : "no");
        if (a.createdAt > 0) std::cout << "  at=" << formatUtcMillis(a.createdAt);
        if (!a.reason.empty()) std::cout << "\n    reason=" << a.reason;
        std::cout << "\n";
      }
    }
  }

  std::cout << "\nTips:\n";
  std::cout << "- /undo_turn  (undo last assistant turn)\n";
  std::cout << "- /undo [n]   (undo N turns; omit n to rewind to last user boundary)\n";
  std::cout << "- /redo       (redo last transcript undo)\n";
  std::cout << "- /undo_transcript [n] (persisted undo capture, prints undo_action_id)\n";
  std::cout << "- /redo_transcript <undo_action_id> (replay persisted undo by id)\n";
}

} // namespace firmius::tui
