#ifndef FIRMIUS_TUI_USER_PREFERENCES_HPP
#define FIRMIUS_TUI_USER_PREFERENCES_HPP

#include "Context.hpp"

#include <optional>
#include <string>

namespace firmius::tui {

struct UserPreferences {
  std::optional<std::string> theme_name;
  std::optional<shared::ThreadPermissionMode> preferred_permission_mode;
  std::optional<bool> prefer_todo_panel_on_narrow;
};

UserPreferences loadUserPreferences();
void saveUserPreferences(const UserPreferences &preferences);

} // namespace firmius::tui

#endif
