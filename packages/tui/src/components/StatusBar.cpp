#include "components/StatusBar.hpp"
#include "components/GlintEffect.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

namespace firmius::tui {

namespace {

struct StatusVisualConfig {
  ftxui::Color badge_bg;
  ftxui::Color badge_fg;
  std::vector<ftxui::Color> gradient_colors;
  bool use_gradient = true;
};

[[maybe_unused]] StatusVisualConfig BuildStatusVisualConfig(const std::string &mode) {
  StatusVisualConfig config;
  config.badge_bg = ftxui::Color::RGB(80, 80, 120);
  config.badge_fg = ftxui::Color::RGB(220, 220, 255);
  config.gradient_colors = {ftxui::Color::RGB(90, 100, 150),
                             ftxui::Color::RGB(180, 190, 220)};
  config.use_gradient = true;

  if (mode == "streaming" || mode == "executing_tool") {
    config.badge_bg = ftxui::Color::RGB(40, 140, 80);
    config.badge_fg = ftxui::Color::RGB(220, 255, 220);
    config.gradient_colors = {ftxui::Color::RGB(32, 110, 70),
                               ftxui::Color::RGB(106, 220, 150)};
  } else if (mode == "idle") {
    config.badge_bg = ftxui::Color::RGB(60, 60, 100);
    config.badge_fg = ftxui::Color::RGB(180, 180, 220);
    config.gradient_colors = {ftxui::Color::RGB(60, 60, 100),
                               ftxui::Color::RGB(100, 110, 160)};
    config.use_gradient = false;
  } else if (mode == "awaiting_input") {
    config.badge_bg = ftxui::Color::RGB(60, 60, 100);
    config.badge_fg = ftxui::Color::RGB(200, 200, 240);
    config.gradient_colors = {ftxui::Color::RGB(70, 80, 130),
                               ftxui::Color::RGB(160, 180, 230)};
  } else if (mode == "error" || mode == "cancelled") {
    config.badge_bg = ftxui::Color::RGB(160, 50, 50);
    config.badge_fg = ftxui::Color::RGB(255, 200, 200);
    config.gradient_colors.clear();
    config.use_gradient = false;
  } else if (mode == "provider_waiting" || mode == "compacting") {
    config.badge_bg = ftxui::Color::RGB(140, 120, 40);
    config.badge_fg = ftxui::Color::RGB(255, 240, 180);
    config.gradient_colors = {ftxui::Color::RGB(120, 110, 50),
                               ftxui::Color::RGB(245, 220, 110)};
  }

  return config;
}

class StatusBarComponentBase : public ftxui::ComponentBase {
public:
  explicit StatusBarComponentBase(std::shared_ptr<StatusBarModel> model)
      : model_(std::move(model)) {}

