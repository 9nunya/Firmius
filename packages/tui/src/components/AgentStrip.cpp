#include "components/AgentStrip.hpp"
#include "components/GlintEffect.hpp"
#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <map>
#include <set>

namespace firmius::tui {

namespace {

static constexpr size_t MAX_STRIP_ITEMS = 3;

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
    size_t count = std::min(model_->items.size(), MAX_STRIP_ITEMS);
    for (size_t i = 0; i < count; ++i) {
      const auto &item = model_->items[i];
      auto bullet = ftxui::text(" ┄ ") | ftxui::dim |
                    ftxui::color(ftxui::Color::RGB(100, 100, 140));
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
        title_el = ftxui::text(item.title) | ftxui::bold |
                   ftxui::color(ftxui::Color::RGB(180, 160, 220));
      }
      auto purpose = ftxui::text(" " + item.purpose + " ") | ftxui::dim |
                     ftxui::color(ftxui::Color::RGB(120, 120, 160));
      ftxui::Color state_fg = ftxui::Color::White;
      ftxui::Color state_bg = ftxui::Color::RGB(60, 60, 80);
      if (item.is_busy) {
        state_bg = ftxui::Color::RGB(40, 120, 60);
      }
      auto state = ftxui::text(" " + item.status_text + " ") |
                   ftxui::color(state_fg) | ftxui::bgcolor(state_bg);
      ftxui::Color ctx_color = ftxui::Color::RGB(80, 200, 120);
      if (item.context_percent > 0.85f)
        ctx_color = ftxui::Color::RGB(200, 60, 60);
      else if (item.context_percent > 0.60f)
        ctx_color = ftxui::Color::RGB(220, 180, 60);
      auto ctx_pct =
          ftxui::text(" " +
                      std::to_string(
                          static_cast<int>(item.context_percent * 100)) +
                      "%") |
          ftxui::color(ctx_color);
      rows.push_back(ftxui::hbox({bullet, title_el, purpose, ftxui::filler(),
                                  state, ftxui::text(" "), ctx_pct}));
    }
    return ftxui::vbox(std::move(rows)) |
           ftxui::bgcolor(ftxui::Color::RGB(20, 20, 40));
  }

private:
  void syncGlints() {
    std::set<std::string> current_ids;
    size_t count = std::min(model_->items.size(), MAX_STRIP_ITEMS);
    for (size_t i = 0; i < count; ++i) {
      const auto &item = model_->items[i];
      if (!item.is_busy)
        continue;
      current_ids.insert(item.id);
      auto it = glint_cache_.find(item.id);
      if (it != glint_cache_.end()) {
        if (title_cache_[item.id] == item.title)
          continue;
      }
      GlintConfig cfg;
      cfg.target = GlintConfig::Target::Text;
      cfg.gradientColors = {ftxui::Color::Blue, ftxui::Color::White};
      cfg.glintSize = 14;
      cfg.intervalSeconds = 3;
      cfg.durationSeconds = 1.2f;
      cfg.easing = GlintEasing::EaseInOut;
      glint_cache_[item.id] = GlintEffect(
          ftxui::text(item.title) | ftxui::bold |
              ftxui::color(ftxui::Color::RGB(180, 160, 220)),
          cfg);
      title_cache_[item.id] = item.title;
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
