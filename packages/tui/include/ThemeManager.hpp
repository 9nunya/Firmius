#ifndef FIRMIUS_TUI_THEME_MANAGER_HPP
#define FIRMIUS_TUI_THEME_MANAGER_HPP

#include "Theme.hpp"
#include <string>
#include <vector>

namespace firmius::tui {

class ThemeManager {
public:
  static ThemeManager &instance();

  /**
   * @brief Scans ~/.firmius/themes for *.theme.json files.
   *        If none found, creates default themes.
   */
  void loadThemes();

  /**
   * @brief Cycles to the next available theme.
   */
  void cycleTheme();
  void setTheme(const std::string &name);

  /**
   * @brief Returns the currently active theme.
   */
  const Theme &getCurrentTheme() const;

  /**
   * @brief Returns names of all loaded themes.
   */
  std::vector<std::string> getThemeNames() const;

private:
  ThemeManager();
  ~ThemeManager() = default;

  void loadPersistedSelection();
  void persistSelection() const;
  void
  loadSystemThemes(); // From /usr/local/share/firmius/themes or project/themes
  void loadUserThemes(); // From ~/.firmius/themes

  Theme loadThemeFromFile(const std::string &path);

  std::vector<Theme> themes_;
  size_t current_theme_index_ = 0;
};

} // namespace firmius::tui

#endif
