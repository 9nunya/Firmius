#ifndef FIRMIUS_CORE_HISTORYEDITOR_HPP
#define FIRMIUS_CORE_HISTORYEDITOR_HPP

#include "Context.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace firmius::core {

using namespace firmius::shared;

struct UndoResult {
    int turnsRemoved = 0;
    bool compactionReversed = false;
    int restoredTurns = 0;
    bool willExceedContext = false;
};

// V1 Historical Redo Contracts (from monolith 08fd932 era)
struct UndoActionResult {
    std::string undoActionId;
    std::string threadId;
    std::string agentId;
    int turnsRemoved = 0;
};

struct RedoActionResult {
    std::string redoActionId;
    std::string threadId;
    std::string agentId;
    int restoredTurns = 0;
};

class HistoryEditor {
public:
    static UndoResult undoTurns(std::vector<AgentTurn>& turns, int count);
    static UndoResult undoMessages(std::vector<AgentTurn>& turns, int count);
    static UndoResult undoAfterTimestamp(std::vector<AgentTurn>& turns, uint64_t timestamp);
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_HISTORYEDITOR_HPP
