#include "persistence/HistoryEditor.hpp"
#include <algorithm>

namespace firmius::core {

UndoResult HistoryEditor::undoTurns(std::vector<AgentTurn>& turns, int count) {
    if (count <= 0 || turns.size() <= 2) {
        return {0, false, 0, false};
    }

    UndoResult result;
    int maxRemovable = static_cast<int>(turns.size()) - 2;
    int toRemove = std::min(count, maxRemovable);

    for (int i = 0; i < toRemove; ++i) {
        const auto& turn = turns[turns.size() - 1 - i];
        if (turn.turnId.find("compaction-summary-") == 0) {
            result.compactionReversed = true;
        }
    }

    turns.erase(turns.end() - toRemove, turns.end());
    result.turnsRemoved = toRemove;
    result.restoredTurns = toRemove;
    result.willExceedContext = false;

    return result;
}

UndoResult HistoryEditor::undoMessages(std::vector<AgentTurn>& turns, int count) {
    if (count <= 0 || turns.size() <= 2) {
        return {0, false, 0, false};
    }

    UndoResult result;
    int messagesRemoved = 0;
    int turnsRemoved = 0;

    for (auto it = turns.rbegin(); it != turns.rend() && messagesRemoved < count;) {
        size_t originalIndex = turns.size() - 1 - (it - turns.rbegin());
        if (originalIndex <= 1) {
            break;
        }

        int messagesInTurn = static_cast<int>(it->messages.size());
        if (messagesInTurn <= 0) {
            ++it;
            continue;
        }

        int remaining = count - messagesRemoved;
        if (messagesInTurn <= remaining) {
            if (it->turnId.find("compaction-summary-") == 0) {
                result.compactionReversed = true;
            }
            it = decltype(it)(turns.erase(turns.begin() + originalIndex));
            ++turnsRemoved;
            messagesRemoved += messagesInTurn;
        } else {
            it->messages.erase(it->messages.end() - remaining, it->messages.end());
            messagesRemoved = count;
            ++it;
        }
    }

    result.turnsRemoved = turnsRemoved;
    result.restoredTurns = turnsRemoved;
    result.willExceedContext = false;

    return result;
}

UndoResult HistoryEditor::undoAfterTimestamp(std::vector<AgentTurn>& turns, uint64_t timestamp) {
    if (turns.size() <= 2) {
        return {0, false, 0, false};
    }

    UndoResult result;
    int turnsRemoved = 0;

    auto it = turns.begin() + 2;
    while (it != turns.end()) {
        if (!it->messages.empty() && it->messages[0].timestamp > timestamp) {
            if (it->turnId.find("compaction-summary-") == 0) {
                result.compactionReversed = true;
            }
            it = turns.erase(it);
            ++turnsRemoved;
        } else {
            ++it;
        }
    }

    result.turnsRemoved = turnsRemoved;
    result.restoredTurns = turnsRemoved;
    result.willExceedContext = false;

    return result;
}

}
