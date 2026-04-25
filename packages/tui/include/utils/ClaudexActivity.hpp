#ifndef FIRMIUS_TUI_UTILS_CLAUDEX_ACTIVITY_HPP
#define FIRMIUS_TUI_UTILS_CLAUDEX_ACTIVITY_HPP

#include "Context.hpp"
#include "StreamStateManager.hpp"

#include <string>

namespace firmius::tui {

// Compute the right-hand activity label for Claudex's persistent live row.
//
// This must be resilient even when some surfaces aren't available yet.
// The returned value is also used as the mode key for Claudex phrase banks.
std::string inferClaudexActivity(const firmius::shared::AgentContext &ctx,
                                 const firmius::tui::StreamState *stream,
                                 const std::string &fallback,
                                 const firmius::shared::AgentTodoList *todo = nullptr,
                                 const std::string &status_text = "");

} // namespace firmius::tui

#endif
