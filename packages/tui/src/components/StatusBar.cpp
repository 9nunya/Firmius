#include "components/StatusBar.hpp"
#include "ThemeManager.hpp"
#include "components/GlintEffect.hpp"
#include "utils/Icons.hpp"
#include "utils/ModelUtil.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

namespace firmius::tui {

namespace {

const StateColors &GetStateColorsForMode(const std::string &mode,
                                         const Theme &theme) {
  if (mode == "streaming")
    return theme.status_bar.streaming;
  if (mode == "executing_tool")
    return theme.status_bar.executing_tool;
  if (mode == "provider_waiting")
    return theme.status_bar.provider_waiting;
  if (mode == "compacting")
    return theme.status_bar.compacting;
  if (mode == "error" || mode == "cancelled")
    return theme.status_bar.error;
  return theme.status_bar.idle;
}

class StatusBarComponentBase : public ftxui::ComponentBase {
public:
  explicit StatusBarComponentBase(std::shared_ptr<StatusBarModel> model)
      : model_(std::move(model)) {}

  ftxui::Element OnRender() override {
    if (!model_) {
      return ftxui::text("");
    }

    syncGlint();

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    std::string mode = model_->status_text;
    const auto &state = GetStateColorsForMode(mode, theme);

    // Get terminal dimensions for responsive layout
    int term_width = ftxui::Terminal::Size().dimx;
    bool compact_mode = term_width <= 110;
    bool ultra_compact = term_width < 70;

    // 1. Status Segment (Icon)
    std::string status_icon = firmius::shared::ICON_WAIT;
    if (mode == "streaming" || mode == "executing_tool")
      status_icon = firmius::shared::ICON_GEAR;
    else if (mode == "error" || mode == "cancelled")
      status_icon = firmius::shared::ICON_ERROR;

    ftxui::Color status_bg = state.normal.bg;
    ftxui::Color status_fg = state.normal.fg;

    auto status_seg = ftxui::text(" " + status_icon + " ") | ftxui::bold |
                      ftxui::color(status_fg) | ftxui::bgcolor(status_bg);

    // Colors for subsequent segments
    ftxui::Color agent_bg = theme.status_bar.agent_bg;
    ftxui::Color agent_fg = theme.status_bar.agent_fg;
    ftxui::Color pill_bg = theme.status_bar.pill_bg;
    ftxui::Color pill_fg = theme.status_bar.pill_fg;
    ftxui::Color filler_bg = theme.status_bar.filler_bg;

    // Separators
    auto sep1 = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                ftxui::color(status_bg) | ftxui::bgcolor(agent_bg);
    auto sep2 = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                ftxui::color(agent_bg) | ftxui::bgcolor(pill_bg);
    auto sep3 = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                ftxui::color(pill_bg) | ftxui::bgcolor(filler_bg);

    // 2. Agent Segment (abbreviated in compact mode)
    std::string agent_name = firmius::shared::PrettifyModelName(model_->agent_name);
    if (compact_mode) {
      // Shorten agent name: "Firmius Orchestrator" -> "Firmius"
      size_t space_pos = agent_name.find(' ');
      if (space_pos != std::string::npos) {
        agent_name = agent_name.substr(0, space_pos);
      }
      // If ultra compact, just first 4 chars
      if (ultra_compact && agent_name.length() > 4) {
        agent_name = agent_name.substr(0, 4);
      }
    }

    ftxui::Element agent_name_el;
    agent_name_el = ftxui::text(" " + agent_name + " ") |
        ftxui::bold | ftxui::color(agent_fg) | ftxui::bgcolor(agent_bg);

    // 3. Model/Purpose Pill Segment
    std::string model_text =
        firmius::shared::PrettifyModelName(model_->model_name);
    
    // In compact mode, simplify model name
    if (compact_mode) {
      // "GPT 5.1 Codex Mini" -> "GPT 5.1"
      size_t last_space = model_text.rfind(' ');
      if (last_space != std::string::npos) {
        // Check if last word is likely a qualifier (Mini, Pro, etc.)
        std::string last_word = model_text.substr(last_space + 1);
        if (last_word == "Mini" || last_word == "Pro" || last_word == "Max" ||
            last_word == "Ultra" || last_word == "Nano") {
          model_text = model_text.substr(0, last_space);
        }
      }
    }
    
    if (!model_->purpose.empty() && !compact_mode) {
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
    ftxui::Color ctx_bg = theme.status_bar.context.bg;
    if (model_->context_max > 0 && !ultra_compact) {
      float ratio =
          static_cast<float>(model_->context_used) / model_->context_max;
      ftxui::Color ctx_color = theme.status_bar.context.low;
      if (ratio > 0.85f)
        ctx_color = theme.status_bar.context.high;
      else if (ratio > 0.60f)
        ctx_color = theme.status_bar.context.medium;

      char buf[32];
      snprintf(buf, sizeof(buf), "%.0f%%", ratio * 100.0f);
      std::string pct_str = buf;

      if (compact_mode) {
        // Just show percentage in compact mode
        ctx_seg = ftxui::hbox({
            ftxui::text(firmius::shared::PL_RIGHT_SEP) | ftxui::color(ctx_bg) |
                ftxui::bgcolor(filler_bg),
            ftxui::text(" " + pct_str + " ") | ftxui::bold |
                ftxui::color(ctx_color) | ftxui::bgcolor(ctx_bg),
        });
      } else {
        // Formatting helper for context numbers
        auto format_val = [](uint32_t val) -> std::string {
          if (val >= 1000000) {
            char val_buf[32];
            snprintf(val_buf, sizeof(val_buf), "%.1fM", val / 1000000.0f);
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
            ftxui::text(firmius::shared::PL_RIGHT_SEP) | ftxui::color(ctx_bg) |
                ftxui::bgcolor(filler_bg),
            ftxui::text(" " + firmius::shared::ICON_CONTEXT + " " + combined_ctx +
                        " ") |
                ftxui::color(theme.status_bar.context.icon) |
                ftxui::bgcolor(ctx_bg),
            ftxui::text(firmius::shared::PL_RIGHT_SOFT_SEP) |
                ftxui::color(theme.base.dim) | ftxui::bgcolor(ctx_bg),
            ftxui::text(" " + pct_str + " ") | ftxui::bold |
                ftxui::color(ctx_color) | ftxui::bgcolor(ctx_bg),
        });
      }
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

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    if (model_text == cached_name_ && model_->status_text == cached_status_ &&
        theme.name == cached_theme_name_ && glint_)
      return;

    cached_name_ = model_text;
    cached_status_ = model_->status_text;
    cached_theme_name_ = theme.name;

    const auto &state = GetStateColorsForMode(model_->status_text, theme);

    GlintConfig cfg;
    cfg.target = GlintConfig::Target::Text;
    cfg.gradientColors =
        state.glint.empty()
            ? std::vector<ftxui::Color>{ftxui::Color::Blue, ftxui::Color::White}
            : state.glint;
    cfg.glintSize = 14;
    cfg.intervalSeconds = 3;
    cfg.durationSeconds = 1.2f;
    cfg.easing = GlintEasing::EaseInOut;

    glint_ = GlintEffect(ftxui::text(" " + model_text + " ") |
                             ftxui::color(theme.status_bar.pill_fg),
                         cfg);
  }

  std::shared_ptr<StatusBarModel> model_;
  ftxui::Component glint_;
  std::string cached_name_;
  std::string cached_status_;
  std::string cached_theme_name_;
};

} // namespace

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel> &model) {
  return std::make_shared<StatusBarComponentBase>(model);
}

} // namespace firmius::tui
