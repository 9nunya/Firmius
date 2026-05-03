#include "utils/ModeCycle.hpp"

#include "agents/modes/Mode.hpp"

#include <algorithm>
#include <set>

namespace firmius::tui {

std::vector<std::string>
buildModeCycleList(const std::string &personaName) {
  // The cycle always carries an explicit "no mode" entry so the operator
  // can drop back to the base persona stance without typing /mode "".
  std::vector<std::string> cycle;
  cycle.push_back("");

  auto &registry = firmius::core::modes::ModeRegistry::instance();
  std::set<std::string> seen;

  // Persona-scoped sub-modes lead — they're the operator's primary
  // operating set when a persona is selected.
  if (!personaName.empty()) {
    for (const auto &name : registry.listForPersona(personaName)) {
      if (seen.insert(name).second) {
        cycle.push_back(name);
      }
    }
  }

  // System-level modes follow. listNames() returns every registered mode
  // (system + persona-scoped). Filter to system-only by skipping any
  // qualified name that contains a colon — those are sub-modes already
  // covered above (or they belong to a different persona, which is not
  // cycleable from here).
  for (const auto &name : registry.listNames()) {
    if (name.find(':') != std::string::npos) {
      continue;
    }
    if (seen.insert(name).second) {
      cycle.push_back(name);
    }
  }

  return cycle;
}

std::string cycleMode(const std::string &currentMode,
                      const std::string &personaName, int direction) {
  const auto cycle = buildModeCycleList(personaName);
  if (cycle.empty()) {
    return "";
  }
  auto it = std::find(cycle.begin(), cycle.end(), currentMode);
  if (it == cycle.end()) {
    return cycle.front();
  }
  const int idx = static_cast<int>(it - cycle.begin());
  const int n = static_cast<int>(cycle.size());
  // C++ negative-modulo defence: normalise into [0, n).
  int next = ((idx + direction) % n + n) % n;
  return cycle[static_cast<std::size_t>(next)];
}

} // namespace firmius::tui
