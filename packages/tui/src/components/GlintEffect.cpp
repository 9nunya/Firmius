#include "components/GlintEffect.hpp"

#include <cmath>
#include <algorithm>

#include <ftxui/component/animation.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace firmius::tui {

namespace GlintEasing {
  float Linear(float t) { return t; }
  
  float EaseInOut(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
  }
  
  float EaseIn(float t) {
    return t * t * t;
  }
  
  float EaseOut(float t) {
    return 1.0f - std::pow(1.0f - t, 3.0f);
  }
}

namespace {

class NodeDecorator : public ftxui::Node {
 public:
  NodeDecorator(ftxui::Element child) : ftxui::Node({std::move(child)}) {}

  void ComputeRequirement() override {
    ftxui::Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();
  }

  void SetBox(ftxui::Box box) override {
    ftxui::Node::SetBox(box);
    children_[0]->SetBox(box);
  }

  void Render(ftxui::Screen& screen) override {
    ftxui::Node::Render(screen);
  }
};

class GlintNode : public NodeDecorator {
public:
  GlintNode(ftxui::Element child, const GlintConfig& config, float progress)
      : NodeDecorator(std::move(child)), config_(config), progress_(progress) {}

  void Render(ftxui::Screen& screen) override {
    NodeDecorator::Render(screen);

    int width = box_.x_max - box_.x_min + 1;
    if (width <= 0 || config_.glintSize <= 0) return;

    float start_x = static_cast<float>(box_.x_min - 1);
    float end_x = static_cast<float>(box_.x_max + config_.glintSize);
    
    float exact_pos = start_x + (end_x - start_x) * progress_;
    int leading_x = static_cast<int>(std::round(exact_pos));

    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int i = 0; i < config_.glintSize; ++i) {
        int x = leading_x - i;
        if (x >= box_.x_min && x <= box_.x_max) {
          float color_t = 0.0f;
          if (config_.glintSize > 1) {
            color_t = static_cast<float>(i) / (config_.glintSize - 1);
          }
          
          ftxui::Color c = InterpolateGradient(config_.gradientColors, color_t);
          
          auto& pixel = screen.PixelAt(x, y);
          if (config_.target == GlintConfig::Target::Text) {
            pixel.foreground_color = c;
          } else {
            pixel.background_color = c;
          }
        }
      }
    }
  }

private:
  GlintConfig config_;
  float progress_;

  ftxui::Color InterpolateGradient(const std::vector<ftxui::Color>& colors, float t) {
    if (colors.empty()) return ftxui::Color::Default;
    if (colors.size() == 1) return colors[0];
    
    t = std::max(0.0f, std::min(1.0f, t));
    float scaled_t = t * (colors.size() - 1);
    int idx = static_cast<int>(scaled_t);
    if (idx >= static_cast<int>(colors.size()) - 1) return colors.back();
    
    float frac = scaled_t - idx;
    return ftxui::Color::Interpolate(frac, colors[idx], colors[idx + 1]);
  }
};

class GlintComponentBase : public ftxui::ComponentBase {
public:
  GlintComponentBase(ftxui::Element child, GlintConfig config)
      : child_(std::move(child)), config_(std::move(config)) {
    start_time_ = std::chrono::steady_clock::now();
  }

  ftxui::Element Render() override {
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - start_time_).count();
    float cycle_duration = config_.durationSeconds + config_.intervalSeconds;
    
    if (cycle_duration <= 0.0f || config_.durationSeconds <= 0.0f) {
      return child_;
    }

    float cycle_time = std::fmod(elapsed, cycle_duration);
    bool animating = cycle_time <= config_.durationSeconds;

    if (animating) {
      ftxui::animation::RequestAnimationFrame();
      float progress = cycle_time / config_.durationSeconds;
      if (config_.easing) {
        progress = config_.easing(progress);
      }
      return std::make_shared<GlintNode>(child_, config_, progress);
    }

    return child_;
  }

private:
  ftxui::Element child_;
  GlintConfig config_;
  std::chrono::steady_clock::time_point start_time_;
};

} // namespace

ftxui::Component GlintEffect(ftxui::Element child, const GlintConfig& config) {
  return std::make_shared<GlintComponentBase>(std::move(child), config);
}

} // namespace firmius::tui
