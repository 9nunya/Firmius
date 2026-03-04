#pragma once

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

class HistoryEditor {
public:
    static UndoResult undoTurns(std::vector<AgentTurn>& turns, int count);
    static UndoResult undoMessages(std::vector<AgentTurn>& turns, int count);
    static UndoResult undoAfterTimestamp(std::vector<AgentTurn>& turns, uint64_t timestamp);
};

} // namespace firmius::core
