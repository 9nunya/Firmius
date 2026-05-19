#ifndef FIRMIUS_TUI_GLINTRENDERER_HPP
#define FIRMIUS_TUI_GLINTRENDERER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace firmius::tui {

struct GlintColorStop {
  int r = 255;
  int g = 255;
  int b = 255;
};

struct GlintConfig {
  std::vector<GlintColorStop> gradient = {
      {155, 205, 255},
      {255, 255, 255},
      {120, 170, 255},
  };
  int glintSize = 10;
  std::uint64_t durationMs = 4800;
  std::uint64_t intervalMs = 13500;
};

class GlintRenderer {
public:
  static std::string apply(const std::string& text,
                           const GlintConfig& config,
                           std::uint64_t nowMs,
                           bool active = true);
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_GLINTRENDERER_HPP
