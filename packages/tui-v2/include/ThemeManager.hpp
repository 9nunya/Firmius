#pragma once

#include <optional>
#include <string>
#include <vector>

namespace firmius::tui2 {

struct ThemeRgb {
  int r = 0;
  int g = 0;
  int b = 0;
};

struct ThemeColorGroup {
  ThemeRgb bg;
  ThemeRgb fg;
};

struct ThemeStateColors {
  ThemeColorGroup normal;
  ThemeColorGroup focused;
  ThemeColorGroup busy;
  ThemeColorGroup error;
  std::vector<ThemeRgb> glint;
};

struct ThemeSpec {
  std::string name = "Default";

  struct Base {
    ThemeRgb bg{14, 16, 24};
    ThemeRgb fg{222, 228, 236};
    ThemeRgb border{52, 56, 68};
    ThemeRgb separator{70, 74, 88};
    ThemeRgb highlight{117, 164, 255};
    ThemeRgb dim{120, 124, 138};
  } base;

  struct StatusBar {
    ThemeStateColors idle;
    ThemeStateColors streaming;
    ThemeStateColors executingTool;
    ThemeStateColors providerWaiting;
    ThemeStateColors compacting;
    ThemeStateColors error;
    ThemeRgb agentBg{36, 88, 58};
    ThemeRgb agentFg{18, 20, 28};
    ThemeRgb pillBg{239, 241, 245};
    ThemeRgb pillFg{20, 20, 22};
    ThemeRgb fillerBg{18, 18, 28};

    struct Context {
      ThemeRgb bg{245, 194, 103};
      ThemeRgb icon{19, 20, 24};
      ThemeRgb low{131, 231, 154};
      ThemeRgb medium{245, 194, 103};
      ThemeRgb high{255, 107, 107};
    } context;
  } statusBar;

  struct AgentStrip {
    ThemeRgb bg{18, 18, 28};
    ThemeStateColors item;
    struct Pills {
      ThemeRgb slugBg{40, 70, 120};
      ThemeRgb slugFg{230, 236, 245};
      ThemeRgb purposeBg{70, 150, 140};
      ThemeRgb purposeFg{18, 20, 24};
      ThemeRgb modelBg{239, 241, 245};
      ThemeRgb modelFg{20, 20, 22};
      ThemeRgb stateBg{90, 96, 116};
      ThemeRgb stateFg{230, 236, 245};
      ThemeRgb toolBg{245, 194, 103};
      ThemeRgb toolFg{19, 20, 24};
      ThemeRgb contextBg{98, 110, 128};
    } pills;
  } agentStrip;

  struct Input {
    ThemeRgb bg{14, 16, 24};
    ThemeRgb fg{222, 228, 236};
    ThemeRgb prompt{96, 170, 255};
    ThemeRgb cursor{255, 255, 255};
    ThemeRgb placeholder{120, 124, 138};
  } input;

  struct Chat {
    ThemeRgb bg{14, 16, 24};
    ThemeRgb userPrefix{96, 170, 255};
    ThemeRgb agentPrefix{170, 214, 255};
    ThemeRgb timestamp{120, 124, 138};
  } chat;
};

class ThemeManager {
public:
  static ThemeManager& instance();

  void loadThemes();
  const ThemeSpec& currentTheme() const;
  std::vector<std::string> themeNames() const;
  bool setTheme(const std::string& name);

private:
  ThemeManager();

  void loadPersistedSelection();
  void persistSelection() const;

  std::vector<ThemeSpec> themes_;
  std::size_t currentThemeIndex_ = 0;
};

} // namespace firmius::tui2
