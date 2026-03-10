#include "components/StatusBar.hpp"
#include "components/GlintEffect.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

namespace {

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
    ftxui::Color badge_bg = ftxui::Color::RGB(80, 80, 120);
    ftxui::Color badge_fg = ftxui::Color::RGB(220, 220, 255);
    if (mode == "streaming" || mode == "executing_tool") {
      badge_bg = ftxui::Color::RGB(40, 140, 80);
      badge_fg = ftxui::Color::RGB(220, 255, 220);
    } else if (mode == "idle" || mode == "awaiting_input") {
      badge_bg = ftxui::Color::RGB(60, 60, 100);
      badge_fg = ftxui::Color::RGB(180, 180, 220);
    } else if (mode == "error" || mode == "cancelled") {
      badge_bg = ftxui::Color::RGB(160, 50, 50);
      badge_fg = ftxui::Color::RGB(255, 200, 200);
    } else if (mode == "provider_waiting" || mode == "compacting") {
      badge_bg = ftxui::Color::RGB(140, 120, 40);
      badge_fg = ftxui::Color::RGB(255, 240, 180);
    }
    std::string mode_upper;
    for (char c : mode)
      mode_upper += static_cast<char>(toupper(c));

    if (mode_upper == "IDLE")
      glint_badge_ = ftxui::Renderer([=] {
        return ftxui::text(" " + mode_upper + " ") | ftxui::bold |
               ftxui::color(badge_fg) | ftxui::bgcolor(badge_bg);
      });
    else {
      GlintConfig cfg_badge;
      cfg_badge.target = GlintConfig::Target::Background;
      cfg_badge.gradientColors = {badge_bg, badge_fg};
      cfg_badge.glintSize = 14;
      cfg_badge.intervalSeconds = 3;
      cfg_badge.durationSeconds = 3;
      cfg_badge.easing = GlintEasing::EaseInOut;
      glint_badge_ =
          GlintEffect(ftxui::text(" " + mode_upper + " ") | ftxui::bold |
                          ftxui::color(badge_fg) | ftxui::bgcolor(badge_bg),
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
