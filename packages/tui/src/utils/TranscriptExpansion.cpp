#include "utils/TranscriptExpansion.hpp"

#include "persistence/ThreadManager.hpp"

#include <algorithm>
#include <cstddef>

namespace firmius::tui::detail {

std::optional<std::string>
compactionIdFromTurnIdForDisplay(const std::string &turnId) {
  constexpr const char *prefixes[] = {"compaction-start-",
                                      "compaction-summary-",
                                      "compaction-end-"};
  for (const char *prefix : prefixes) {
    const std::size_t len = std::char_traits<char>::length(prefix);
    if (turnId.rfind(prefix, 0) == 0) {
      return turnId.substr(len);
    }
  }
  return std::nullopt;
}

namespace {

std::size_t overlappingSnapshotSuffixLengthForDisplayImpl(
    const std::vector<shared::AgentTurn> &snapshotTurns,
    const std::vector<shared::AgentTurn> &currentTurns,
    std::size_t currentStart) {
  if (currentTurns.size() <= currentStart) {
    return 0;
  }
  const std::size_t maxCount =
      std::min(snapshotTurns.size(), currentTurns.size() - currentStart);
  for (std::size_t count = maxCount; count > 0; --count) {
    bool allMatch = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (snapshotTurns[snapshotTurns.size() - count + i].turnId !=
          currentTurns[currentStart + i].turnId) {
        allMatch = false;
        break;
      }
    }
    if (allMatch) {
      return count;
    }
  }
  return 0;
}

std::size_t overlappingRenderedPrefixLengthForDisplayImpl(
    const std::vector<shared::AgentTurn> &renderedTurns,
    const std::vector<shared::AgentTurn> &snapshotTurns) {
  const std::size_t maxCount =
      std::min(renderedTurns.size(), snapshotTurns.size());
  for (std::size_t count = maxCount; count > 0; --count) {
    bool allMatch = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (renderedTurns[renderedTurns.size() - count + i].turnId !=
          snapshotTurns[i].turnId) {
        allMatch = false;
        break;
      }
    }
    if (allMatch) {
      return count;
    }
  }
  return 0;
}

} // namespace

std::vector<shared::AgentTurn> expandCompactionTranscriptTurnsForDisplay(
    const std::vector<shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots,
    std::unordered_set<std::string> &expanded_ids) {
  std::vector<shared::AgentTurn> result;
  for (std::size_t i = 0; i < turns.size(); ++i) {
    const auto compactionId =
        compactionIdFromTurnIdForDisplay(turns[i].turnId);
    if (!compactionId.has_value() ||
        turns[i].turnId.rfind("compaction-start-", 0) != 0) {
      result.push_back(turns[i]);
      continue;
    }

    std::size_t blockEnd = i;
    while (blockEnd + 1 < turns.size()) {
      const auto nextId =
          compactionIdFromTurnIdForDisplay(turns[blockEnd + 1].turnId);
      if (!nextId.has_value() || *nextId != *compactionId) {
        break;
      }
      ++blockEnd;
      if (turns[blockEnd].turnId.rfind("compaction-end-", 0) == 0) {
        break;
      }
    }

    auto snapshotIt = snapshots.find(*compactionId);
    if (snapshotIt != snapshots.end() && !expanded_ids.count(*compactionId)) {
      expanded_ids.insert(*compactionId);
      const auto &snapshotTurns = snapshotIt->second.turns;
      auto expandedSnapshot = expandCompactionTranscriptTurnsForDisplay(
          snapshotTurns, snapshots, expanded_ids);
      const std::size_t renderedOverlap =
          overlappingRenderedPrefixLengthForDisplayImpl(result, expandedSnapshot);
      if (renderedOverlap > 0 && renderedOverlap <= expandedSnapshot.size()) {
        expandedSnapshot.erase(expandedSnapshot.begin(),
                               expandedSnapshot.begin() + renderedOverlap);
      }
      result.insert(result.end(), expandedSnapshot.begin(),
                    expandedSnapshot.end());

      for (std::size_t j = i; j <= blockEnd; ++j) {
        result.push_back(turns[j]);
      }

      const std::size_t overlap = overlappingSnapshotSuffixLengthForDisplayImpl(
          snapshotTurns, turns, blockEnd + 1);
      const std::size_t nextIndex = blockEnd + overlap + 1;
      if (nextIndex >= turns.size()) {
        break;
      }
      i = nextIndex - 1;
      continue;
    }

    for (std::size_t j = i; j <= blockEnd; ++j) {
      result.push_back(turns[j]);
    }
    i = blockEnd;
  }
  return result;
}

} // namespace firmius::tui::detail

namespace firmius::tui {

std::vector<shared::AgentTurn> expandCompactionTranscriptForDisplay(
    const std::vector<shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots) {
  std::unordered_set<std::string> expanded_ids;
  return detail::expandCompactionTranscriptTurnsForDisplay(turns, snapshots,
                                                           expanded_ids);
}

} // namespace firmius::tui
