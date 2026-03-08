#ifndef FIRMIUS_TUI_COMPONENTS_GLINT_EFFECT_HPP
#define FIRMIUS_TUI_COMPONENTS_GLINT_EFFECT_HPP

#include <chrono>
#include <functional>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/screen/color.hpp>

namespace firmius::tui {

struct GlintConfig {
  enum class Target { Text, Background };
  Target target = Target::Text;
  
  // Colors: the gradient color stops for the glint highlight
  // Default: white → light gray
  std::vector<ftxui::Color> gradientColors = {
    ftxui::Color::White,
    ftxui::Color::GrayLight,
  };
  
  // Glint width in character cells
  int glintSize = 5;
  
  // Time between glint animations (seconds)
  float intervalSeconds = 2.0f;
  
  // Easing function for the glint travel across the element
  // Signature: float(float) where input/output are 0..1
  std::function<float(float)> easing = nullptr; // nullptr = linear
  
  // Total duration of one glint pass (seconds)  
  float durationSeconds = 0.8f;
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