  ftxui::Element Render() {
    if (!model_) {
      return ftxui::text("");
    }

    syncGlint();
    ftxui::Element mode_badge;
    ftxui::Element agent_name_el;
    if (model_->is_active && glint_) {
      agent_name_el = glint_->Render();
    } else {
      agent_name_el = ftxui::text(" " + model_->agent_name + " ") |
                      ftxui::bold |
                      ftxui::color(ftxui::Color::RGB(180, 160, 220));
    }

    if (glint_badge_)
      mode_badge = glint_badge_->Render();

    auto model_section = ftxui::text("");
    if (!model_->model_name.empty()) {
      auto name_el = ftxui::text(" " + model_->model_name + " ") |
                     ftxui::color(ftxui::Color::RGB(160, 160, 200));
      if (!model_->model_variant.empty()) {
        auto variant_el = ftxui::text(" " + model_->model_variant + " ") |
                          ftxui::bold | ftxui::color(ftxui::Color::Orange1);
        model_section = ftxui::hbox({name_el, variant_el});
      } else {
        model_section = name_el;
      }
    }
    auto purpose_section = ftxui::text("");
    if (!model_->purpose.empty()) {
      purpose_section = ftxui::text(" " + model_->purpose + " ") |
                        ftxui::color(ftxui::Color::RGB(120, 120, 160));
    }
    ftxui::Element ctx_section = ftxui::text("");
    if (model_->context_max > 0) {
      float ratio =
          static_cast<float>(model_->context_used) / model_->context_max;
      ftxui::Color bar_color = ftxui::Color::RGB(80, 200, 120);
      if (ratio > 0.85f)
        bar_color = ftxui::Color::RGB(200, 60, 60);
      else if (ratio > 0.60f)
        bar_color = ftxui::Color::RGB(220, 180, 60);
      std::string used_k = std::to_string(model_->context_used / 1000);
      std::string max_k = std::to_string(model_->context_max / 1000) + "K";
      std::string pct = std::to_string(static_cast<int>(ratio * 100)) + "%";
      auto bar = ftxui::gauge(ratio) | ftxui::color(bar_color);
      ctx_section =
          ftxui::hbox({
              ftxui::text(" "),
              bar | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 10),
              ftxui::text(" " + used_k + "/" + max_k + " (" + pct + ")"),
          }) |
          ftxui::color(ftxui::Color::RGB(160, 160, 180));
    }
    return ftxui::hbox(
               {mode_badge, agent_name_el,
                ftxui::text(" ") | ftxui::color(ftxui::Color::RGB(60, 60, 80)),
                model_section, ftxui::filler(), purpose_section, ctx_section}) |
           ftxui::bgcolor(ftxui::Color::RGB(30, 30, 50));
  }

private:
  void syncGlint() {
    if (model_->agent_name == cached_name_ &&
        model_->status_text == cached_status_ && glint_)
      return;
    cached_name_ = model_->agent_name;
    cached_status_ = model_->status_text;
    GlintConfig cfg;
    cfg.target = GlintConfig::Target::Text;
    cfg.gradientColors = {ftxui::Color::Blue, ftxui::Color::White};
    cfg.glintSize = 14;
    cfg.intervalSeconds = 3;
    cfg.durationSeconds = 1.2f;
    cfg.easing = GlintEasing::EaseInOut;
    glint_ = GlintEffect(ftxui::text(" " + cached_name_ + " ") | ftxui::bold |
                             ftxui::color(ftxui::Color::RGB(180, 160, 220)),
                         cfg);

    std::string mode = model_->status_text;
    auto visual = BuildStatusVisualConfig(mode);
    std::string mode_upper;
    for (char c : mode)
      mode_upper += static_cast<char>(toupper(c));

    bool has_gradient = visual.use_gradient &&
                        visual.gradient_colors.size() >= 2;
    bool should_glint = has_gradient && mode_upper != "IDLE";
    if (!should_glint) {
      glint_badge_ = ftxui::Renderer([visual, mode_upper] {
        return ftxui::text(" " + mode_upper + " ") | ftxui::bold |
               ftxui::color(visual.badge_fg) |
               ftxui::bgcolor(visual.badge_bg);
      });
    } else {
      GlintConfig cfg_badge;
      cfg_badge.target = GlintConfig::Target::Background;
      cfg_badge.gradientColors = visual.gradient_colors;
      cfg_badge.glintSize = 14;
      cfg_badge.intervalSeconds = 3;
      cfg_badge.durationSeconds = 3;
      cfg_badge.easing = GlintEasing::EaseInOut;
      glint_badge_ = GlintEffect(
          ftxui::text(" " + mode_upper + " ") | ftxui::bold |
              ftxui::color(visual.badge_fg) |
              ftxui::bgcolor(visual.badge_bg),
          cfg_badge);
    }
  }

  std::shared_ptr<StatusBarModel> model_;
  ftxui::Component glint_;
  ftxui::Component glint_badge_;
  std::string cached_name_;
  std::string cached_status_;
};

} // namespace

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel> &model) {
  return std::make_shared<StatusBarComponentBase>(model);
}

} // namespace firmius::tui
