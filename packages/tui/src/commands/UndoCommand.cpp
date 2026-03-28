#include "commands/UndoCommand.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"

namespace {

bool isUserTurn(const firmius::shared::AgentTurn& turn) {
  for (const auto& msg : turn.messages) {
    if (msg.role == firmius::shared::Role::User) {
      return true;
    }
  }
  return false;
}

bool isCompactionTurn(const firmius::shared::AgentTurn& turn) {
  return turn.turnId.rfind("compaction-start-", 0) == 0 ||
         turn.turnId.rfind("compaction-summary-", 0) == 0 ||
         turn.turnId.rfind("compaction-end-", 0) == 0;
}

int defaultUndoTurnCount(firmius::core::Harness& harness) {
  const std::string agentId = harness.focusedAgentId();
  if (agentId.empty()) {
    return 1;
  }

  std::shared_ptr<firmius::shared::AgentHistory> history =
      harness.getAgentHistoryPtr(agentId);
  if (!history) {
    const std::string threadId = harness.currentThreadId();
    if (threadId.empty()) {
      return 1;
    }
    auto loaded = firmius::core::ThreadManager(
                      firmius::core::ThreadManager::defaultBasePath())
                      .loadAgentHistory(threadId, agentId);
    history = std::make_shared<firmius::shared::AgentHistory>(std::move(loaded));
  }

  if (!history || history->turns.size() <= 2) {
    return 1;
  }

  for (std::size_t i = history->turns.size(); i-- > 2;) {
    if (isUserTurn(history->turns[i])) {
      const int count = static_cast<int>(history->turns.size() - i);
      return std::max(1, count);
    }
  }

  int compactionTail = 0;
  for (std::size_t i = history->turns.size(); i-- > 2;) {
    if (!isCompactionTurn(history->turns[i])) {
      break;
    }
    ++compactionTail;
  }
  return std::max(1, compactionTail);
}

} // namespace

namespace firmius::tui {

void UndoCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)ctx;
  auto& harness = firmius::core::Harness::instance();
  int count = args.empty() ? defaultUndoTurnCount(harness) : 1;
  if (!args.empty()) {
    try {
      count = args.front().asInt();
    } catch (...) {
      count = 1;
    }
  }
  if (count < 1)
    count = 1;
  harness.undoTurns(count);
}

} // namespace firmius::tui
