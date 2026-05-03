#ifndef FIRMIUS_TUI_MODE_CYCLE_HPP
#define FIRMIUS_TUI_MODE_CYCLE_HPP

#include <string>
#include <vector>

namespace firmius::tui {

/**
 * @brief Build the cycleable mode list for a given persona.
 *
 * The list always begins with the empty string (the "no mode" stance),
 * followed by:
 *   - persona-scoped sub-modes for `personaName` (e.g. "forge:apply") if any,
 *   - system-level modes (e.g. "diagnose"), de-duplicated and sorted.
 *
 * Cycling wraps. Used by:
 *   - the Ctrl+Y / Ctrl+Shift+Y keybinds in `MainView`,
 *   - the `/mode` slash command (no-arg form),
 *   - the welcome screen pre-thread mode picker.
 */
std::vector<std::string>
buildModeCycleList(const std::string &personaName);

/**
 * @brief Return the next mode after `currentMode` in the cycle list for
 * `personaName`. `direction == +1` advances forward; `-1` cycles back.
 * If `currentMode` isn't in the cycle list, returns the first element.
 */
std::string cycleMode(const std::string &currentMode,
                      const std::string &personaName, int direction);

} // namespace firmius::tui

#endif
