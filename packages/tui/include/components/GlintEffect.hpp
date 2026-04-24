#ifndef FIRMIUS_TUI_COMPONENTS_GLINT_EFFECT_HPP
#define FIRMIUS_TUI_COMPONENTS_GLINT_EFFECT_HPP

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/screen/color.hpp>

namespace firmius::tui {

struct GlintConfig {
  enum class Mode {
    // A traveling highlight band sweeping across the element.
    Sweep,
    // A global shimmer/pulse over the whole element.
    Pulse,
    // Sparse twinkles that flicker across the element.
    Sparkle,
  };
  Mode mode = Mode::Sweep;

  enum class Target { Text, Background };
  Target target = Target::Text;
  
  // Colors: the gradient color stops for the glint highlight
  // Default: white → light gray
  std::vector<ftxui::Color> gradientColors = {
    ftxui::Color::White,
    ftxui::Color::GrayLight,
  };

  // If true and target==Text, whitespace cells participate in the glint by
  // tinting their background. This makes sweeps look continuous across spaces.
  bool includeWhitespace = false;
  
  // Glint width in character cells
  int glintSize = 5;
  
  // Time between glint animations (seconds)
  float intervalSeconds = 2.0f;

  // Optional per-component phase offset (seconds) to de-sync many glints.
  // If not set, a stable default is derived from global time only.
  std::optional<float> phaseOffsetSeconds;
  
  // Easing function for the glint travel across the element
  // Signature: float(float) where input/output are 0..1
  std::function<float(float)> easing = nullptr; // nullptr = linear
  
  // Total duration of one glint pass (seconds)  
  float durationSeconds = 0.8f;

  // Sparkle mode parameters
  // - density: 0..1, fraction of cells eligible per frame.
  // - speed:   higher = faster flicker.
  float sparkleDensity = 0.06f;
  float sparkleSpeed = 1.8f;
};

namespace GlintEasing {
  float Linear(float t);
  float EaseInOut(float t);
  float EaseIn(float t);
  float EaseOut(float t);
}

// A function that wraps an FTXUI Element in a Component with animation
ftxui::Component GlintEffect(ftxui::Element child, const GlintConfig& config = {});

} // namespace firmius::tui

#endif // FIRMIUS_TUI_COMPONENTS_GLINT_EFFECT_HPP
