#include "modals/ConfigDisplayModal.hpp"
#include "AgentRegistry.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include "utils/ModelUtil.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>

namespace firmius::tui {

namespace {

std::string permissionModeLabel(firmius::shared::ThreadPermissionMode mode) {
  switch (mode) {
  case firmius::shared::ThreadPermissionMode::Request:
    return "Request";
  case firmius::shared::ThreadPermissionMode::AlwaysAllow:
    return "Always Allow";
  case firmius::shared::ThreadPermissionMode::DenyAll:
    return "Deny All";
  }
  return "Request";
}

} // namespace

ftxui::Component ConfigDisplayModal::create(TuiState &state) {
  auto &h = firmius::core::Harness::instance();

  // We capture everything by value to avoid referencing locals in the renderer
  // and we perform the data gathering once here to avoid calling
  // Harness::getConfig() every frame, which prevents the deadlock.

  const auto config = h.getConfig();
  std::string providerId = config.defaultProviderId;
  std::string modelId = config.defaultModelId;
  std::string modelVariant = config.defaultModelVariant;
  std::string modelLabel = firmius::shared::PrettifyModelName(modelId);
  if (modelLabel.empty() || modelLabel == modelId) {
    modelLabel = modelId;
  } else {
    modelLabel += " (" + modelId + ")";
  }

  auto agentId = h.focusedAgentId();
  auto agent = agentId.empty()
                   ? nullptr
                   : firmius::core::AgentRegistry::instance().getAgent(agentId);
  if (agent) {
    const auto &agentConfig = agent->getContext().config;
    providerId = agentConfig.providerId;
    modelId = agentConfig.modelId;
    modelVariant = agentConfig.modelVariant;
    modelLabel = firmius::shared::PrettifyModelName(modelId);
    if (modelLabel.empty() || modelLabel == modelId) {
      modelLabel = modelId;
    } else {
      modelLabel += " (" + modelId + ")";
    }
  }

  std::vector<std::string> apiKeyNames;
  for (const auto &[key, val] : config.apiKeys) {
    std::string masked =
        val.size() > 8 ? val.substr(0, 4) + "..." + val.substr(val.size() - 4)
                       : "****";
    apiKeyNames.push_back(key + " = " + masked);
  }

  std::vector<std::string> provOptLines;
  for (const auto &[key, val] : config.providerOptions) {
    provOptLines.push_back(key + " = " + val);
  }

  auto component = ftxui::Renderer([=, &state]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const bool hasActiveThread = state.hasActiveThread();
    ftxui::Elements rows;
    rows.push_back(ftxui::hbox(
        {ftxui::text("Provider:    ") | ftxui::bold |
             ftxui::color(theme.modals.fg),
         ftxui::text(providerId) | ftxui::color(theme.modals.highlight_fg)}));
    rows.push_back(ftxui::hbox(
        {ftxui::text("Model:       ") | ftxui::bold |
             ftxui::color(theme.modals.fg),
         ftxui::text(modelLabel) | ftxui::color(theme.modals.highlight_fg)}));
    rows.push_back(ftxui::hbox(
        {ftxui::text("Variant:     ") | ftxui::bold |
             ftxui::color(theme.modals.fg),
         ftxui::text(modelVariant.empty() ? "(default)" : modelVariant) |
             ftxui::color(modelVariant.empty() ? theme.base.dim
                                               : theme.modals.highlight_fg)}));
    rows.push_back(ftxui::hbox(
        {ftxui::text("Permissions: ") | ftxui::bold |
             ftxui::color(theme.modals.fg),
         ftxui::text(hasActiveThread
                         ? ("Thread " + state.currentThreadId())
                         : "No active thread") |
             ftxui::color(hasActiveThread ? theme.modals.highlight_fg
                                          : theme.base.dim)}));
    rows.push_back(ftxui::hbox(
        {ftxui::text("Mode:        ") | ftxui::bold |
             ftxui::color(theme.modals.fg),
         ftxui::text(
             hasActiveThread
                 ? permissionModeLabel(state.currentThreadPermissionMode())
                 : "Unavailable") |
             ftxui::color(hasActiveThread ? theme.modals.highlight_fg
                                          : theme.base.dim)}));

    if (!apiKeyNames.empty()) {
      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.modals.border));
      rows.push_back(ftxui::text("API Keys:") | ftxui::bold |
                     ftxui::color(theme.modals.title));
      for (const auto &k : apiKeyNames) {
        rows.push_back(ftxui::text("  " + k) | ftxui::color(theme.base.dim));
      }
    }

    if (!provOptLines.empty()) {
      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.modals.border));
      rows.push_back(ftxui::text("Provider Options:") | ftxui::bold |
                     ftxui::color(theme.modals.title));
      for (const auto &o : provOptLines) {
        rows.push_back(ftxui::text("  " + o) | ftxui::color(theme.base.dim));
      }
    }

    rows.push_back(ftxui::text(""));
    rows.push_back(
        ftxui::hbox(
            {ftxui::text(" [C] ") | ftxui::bold |
                 ftxui::color(theme.modals.highlight_fg),
             ftxui::text("Change Model   ") | ftxui::color(theme.modals.fg),
             ftxui::text(" [P] ") | ftxui::bold |
                 ftxui::color(theme.modals.title),
             ftxui::text(hasActiveThread ? "Cycle Thread Mode   "
                                         : "No Active Thread   ") |
                 ftxui::color(hasActiveThread ? theme.modals.fg
                                              : theme.base.dim),
             ftxui::text(" [ESC] ") | ftxui::bold |
                 ftxui::color(theme.base.dim),
             ftxui::text("Close") | ftxui::color(theme.modals.fg)}) |
        ftxui::center);

    return FlatModalPanel(
        theme, "Configuration",
        ModalSection(
            theme,
            ftxui::vbox(rows) | ftxui::yframe | ftxui::vscroll_indicator |
                ftxui::yflex,
            theme.modals.bg),
        60, 20);
  });

  return ftxui::CatchEvent(component, [&state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModal();
      return true;
    }
    if (event == ftxui::Event::Character('c') ||
        event == ftxui::Event::Character('C')) {
      state.deferUiMutation([&state]() {
        state.popModalImmediate();
        state.openModal("model_picker");
      });
      return true;
    }
    if (event == ftxui::Event::Character('p') ||
        event == ftxui::Event::Character('P')) {
      state.cycleThreadPermissionMode();
      state.deferUiMutation([&state]() {
        state.popModalImmediate();
        state.openModal("config_display");
      });
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui
