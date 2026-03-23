#include "modals/PurposesModal.hpp"
#include "AgentRegistry.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "agents/PurposeLoader.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include <algorithm>
#include <unordered_set>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

namespace {

std::vector<std::string> sortedCategories(const firmius::shared::UserConfig &cfg) {
  std::vector<std::string> out;
  out.reserve(cfg.modelRouterCategories.size());
  for (const auto &[name, _] : cfg.modelRouterCategories) {
    out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace

PurposesModal::PurposesModal() = default;
PurposesModal::~PurposesModal() = default;

ftxui::Component PurposesModal::create(TuiState &state) {
  auto purposes =
      std::make_shared<std::vector<std::string>>(firmius::core::PurposeLoader::listPurposes());
  auto selected = std::make_shared<int>(0);
  auto message = std::make_shared<std::string>();

  const auto cfg = firmius::core::Harness::instance().getConfig();
  for (const auto &[purpose, _] : cfg.purposeRoutes) {
    purposes->push_back(purpose);
  }

  const auto threadId = state.currentThreadId();
  if (!threadId.empty()) {
    auto agentIds = firmius::core::Harness::instance().listAgents(threadId);
    for (const auto &agentId : agentIds) {
      auto agent = firmius::core::AgentRegistry::instance().getAgent(agentId);
      if (!agent) {
        continue;
      }
      const auto &persona = agent->getContext().config.personaName;
      if (!persona.empty()) {
        purposes->push_back(persona);
      }
    }
  }

  if (purposes->empty()) {
    purposes->push_back("lead");
  }
  std::sort(purposes->begin(), purposes->end());
  purposes->erase(std::unique(purposes->begin(), purposes->end()),
                  purposes->end());

  auto component = ftxui::Renderer([purposes, selected, message] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const auto cfg = firmius::core::Harness::instance().getConfig();
    const auto categories = sortedCategories(cfg);

    ftxui::Elements rows;
    rows.push_back(ftxui::text("Persona Route Mapping") | ftxui::bold |
                   ftxui::color(theme.modals.title));
    rows.push_back(
        ftxui::text("Use ←/→ to cycle category mapping for selected persona.") |
        ftxui::color(theme.base.dim));
    rows.push_back(ftxui::separatorLight() | ftxui::color(theme.modals.border));

    for (size_t i = 0; i < purposes->size(); ++i) {
      const auto &purpose = (*purposes)[i];
      const bool is_selected = static_cast<int>(i) == *selected;
      auto it = cfg.purposeRoutes.find(purpose);
      const std::string mapped =
          (it == cfg.purposeRoutes.end() || it->second.empty()) ? "(none)"
                                                                 : it->second;
      rows.push_back(ftxui::text((is_selected ? "> " : "  ") + purpose +
                                 " -> " + mapped) |
                     ftxui::color(is_selected ? theme.modals.highlight_fg
                                              : theme.modals.fg));
    }

    rows.push_back(ftxui::separatorLight() | ftxui::color(theme.modals.border));
    rows.push_back(ftxui::text("Categories: " +
                               (categories.empty() ? std::string("(none)")
                                                   : std::string{})) |
                   ftxui::color(theme.base.dim));
    if (!categories.empty()) {
      std::string category_line;
      for (size_t i = 0; i < categories.size(); ++i) {
        if (i > 0) {
          category_line += ", ";
        }
        category_line += categories[i];
      }
      rows.push_back(ftxui::text(category_line) | ftxui::color(theme.base.dim));
    }
    if (!message->empty()) {
      rows.push_back(ftxui::text(*message) | ftxui::color(theme.base.dim));
    }
    rows.push_back(
        ftxui::text("↑↓ select | ←/→ cycle mapping | C clear | Esc close") |
        ftxui::color(theme.base.dim));

    return FlatModalPanel(
        theme, "Purposes",
        ModalSection(theme, ftxui::vbox(std::move(rows)) | ftxui::yframe |
                                ftxui::vscroll_indicator | ftxui::yflex,
                     theme.modals.bg),
        98, 28);
  });

  return ftxui::CatchEvent(component, [&, purposes, selected, message](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModal();
      return true;
    }
    if (event == ftxui::Event::ArrowUp) {
      if (*selected > 0) {
        --(*selected);
      }
      return true;
    }
    if (event == ftxui::Event::ArrowDown) {
      if (*selected + 1 < static_cast<int>(purposes->size())) {
        ++(*selected);
      }
      return true;
    }

    if (*selected < 0 || *selected >= static_cast<int>(purposes->size())) {
      return false;
    }

    const std::string purpose = (*purposes)[*selected];
    auto cfg = firmius::core::Harness::instance().getConfig();
    const auto categories = sortedCategories(cfg);
    const auto it = cfg.purposeRoutes.find(purpose);
    const std::string current = (it == cfg.purposeRoutes.end()) ? "" : it->second;

    auto persist = [message](const firmius::shared::UserConfig &updated,
                             const std::string &status) {
      auto &h = firmius::core::Harness::instance();
      h.updateConfig(updated);
      h.saveConfig();
      *message = status;
    };

    if (event == ftxui::Event::Character('c') ||
        event == ftxui::Event::Character('C')) {
      cfg.purposeRoutes.erase(purpose);
      persist(cfg, "Cleared route for '" + purpose + "'.");
      return true;
    }

    if ((event == ftxui::Event::ArrowRight || event == ftxui::Event::ArrowLeft) &&
        !categories.empty()) {
      int current_index = -1;
      if (!current.empty()) {
        auto it_cat = std::find(categories.begin(), categories.end(), current);
        if (it_cat != categories.end()) {
          current_index = static_cast<int>(it_cat - categories.begin());
        }
      }

      int next_index = current_index;
      if (event == ftxui::Event::ArrowRight) {
        next_index = (current_index + 1) % static_cast<int>(categories.size());
      } else {
        next_index = (current_index <= 0)
                         ? static_cast<int>(categories.size() - 1)
                         : current_index - 1;
      }

      cfg.purposeRoutes[purpose] = categories[next_index];
      persist(cfg, "Mapped '" + purpose + "' -> '" + categories[next_index] + "'.");
      return true;
    }

    return false;
  });
}

} // namespace firmius::tui
