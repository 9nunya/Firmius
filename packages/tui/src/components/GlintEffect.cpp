#include "components/GlintEffect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <ftxui/component/animation.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace firmius::tui {

namespace GlintEasing {
float Linear(float t) { return t; }

float EaseInOut(float t) {
  return t < 0.5f ? 4.0f * t * t * t
                  : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

float EaseIn(float t) { return t * t * t; }

float EaseOut(float t) { return 1.0f - std::pow(1.0f - t, 3.0f); }
} // namespace GlintEasing

namespace {

static float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }

static float triangle01(float t) {
  // 0..1..0
  t = clamp01(t);
  return 1.0f - std::abs(2.0f * t - 1.0f);
}

static uint32_t mix32(uint32_t x) {
  // Simple avalanching mix for stable pseudo-randomness.
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

static float hash01(int x, int y, int t_bucket) {
  uint32_t v = 0x9e3779b9U;
  v ^= static_cast<uint32_t>(x) + 0x85ebca6bU;
  v = mix32(v);
  v ^= static_cast<uint32_t>(y) + 0xc2b2ae35U;
  v = mix32(v);
  v ^= static_cast<uint32_t>(t_bucket) + 0x27d4eb2fU;
  v = mix32(v);
  return static_cast<float>(v & 0x00FFFFFFU) / static_cast<float>(0x01000000U);
}

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

  void Render(ftxui::Screen &screen) override { ftxui::Node::Render(screen); }
};

class GlintNode : public NodeDecorator {
public:
  GlintNode(ftxui::Element child, const GlintConfig &config, float progress)
      : NodeDecorator(std::move(child)), config_(config), progress_(progress) {}

  void Render(ftxui::Screen &screen) override {
    NodeDecorator::Render(screen);

    int width = box_.x_max - box_.x_min + 1;
    if (width <= 0 || config_.glintSize <= 0)
      return;

    const float glint_size =
        static_cast<float>(std::min(config_.glintSize, width));

    if (config_.mode == GlintConfig::Mode::Pulse) {
      const float intensity = triangle01(progress_);
      if (intensity <= 0.0f)
        return;

      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        for (int x = box_.x_min; x <= box_.x_max; ++x) {
          auto &pixel = screen.PixelAt(x, y);
          ftxui::Color c =
              InterpolateGradient(config_.gradientColors, intensity);

          if (config_.target == GlintConfig::Target::Text) {
            if (pixel.character.empty() || pixel.character == " ")
              continue;
            pixel.foreground_color = c;
          } else {
            pixel.background_color = c;
          }
        }
      }
      return;
    }

    if (config_.mode == GlintConfig::Mode::Sparkle) {
      const float speed = std::max(0.05f, config_.sparkleSpeed);
      const int t_bucket =
          static_cast<int>(std::floor(progress_ * 120.0f * speed));
      const float density = clamp01(config_.sparkleDensity);

      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        for (int x = box_.x_min; x <= box_.x_max; ++x) {
          auto &pixel = screen.PixelAt(x, y);

          if (config_.target == GlintConfig::Target::Text) {
            // Do not tint whitespace.
            if (pixel.character.empty() || pixel.character == " ")
              continue;
          }
          float r = hash01(x, y, t_bucket);
          if (r > density)
            continue;

          float intensity =
              0.55f + 0.45f * (1.0f - (r / std::max(density, 1e-6f)));
          intensity = clamp01(intensity);
          ftxui::Color c =
              InterpolateGradient(config_.gradientColors, intensity);

          if (config_.target == GlintConfig::Target::Text) {
            if (pixel.character.empty() || pixel.character == " ")
              continue;
            pixel.foreground_color = c;
          } else {
            pixel.background_color = c;
          }
        }
      }
      return;
    }

    // Default: Sweep.
    const float start_x = static_cast<float>(box_.x_min) - glint_size;
    const float end_x = static_cast<float>(box_.x_max) + glint_size;
    const float exact_pos = start_x + (end_x - start_x) * progress_;

    const float half_span = glint_size / 2.0f;
    const float min_center = exact_pos - half_span;
    const float max_center = exact_pos + half_span;

    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      int min_x = static_cast<int>(std::floor(min_center));
      int max_x = static_cast<int>(std::ceil(max_center));
      for (int x = min_x; x <= max_x; ++x) {
        if (x < box_.x_min || x > box_.x_max)
          continue;

        float distance = std::abs(exact_pos - static_cast<float>(x));
        float color_t = 0.0f;
        if (half_span > 0.0f) {
          color_t = 1.0f - std::min(distance / half_span, 1.0f);
        }

        if (color_t <= 0.0f)
          continue;

        ftxui::Color c =
            InterpolateGradient(config_.gradientColors, clamp01(color_t));

        auto &pixel = screen.PixelAt(x, y);
        if (config_.target == GlintConfig::Target::Text) {
          // Do not tint whitespace.
          if (pixel.character.empty() || pixel.character == " ") {
            continue;
          }
          if (c == pixel.background_color) {
            pixel.foreground_color = ftxui::Color::White;
          } else {
            pixel.foreground_color = c;
          }
        } else {
          pixel.background_color = c;
        }
      }
    }
  }

private:
  GlintConfig config_;
  float progress_;

  ftxui::Color InterpolateGradient(const std::vector<ftxui::Color> &colors,
                                   float t) {
    if (colors.empty())
      return ftxui::Color::Default;
    if (colors.size() == 1)
      return colors[0];

    t = clamp01(t);
    float scaled_t = t * (colors.size() - 1);

    // NOTE: Keep interpolation monotonic. The glint strength profile is handled
    // by the sweep's distance->intensity mapping (color_t), not by reflecting
    // gradients here.

    int idx = static_cast<int>(scaled_t);
    if (idx >= static_cast<int>(colors.size()) - 1)
      return colors.back();

    float frac = scaled_t - idx;
    return ftxui::Color::Interpolate(frac, colors[idx], colors[idx + 1]);
  }
};

static auto global_start_time = std::chrono::steady_clock::now();

class GlintComponentBase : public ftxui::ComponentBase {
public:
  GlintComponentBase(ftxui::Element child, GlintConfig config)
      : child_(std::move(child)), config_(std::move(config)) {
    start_time_ = std::chrono::steady_clock::now();
  }

  ftxui::Element OnRender() override {
    auto now = std::chrono::steady_clock::now();
    float elapsed =
        std::chrono::duration<float>(now - global_start_time).count();

    if (config_.phaseOffsetSeconds.has_value())
      elapsed += *config_.phaseOffsetSeconds;

    const float width_scale =
        std::max(1.6f, static_cast<float>(config_.glintSize) / 10.0f);

    // TUI-wide: make glints appreciably slower.
    // "3x longer" means both travel duration and cooldown interval.
    constexpr float kGlintTimeScale = 3.0f;

    const float effective_duration =
        config_.durationSeconds * width_scale * kGlintTimeScale;
    const float effective_interval =
        config_.intervalSeconds * 1.4f * kGlintTimeScale;
    float cycle_duration = effective_duration + effective_interval;

    if (cycle_duration <= 0.0f || effective_duration <= 0.0f) {
      return child_;
    }

    float cycle_time = std::fmod(elapsed, cycle_duration);
    bool animating = cycle_time <= effective_duration;

    if (animating) {
      ftxui::animation::RequestAnimationFrame();
      float progress = std::min(1.0f, cycle_time / effective_duration);
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

ftxui::Component GlintEffect(ftxui::Element child, const GlintConfig &config) {
  return std::make_shared<GlintComponentBase>(std::move(child), config);
}

} // namespace firmius::tui
