#include "components/AgentStrip.hpp"
#include "ThemeManager.hpp"
#include "components/GlintEffect.hpp"
#include "utils/Icons.hpp"
#include "utils/ModelUtil.hpp"
#include <algorithm>
#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/animation.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <map>
#include <set>

namespace firmius::tui {

namespace {

std::string spinnerFrame() {
  static const std::vector<std::string> frames = {
      "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count();
  size_t idx = static_cast<size_t>((ms / 80) % frames.size());
  ftxui::animation::RequestAnimationFrame();
  return frames[idx];
}

std::string formatWorkingDuration(uint64_t elapsed_ms) {
  uint64_t total_seconds = elapsed_ms / 1000;
  uint64_t hours = total_seconds / 3600;
  uint64_t minutes = (total_seconds / 60) % 60;
  uint64_t seconds = total_seconds % 60;
  if (hours > 0) {
    return std::to_string(hours) + "h, " + std::to_string(minutes) + "m";
  }
  if (minutes > 0) {
    return std::to_string(minutes) + "m, " + std::to_string(seconds) + "s";
  }
  return std::to_string(seconds) + "s";
}

class AgentStripComponentBase : public ftxui::ComponentBase {
public:
  explicit AgentStripComponentBase(std::shared_ptr<AgentStripModel> model)
      : model_(std::move(model)) {}

  ftxui::Element OnRender() override {
    if (!model_ || model_->items.empty()) {
      return ftxui::text("");
    }
    syncGlints();
    std::vector<ftxui::Element> rows;

    size_t count = std::min(model_->items.size(), kAgentStripVisibleRows);
    auto term_size = ftxui::Terminal::Size();
    bool wide_mode = term_size.dimx > 110;
    uint64_t now_ms =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    for (size_t i = 0; i < count; ++i) {
      const auto &item = model_->items[i];

      const auto &state =
          item.is_busy
              ? theme.agent_strip.item.busy
              : (item.status_text == "error"
                     ? theme.agent_strip.item.error
                     : (item.is_focused ? theme.agent_strip.item.focused
                                        : theme.agent_strip.item.normal));

      // Colors for pills and rows
      ftxui::Color row_bg = state.bg;

      // --- 1. Agent Name Area / Wide Pill ---
      ftxui::Element name_area;
      std::string busy_spinner;
      if (item.is_busy) {
        busy_spinner = spinnerFrame();
      }
      std::string primary_icon = firmius::shared::ICON_CHECK;
      if (item.is_busy) {
        primary_icon = busy_spinner;
      } else if (item.status_text == "error") {
        primary_icon = firmius::shared::ICON_ERROR;
      }
      auto icon_el =
          ftxui::text(primary_icon + " ") |
          ftxui::color(theme.agent_strip.pills.slug_fg); // use slug_fg for icon

      if (wide_mode) {
        // [Slug] > [Purpose] > [Model]
        ftxui::Color slug_bg = theme.agent_strip.pills.slug_bg;
        ftxui::Color slug_fg = theme.agent_strip.pills.slug_fg;
        ftxui::Color purp_bg = theme.agent_strip.pills.purpose_bg;
        ftxui::Color purp_fg = theme.agent_strip.pills.purpose_fg;
        ftxui::Color mod_bg = theme.agent_strip.pills.model_bg;
        ftxui::Color mod_fg = theme.agent_strip.pills.model_fg;

        auto slug_el =
            ftxui::text(" " + item.title + " ") |
            ftxui::bold | ftxui::color(slug_fg) | ftxui::bgcolor(slug_bg);
        auto sep1 = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                    ftxui::color(slug_bg) | ftxui::bgcolor(purp_bg);
        auto purp_el = ftxui::text(" " + item.purpose + " ") |
                       ftxui::color(purp_fg) | ftxui::bgcolor(purp_bg);
        auto sep2 = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                    ftxui::color(purp_bg) | ftxui::bgcolor(mod_bg);
        auto mod_el =
            ftxui::text(" " +
                        firmius::shared::PrettifyModelName(item.model_name) +
                        " ") |
            ftxui::color(mod_fg) | ftxui::bgcolor(mod_bg);
        auto sep3 = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                    ftxui::color(mod_bg) | ftxui::bgcolor(row_bg);

        ftxui::Element wide_content =
            ftxui::hbox({icon_el | ftxui::bgcolor(slug_bg), slug_el, sep1,
                         purp_el, sep2, mod_el, sep3});

        if (item.is_busy) {
          auto it = glint_cache_.find(item.id + "_wide");
          if (it != glint_cache_.end()) {
            wide_content =
                ftxui::hbox({icon_el | ftxui::bgcolor(slug_bg),
                             it->second->Render(), sep3});
          }
        }
        name_area = wide_content;
      } else {
        ftxui::Element title_el;
        if (item.is_busy) {
          auto it = glint_cache_.find(item.id);
          if (it != glint_cache_.end()) {
            title_el = it->second->Render();
          } else {
            title_el = ftxui::text(item.title) | ftxui::bold |
                       ftxui::color(theme.agent_strip.pills.slug_fg);
          }
        } else {
          title_el = ftxui::text(item.title) | ftxui::bold |
                     ftxui::color(theme.agent_strip.pills.slug_fg);
        }
        name_area = ftxui::hbox({icon_el, title_el});
      }

      // --- 2. State Pill ---
      ftxui::Color state_bg = theme.agent_strip.pills.state_bg;
      ftxui::Color state_fg = theme.agent_strip.pills.state_fg;
      std::string status_icon = firmius::shared::ICON_CHECK;
      std::string status_label = ""; // Empty for idle

      if (item.is_busy) {
        status_icon = busy_spinner;
        status_label = "WORKING";
        if (item.working_since_ms.has_value()) {
          uint64_t elapsed = 0;
          if (now_ms >= item.working_since_ms.value()) {
            elapsed = now_ms - item.working_since_ms.value();
          }
          status_label += " (" + formatWorkingDuration(elapsed) + ")";
        }
      } else if (item.status_text == "error") {
        status_icon = firmius::shared::ICON_ERROR;
        status_label = "ERR";
      }

      auto state_pill =
          ftxui::text(" " + status_icon +
                      (status_label.empty() ? "" : " " + status_label) + " ") |
          ftxui::bold | ftxui::color(state_fg) | ftxui::bgcolor(state_bg);

      // --- 3. Tool Pill ---
      ftxui::Color tool_bg = theme.agent_strip.pills.tool_bg;
      auto tool_pill =
          ftxui::hbox({
              ftxui::text(" " + firmius::shared::ICON_TOOL + " ") |
                  ftxui::color(theme.agent_strip.pills.tool_fg),
              ftxui::text(std::to_string(item.tool_call_count) + " ") |
                  ftxui::color(theme.agent_strip.pills.tool_fg),
          }) |
          ftxui::bgcolor(tool_bg);

      // --- 4. Context Pill ---
      ftxui::Color ctx_bg = theme.agent_strip.pills.context_bg;
      ftxui::Color ctx_color = theme.status_bar.context.low;
      if (item.context_percent > 0.85f)
        ctx_color = theme.status_bar.context.high;
      else if (item.context_percent > 0.60f)
        ctx_color = theme.status_bar.context.medium;

      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f%%", item.context_percent * 100.0f);
      auto ctx_pill =
          ftxui::hbox({
              ftxui::text(" " + firmius::shared::ICON_CONTEXT + " ") |
                  ftxui::color(ctx_color),
              ftxui::text(std::string(buf) + " ") | ftxui::color(ctx_color),
          }) |
          ftxui::bgcolor(ctx_bg);

      // --- 5. Focus Indicator (Right Arrow) ---
      ftxui::Element focus_arrow = ftxui::text("");
      if (item.is_focused) {
        focus_arrow = ftxui::text(firmius::shared::PL_LEFT_SEP + " ") |
                      ftxui::color(row_bg) | ftxui::bgcolor(ctx_bg);
      }

      // Sequence: filler |  state |  tool |  ctx |  pct
      // Wait, let's keep it simple: [filler]  [state]  [tool]  [ctx]
      auto rsep1 = ftxui::text(firmius::shared::PL_RIGHT_SEP) |
                   ftxui::color(state_bg) | ftxui::bgcolor(row_bg);
      auto rsep2 = ftxui::text(firmius::shared::PL_RIGHT_SEP) |
                   ftxui::color(tool_bg) | ftxui::bgcolor(state_bg);
      auto rsep3 = ftxui::text(firmius::shared::PL_RIGHT_SEP) |
                   ftxui::color(ctx_bg) | ftxui::bgcolor(tool_bg);

      auto row = ftxui::hbox({name_area, ftxui::filler(),
                              rsep1, state_pill, rsep2, tool_pill, rsep3,
                              ctx_pill, focus_arrow});

      rows.push_back(row | ftxui::bgcolor(row_bg));
    }
    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme.agent_strip.bg);
  }

private:
  void syncGlints() {
    std::set<std::string> current_ids;
    size_t count = std::min(model_->items.size(), kAgentStripVisibleRows);
    auto term_size = ftxui::Terminal::Size();
    bool wide_mode = term_size.dimx > 110;

    for (size_t i = 0; i < count; ++i) {
      const auto &item = model_->items[i];
      if (!item.is_busy)
        continue;

      using namespace firmius::shared;

      current_ids.insert(item.id);
      if (wide_mode)
        current_ids.insert(item.id + "_wide");

      const auto &theme = ThemeManager::instance().getCurrentTheme();
      if (!wide_mode) {
        if (glint_cache_.count(item.id) &&
            title_cache_[item.id] == item.title &&
            cached_theme_name_ == theme.name)
          continue;
        const auto &state = theme.agent_strip.item;
        GlintConfig cfg;
        cfg.target = GlintConfig::Target::Text;
        cfg.gradientColors =
            state.glint.empty()
                ? std::vector<ftxui::Color>{ftxui::Color::RGB(0, 80, 255),
                                            ftxui::Color::White,
                                            ftxui::Color::RGB(0, 80, 255)}
                : state.glint;
        cfg.glintSize = 20;
        cfg.intervalSeconds = 2.0f;
        cfg.durationSeconds = 1.5f;
        cfg.easing = GlintEasing::EaseInOut;
        glint_cache_[item.id] = GlintEffect(
            ftxui::text(item.title) | ftxui::bold |
                ftxui::color(theme.agent_strip.pills.slug_fg),
            cfg);
        title_cache_[item.id] = item.title;
      } else {
        // Wide glint
        if (glint_cache_.count(item.id + "_wide") &&
            title_cache_[item.id + "_wide"] ==
                item.title + item.purpose + item.model_name &&
            cached_theme_name_ == theme.name)
          continue;
        const auto &state = theme.agent_strip.item;
        GlintConfig cfg;
        cfg.target = GlintConfig::Target::Text;
        cfg.gradientColors =
            state.glint.empty()
                ? std::vector<ftxui::Color>{ftxui::Color::RGB(100, 100, 255),
                                            ftxui::Color::White,
                                            ftxui::Color::RGB(100, 100, 255)}
                : state.glint;
        cfg.glintSize = 30;
        cfg.intervalSeconds = 3.0f;
        cfg.durationSeconds = 2.0f;
        cfg.easing = GlintEasing::EaseInOut;

        // Colors for wide pill glint
        ftxui::Color slug_bg = theme.agent_strip.pills.slug_bg;
        ftxui::Color purp_bg = theme.agent_strip.pills.purpose_bg;
        ftxui::Color mod_bg = theme.agent_strip.pills.model_bg;

        auto slug_el =
            ftxui::text(" " + item.title + " ") |
            ftxui::bold | ftxui::bgcolor(slug_bg);
        auto sep1 = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                    ftxui::color(slug_bg) | ftxui::bgcolor(purp_bg);
        auto purp_el =
            ftxui::text(" " + item.purpose + " ") | ftxui::bgcolor(purp_bg);
        auto sep2 = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                    ftxui::color(purp_bg) | ftxui::bgcolor(mod_bg);
        auto mod_el =
            ftxui::text(" " +
                        firmius::shared::PrettifyModelName(item.model_name) +
                        " ") |
            ftxui::bgcolor(mod_bg);
        glint_cache_[item.id + "_wide"] = GlintEffect(
            ftxui::hbox({slug_el, sep1, purp_el, sep2, mod_el}) |
                ftxui::color(theme.agent_strip.pills.slug_fg),
            cfg);
        title_cache_[item.id + "_wide"] =
            item.title + item.purpose + item.model_name;
      }
    }
    cached_theme_name_ = ThemeManager::instance().getCurrentTheme().name;
    for (auto it = glint_cache_.begin(); it != glint_cache_.end();) {
      if (current_ids.find(it->first) == current_ids.end()) {
        title_cache_.erase(it->first);
        it = glint_cache_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::shared_ptr<AgentStripModel> model_;
  std::map<std::string, ftxui::Component> glint_cache_;
  std::map<std::string, std::string> title_cache_;
  std::string cached_theme_name_;
};

} // namespace

ftxui::Component AgentStrip(const std::shared_ptr<AgentStripModel> &model) {
  return std::make_shared<AgentStripComponentBase>(model);
}

} // namespace firmius::tui
