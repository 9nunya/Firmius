#include "components/StatusBar.hpp"
#include "components/GlintEffect.hpp"
#include "utils/Icons.hpp"
#include "utils/ModelUtil.hpp"
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

StatusVisualConfig BuildStatusVisualConfig(const std::string &mode) {
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

  ftxui::Element Render() override {
    if (!model_) {
      return ftxui::text("");
    }

    syncGlint();

    using namespace firmius::shared;

    std::string mode = model_->status_text;
    auto visual = BuildStatusVisualConfig(mode);

    // 1. Status Segment (Icon)
    std::string status_icon = ICON_WAIT;
    if (mode == "streaming" || mode == "executing_tool")
      status_icon = ICON_GEAR;
    else if (mode == "error" || mode == "cancelled")
      status_icon = ICON_ERROR;
    else if (mode == "provider_waiting" || mode == "compacting")
      status_icon = ICON_WAIT;

    ftxui::Color status_bg = visual.badge_bg;
    ftxui::Color status_fg = visual.badge_fg;

    auto status_seg = ftxui::text(" " + status_icon + " ") | ftxui::bold |
                      ftxui::color(status_fg) | ftxui::bgcolor(status_bg);

    // Colors for subsequent segments
    ftxui::Color agent_bg = ftxui::Color::RGB(50, 50, 80);
    ftxui::Color agent_fg = ftxui::Color::RGB(200, 180, 255);
    ftxui::Color pill_bg = ftxui::Color::RGB(40, 40, 60);
    ftxui::Color pill_fg = ftxui::Color::RGB(160, 160, 200);
    ftxui::Color filler_bg = ftxui::Color::RGB(30, 30, 50);

    // Separators
    auto sep1 = ftxui::text(PL_LEFT_SEP) | ftxui::color(status_bg) |
                ftxui::bgcolor(agent_bg);
    auto sep2 = ftxui::text(PL_LEFT_SEP) | ftxui::color(agent_bg) |
                ftxui::bgcolor(pill_bg);
    auto sep3 = ftxui::text(PL_LEFT_SEP) | ftxui::color(pill_bg) |
                ftxui::bgcolor(filler_bg);

    // 2. Agent Segment
    ftxui::Element agent_name_el;
    agent_name_el =
        ftxui::text(" " +
                    firmius::shared::PrettifyModelName(model_->agent_name) +
                    " ") |
        ftxui::bold | ftxui::color(agent_fg) | ftxui::bgcolor(agent_bg);

    // 3. Model/Purpose Pill Segment
    std::string model_text =
        firmius::shared::PrettifyModelName(model_->model_name);
    if (!model_->purpose.empty()) {
      model_text = model_->purpose + " | " + model_text;
    }

    ftxui::Element pill_el;
    bool is_working = (mode == "streaming" || mode == "executing_tool" ||
                       mode == "provider_waiting" || mode == "compacting");
    if (is_working && glint_) {
      pill_el = glint_->Render() | ftxui::bgcolor(pill_bg);
    } else {
      pill_el = ftxui::text(" " + model_text + " ") | ftxui::color(pill_fg) |
                ftxui::bgcolor(pill_bg);
    }

    // 4. Context Segment (Right Aligned)
    ftxui::Element ctx_seg = ftxui::text("");
    ftxui::Color ctx_bg = ftxui::Color::RGB(40, 50, 70);
    if (model_->context_max > 0) {
      float ratio =
          static_cast<float>(model_->context_used) / model_->context_max;
      ftxui::Color ctx_color = ftxui::Color::RGB(100, 255, 150);
      if (ratio > 0.85f)
        ctx_color = ftxui::Color::RGB(255, 100, 100);
      else if (ratio > 0.60f)
        ctx_color = ftxui::Color::RGB(255, 220, 100);

      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f%%", ratio * 100.0f);
      std::string pct_str = buf;

      // Formatting helper for context numbers
      auto format_val = [](uint32_t val) -> std::string {
        if (val >= 1000000) {
          char val_buf[32];
          snprintf(val_buf, sizeof(val_buf), "%.1fM", val / 1000000.0f);
          // If ends in .0M, simplify to M
          std::string s = val_buf;
          if (s.size() > 3 && s.substr(s.size() - 3) == ".0M") {
            return s.substr(0, s.size() - 3) + "M";
          }
          return s;
        } else if (val >= 1000) {
          char val_buf[32];
          snprintf(val_buf, sizeof(val_buf), "%.1fk", val / 1000.0f);
          std::string s = val_buf;
          if (s.size() > 3 && s.substr(s.size() - 3) == ".0k") {
            return s.substr(0, s.size() - 3) + "k";
          }
          return s;
        }
        return std::to_string(val);
      };

      std::string combined_ctx = format_val(model_->context_used) + " / " +
                                 format_val(model_->context_max);

      ctx_seg = ftxui::hbox({
          ftxui::text(PL_RIGHT_SEP) | ftxui::color(ctx_bg) |
              ftxui::bgcolor(filler_bg),
          ftxui::text(" " + ICON_CONTEXT + " " + combined_ctx + " ") |
              ftxui::color(ftxui::Color::GrayLight) | ftxui::bgcolor(ctx_bg),
          ftxui::text(PL_RIGHT_SOFT_SEP) |
              ftxui::color(ftxui::Color::GrayDark) | ftxui::bgcolor(ctx_bg),
          ftxui::text(" " + pct_str + " ") | ftxui::bold |
              ftxui::color(ctx_color) | ftxui::bgcolor(ctx_bg),
      });
    }

    return ftxui::hbox({
               status_seg,
               sep1,
               agent_name_el,
               sep2,
               pill_el,
               sep3,
               ftxui::filler() | ftxui::bgcolor(filler_bg),
               ctx_seg,
           }) |
           ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1);
  }

private:
  void syncGlint() {
    using namespace firmius::shared;
    std::string model_text = PrettifyModelName(model_->model_name);
    if (!model_->purpose.empty()) {
      model_text = model_->purpose + " | " + model_text;
    }

    if (model_text == cached_name_ && model_->status_text == cached_status_ &&
        glint_)
      return;

    cached_name_ = model_text;
    cached_status_ = model_->status_text;

    GlintConfig cfg;
    cfg.target = GlintConfig::Target::Text;
    cfg.gradientColors = {ftxui::Color::Blue, ftxui::Color::White};
    cfg.glintSize = 14;
    cfg.intervalSeconds = 3;
    cfg.durationSeconds = 1.2f;
    cfg.easing = GlintEasing::EaseInOut;

    glint_ = GlintEffect(ftxui::text(" " + model_text + " ") |
                             ftxui::color(ftxui::Color::RGB(160, 160, 200)),
                         cfg);
  }

  std::shared_ptr<StatusBarModel> model_;
  ftxui::Component glint_;
  std::string cached_name_;
  std::string cached_status_;
};

} // namespace

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel> &model) {
  return std::make_shared<StatusBarComponentBase>(model);
}

} // namespace firmius::tui
