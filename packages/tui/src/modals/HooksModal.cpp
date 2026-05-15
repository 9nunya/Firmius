#include "modals/HooksModal.hpp"

#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include "workflow/WorkflowLoader.hpp"

#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {
namespace {

std::string commandName(const firmius::core::Workflow &w) {
  if (w.slashCommand.has_value() && !w.slashCommand->empty()) {
    return *w.slashCommand;
  }
  return "/" + w.id;
}

std::string actionKind(const firmius::core::Workflow &w) {
  return firmius::core::workflowActionKindToString(w.action.kind);
}

struct Entry {
  bool is_hook;
  std::string primary;
  std::string detail;
  std::string source;
};

std::vector<Entry> buildEntries() {
  auto workflows = firmius::core::WorkflowLoader::instance().getAllWorkflows();
  std::vector<Entry> out;
  out.reserve(workflows.size());
  for (const auto &w : workflows) {
    if (w.isHook()) {
      Entry e;
      e.is_hook = true;
      e.primary = w.id;
      e.detail = "event=" +
                 firmius::core::workflowEventKindToString(w.trigger.event) +
                 "  action=" + actionKind(w) +
                 (w.trigger.block ? "  block=yes" : "");
      e.source = w.sourcePath;
      out.push_back(std::move(e));
    }
  }
  for (const auto &w : workflows) {
    if (!w.slashCommand.has_value()) {
      continue;
    }
    Entry e;
    e.is_hook = false;
    e.primary = commandName(w);
    e.detail = "id=" + w.id + "  action=" + actionKind(w) +
               (w.rawRemainder ? "  raw_remainder=yes" : "");
    e.source = w.sourcePath;
    out.push_back(std::move(e));
  }
  return out;
}

} // namespace

ftxui::Component HooksModal::create(TuiState &state) {
  auto entries = std::make_shared<std::vector<Entry>>(buildEntries());
  auto selected = std::make_shared<int>(0);
  auto message = std::make_shared<std::string>(
      "Enter to open source path info. R reload. Esc close.");

  auto reload = [entries, selected, message]() {
    firmius::core::WorkflowLoader::instance().init();
    firmius::core::hooks::HookRegistry::instance().reload();
    *entries = buildEntries();
    *selected = std::clamp(*selected, 0,
                           std::max(0, static_cast<int>(entries->size()) - 1));
    *message = "Reloaded workflows + hooks.";
  };

  auto renderer = ftxui::Renderer([entries, selected, message]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const std::size_t hook_count =
        firmius::core::hooks::HookRegistry::instance().size();

    ftxui::Elements rows;
    rows.push_back(
        ftxui::hbox({
            ftxui::text("Registered event hooks: ") |
                ftxui::color(theme.base.dim),
            ftxui::text(std::to_string(hook_count)) | ftxui::bold,
            ftxui::filler(),
            ftxui::text(std::to_string(entries->size()) + " entries") |
                ftxui::color(theme.base.dim),
        }));
    rows.push_back(ftxui::separatorLight() | ftxui::color(theme.modals.border));

    if (entries->empty()) {
      rows.push_back(ftxui::text("No workflows or hooks configured.") |
                     ftxui::color(theme.base.dim));
    }

    const std::string threadId =
        firmius::core::Harness::instance().currentThreadId();
    const auto recent =
        threadId.empty()
            ? std::vector<firmius::core::hooks::HookActivityRecord>{}
            : firmius::core::hooks::HookRegistry::instance().recentActivity(
                  threadId, 6);
    if (!recent.empty()) {
      rows.push_back(ftxui::text(""));
      rows.push_back(ftxui::text("Recent Activity") | ftxui::bold |
                     ftxui::color(theme.modals.title));
      for (const auto &record : recent) {
        rows.push_back(ftxui::text("  " + record.statusLine) |
                       ftxui::color(theme.base.dim));
      }
      rows.push_back(ftxui::text(""));
    }

    bool in_hooks_section = false;
    bool in_slash_section = false;
    for (std::size_t i = 0; i < entries->size(); ++i) {
      const auto &e = entries->at(i);
      if (e.is_hook && !in_hooks_section) {
        rows.push_back(ftxui::text("Event Hooks") | ftxui::bold |
                       ftxui::color(theme.modals.title));
        in_hooks_section = true;
      }
      if (!e.is_hook && !in_slash_section) {
        rows.push_back(ftxui::text("") );
        rows.push_back(ftxui::text("Slash Workflows") | ftxui::bold |
                       ftxui::color(theme.modals.title));
        in_slash_section = true;
      }
      const bool active = static_cast<int>(i) == *selected;
      auto line = ftxui::hbox({
          ftxui::text(active ? "> " : "  "),
          ftxui::text(e.primary) | ftxui::bold |
              ftxui::color(active ? theme.modals.highlight_fg
                                  : theme.modals.fg),
          ftxui::filler(),
          ftxui::text(e.detail) |
              ftxui::color(active ? theme.modals.highlight_fg
                                  : theme.base.dim),
      });
      if (active) {
        line = line | ftxui::bgcolor(theme.modals.highlight_bg) | ftxui::focus;
      }
      rows.push_back(line);
      auto src = ftxui::text("      " + e.source) |
                 ftxui::color(theme.base.dim);
      if (active) {
        src = src | ftxui::focus;
      }
      rows.push_back(src);
    }

    auto body = ftxui::vbox({
        ftxui::vbox(std::move(rows)) | ftxui::yframe |
            ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 18),
        ftxui::separatorLight() | ftxui::color(theme.modals.border),
        ftxui::text(*message) | ftxui::color(theme.base.dim),
    });

    return FlatModalPanel(theme, "Hooks & Workflows",
                          ModalSection(theme, std::move(body), theme.modals.bg),
                          110, 26);
  });

  return ftxui::CatchEvent(renderer, [entries, selected, message, reload,
                                      &state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModal();
      return true;
    }
    if (entries->empty()) {
      return false;
    }
    if (event == ftxui::Event::ArrowUp) {
      *selected = std::max(0, *selected - 1);
      return true;
    }
    if (event == ftxui::Event::ArrowDown) {
      *selected = std::min(static_cast<int>(entries->size()) - 1, *selected + 1);
      return true;
    }
    if (event == ftxui::Event::Character('r') ||
        event == ftxui::Event::Character('R')) {
      reload();
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui
