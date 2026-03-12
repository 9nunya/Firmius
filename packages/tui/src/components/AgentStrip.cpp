#include "components/AgentStrip.hpp"
#include "components/GlintEffect.hpp"
#include "utils/Icons.hpp"
#include "utils/ModelUtil.hpp"
#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <map>
#include <set>

namespace firmius::tui {

namespace {

class AgentStripComponentBase : public ftxui::ComponentBase {
public:
  explicit AgentStripComponentBase(std::shared_ptr<AgentStripModel> model)
      : model_(std::move(model)) {}

  ftxui::Element Render() override {
    if (!model_ || model_->items.empty()) {
      return ftxui::text("");
    }
    syncGlints();
    std::vector<ftxui::Element> rows;

    size_t count = std::min(model_->items.size(), kAgentStripVisibleRows);
    auto term_size = ftxui::Terminal::Size();
    bool wide_mode = term_size.dimx > 110;

    for (size_t i = 0; i < count; ++i) {
      const auto &item = model_->items[i];

      using namespace firmius::shared;

      // Colors for pills and rows
      ftxui::Color row_bg = ftxui::Color::RGB(25, 25, 45); // Main background
      if (item.is_focused) {
        row_bg = ftxui::Color::RGB(40, 50, 120);
      }

      // --- 1. Agent Name Area / Wide Pill ---
      ftxui::Element name_area;
      auto icon_el = ftxui::text(ICON_AGENT + " ") |
                     ftxui::color(ftxui::Color::RGB(150, 150, 255));

      if (wide_mode) {
        // [Slug] > [Purpose] > [Model]
        ftxui::Color slug_bg = ftxui::Color::RGB(50, 50, 90);
        ftxui::Color slug_fg = ftxui::Color::RGB(200, 200, 255);
        ftxui::Color purp_bg = ftxui::Color::RGB(40, 40, 70);
        ftxui::Color purp_fg = ftxui::Color::RGB(180, 180, 220);
        ftxui::Color mod_bg = ftxui::Color::RGB(30, 30, 50);
        ftxui::Color mod_fg = ftxui::Color::RGB(150, 150, 190);

        auto slug_el =
            ftxui::text(" " + firmius::shared::PrettifyModelName(item.title) +
                        " ") |
            ftxui::bold | ftxui::color(slug_fg) | ftxui::bgcolor(slug_bg);
        auto sep1 = ftxui::text(PL_LEFT_SEP) | ftxui::color(slug_bg) |
                    ftxui::bgcolor(purp_bg);
        auto purp_el = ftxui::text(" " + item.purpose + " ") |
                       ftxui::color(purp_fg) | ftxui::bgcolor(purp_bg);
        auto sep2 = ftxui::text(PL_LEFT_SEP) | ftxui::color(purp_bg) |
                    ftxui::bgcolor(mod_bg);
        auto mod_el = ftxui::text(" " + item.model_name + " ") |
                      ftxui::color(mod_fg) | ftxui::bgcolor(mod_bg);
        auto sep3 = ftxui::text(PL_LEFT_SEP) | ftxui::color(mod_bg) |
                    ftxui::bgcolor(row_bg);

        ftxui::Element wide_content =
            ftxui::hbox({icon_el | ftxui::bgcolor(slug_bg), slug_el, sep1,
                         purp_el, sep2, mod_el, sep3});

        if (item.is_busy) {
          auto it = glint_cache_.find(item.id + "_wide");
          if (it != glint_cache_.end()) {
            wide_content = ftxui::hbox({it->second->Render(), sep3});
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
                       ftxui::color(ftxui::Color::RGB(180, 160, 220));
          }
        } else {
          title_el =
              ftxui::text(firmius::shared::PrettifyModelName(item.title)) |
              ftxui::bold | ftxui::color(ftxui::Color::RGB(180, 160, 220));
        }
        name_area = ftxui::hbox({icon_el, title_el});
      }

      // --- 2. State Pill ---
      ftxui::Color state_bg = ftxui::Color::RGB(45, 45, 75);
      ftxui::Color state_fg = ftxui::Color::RGB(180, 180, 220);
      std::string status_icon = ICON_CHECK;
      std::string status_label = ""; // Empty for idle

      if (item.is_busy) {
        state_bg = ftxui::Color::RGB(40, 110, 70);
        state_fg = ftxui::Color::White;
        status_icon = ICON_GEAR;
        status_label = "BUSY";
      } else if (item.status_text == "error") {
        state_bg = ftxui::Color::RGB(160, 45, 45);
        state_fg = ftxui::Color::White;
        status_icon = ICON_ERROR;
        status_label = "ERR";
      }

      auto state_pill =
          ftxui::text(" " + status_icon +
                      (status_label.empty() ? "" : " " + status_label) + " ") |
          ftxui::bold | ftxui::color(state_fg) | ftxui::bgcolor(state_bg);

      // --- 3. Tool Pill ---
      ftxui::Color tool_bg = ftxui::Color::RGB(35, 40, 65);
      auto tool_pill =
          ftxui::hbox({
              ftxui::text(" " + ICON_TOOL + " ") |
                  ftxui::color(ftxui::Color::Cyan),
              ftxui::text(std::to_string(item.tool_call_count) + " ") |
                  ftxui::color(ftxui::Color::GrayLight),
          }) |
          ftxui::bgcolor(tool_bg);

      // --- 4. Context Pill ---
      ftxui::Color ctx_bg = ftxui::Color::RGB(40, 35, 65);
      ftxui::Color ctx_color = ftxui::Color::RGB(100, 255, 150);
      if (item.context_percent > 0.85f)
        ctx_color = ftxui::Color::RGB(255, 100, 100);
      else if (item.context_percent > 0.60f)
        ctx_color = ftxui::Color::RGB(255, 220, 100);

      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f%%", item.context_percent * 100.0f);
      auto ctx_pill =
          ftxui::hbox({
              ftxui::text(" " + ICON_CONTEXT + " ") | ftxui::color(ctx_color),
              ftxui::text(std::string(buf) + " ") | ftxui::color(ctx_color),
          }) |
          ftxui::bgcolor(ctx_bg);

      // --- 5. Focus Indicator (Right Arrow) ---
      ftxui::Element focus_arrow = ftxui::text("  ");
      if (item.is_focused) {
        focus_arrow = ftxui::text(" " + PL_LEFT_SEP) | ftxui::color(row_bg) |
                      ftxui::bgcolor(ftxui::Color::RGB(20, 20, 35));
      }

      // Cluster on the right: Use PL_RIGHT_SEP () pointing LEFT
      // Sequence: filler |  state |  tool |  ctx |  pct
      // Wait, let's keep it simple: [filler]  [state]  [tool]  [ctx]
      auto rsep1 = ftxui::text(PL_RIGHT_SEP) | ftxui::color(state_bg) |
                   ftxui::bgcolor(row_bg);
      auto rsep2 = ftxui::text(PL_RIGHT_SEP) | ftxui::color(tool_bg) |
                   ftxui::bgcolor(state_bg);
      auto rsep3 = ftxui::text(PL_RIGHT_SEP) | ftxui::color(ctx_bg) |
                   ftxui::bgcolor(tool_bg);

      auto row = ftxui::hbox({ftxui::text(" "), name_area, ftxui::filler(),
                              rsep1, state_pill, rsep2, tool_pill, rsep3,
                              ctx_pill, focus_arrow});

      rows.push_back(row | ftxui::bgcolor(row_bg));
    }
    return ftxui::vbox(std::move(rows)) |
           ftxui::bgcolor(ftxui::Color::RGB(20, 20, 35));
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

      // Compact glint
      if (!wide_mode) {
        if (glint_cache_.count(item.id) && title_cache_[item.id] == item.title)
          continue;
        GlintConfig cfg;
        cfg.target = GlintConfig::Target::Text;
        cfg.gradientColors = {ftxui::Color::RGB(0, 80, 255),
                              ftxui::Color::White,
                              ftxui::Color::RGB(0, 80, 255)};
        cfg.glintSize = 20;
        cfg.intervalSeconds = 2.0f;
        cfg.durationSeconds = 1.5f;
        cfg.easing = GlintEasing::EaseInOut;
        glint_cache_[item.id] = GlintEffect(
            ftxui::text(firmius::shared::PrettifyModelName(item.title)) |
                ftxui::bold | ftxui::color(ftxui::Color::RGB(180, 160, 220)),
            cfg);
        title_cache_[item.id] = item.title;
      } else {
        // Wide glint
        if (glint_cache_.count(item.id + "_wide") &&
            title_cache_[item.id + "_wide"] ==
                item.title + item.purpose + item.model_name)
          continue;
        GlintConfig cfg;
        cfg.target = GlintConfig::Target::Text;
        cfg.gradientColors = {ftxui::Color::RGB(100, 100, 255),
                              ftxui::Color::White,
                              ftxui::Color::RGB(100, 100, 255)};
        cfg.glintSize = 30;
        cfg.intervalSeconds = 3.0f;
        cfg.durationSeconds = 2.0f;
        cfg.easing = GlintEasing::EaseInOut;

        // Colors for wide pill glint
        ftxui::Color slug_bg = ftxui::Color::RGB(50, 50, 90);
        ftxui::Color purp_bg = ftxui::Color::RGB(40, 40, 70);
        ftxui::Color mod_bg = ftxui::Color::RGB(30, 30, 50);

        auto slug_el =
            ftxui::text(" " + firmius::shared::PrettifyModelName(item.title) +
                        " ") |
            ftxui::bold | ftxui::bgcolor(slug_bg);
        auto sep1 = ftxui::text(PL_LEFT_SEP) | ftxui::color(slug_bg) |
                    ftxui::bgcolor(purp_bg);
        auto purp_el =
            ftxui::text(" " + item.purpose + " ") | ftxui::bgcolor(purp_bg);
        auto sep2 = ftxui::text(PL_LEFT_SEP) | ftxui::color(purp_bg) |
                    ftxui::bgcolor(mod_bg);
        auto mod_el =
            ftxui::text(" " +
                        firmius::shared::PrettifyModelName(item.model_name) +
                        " ") |
            ftxui::bgcolor(mod_bg);

        auto icon_el = ftxui::text(ICON_AGENT + " ") | ftxui::bgcolor(slug_bg);

        glint_cache_[item.id + "_wide"] = GlintEffect(
            ftxui::hbox({icon_el, slug_el, sep1, purp_el, sep2, mod_el}) |
                ftxui::color(ftxui::Color::RGB(180, 180, 220)),
            cfg);
        title_cache_[item.id + "_wide"] =
            item.title + item.purpose + item.model_name;
      }
    }
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
};

} // namespace

ftxui::Component AgentStrip(const std::shared_ptr<AgentStripModel> &model) {
  return std::make_shared<AgentStripComponentBase>(model);
}

} // namespace firmius::tui
