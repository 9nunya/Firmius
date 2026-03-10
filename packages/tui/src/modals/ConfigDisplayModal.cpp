#include "modals/ConfigDisplayModal.hpp"
#include "AgentRegistry.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>

namespace firmius::tui {

ftxui::Component ConfigDisplayModal::create(TuiState &state) {
  auto &h = firmius::core::Harness::instance();
  auto offset = std::make_shared<int>(0);

  // We capture everything by value to avoid referencing locals in the renderer
  // and we perform the data gathering once here to avoid calling
  // Harness::getConfig() every frame, which prevents the deadlock.

  const auto config = h.getConfig();
  std::string providerId = config.defaultProviderId;
  std::string modelId = config.defaultModelId;

  auto agentId = h.focusedAgentId();
  auto agent = agentId.empty()
                   ? nullptr
                   : firmius::core::AgentRegistry::instance().getAgent(agentId);
  if (agent) {
    const auto &agentConfig = agent->getContext().config;
    providerId = agentConfig.providerId;
    modelId = agentConfig.modelId;
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

  auto component = ftxui::Renderer([=]() {
    ftxui::Elements rows;
    rows.push_back(ftxui::hbox(
        {ftxui::text("Provider:    ") | ftxui::bold, ftxui::text(providerId)}));
    rows.push_back(ftxui::hbox(
        {ftxui::text("Model:       ") | ftxui::bold, ftxui::text(modelId)}));
    rows.push_back(ftxui::hbox(
        {ftxui::text("Unrestricted Paths: ") | ftxui::bold,
         ftxui::text(config.dangerouslySkipPermissions ? "[ENABLED]" : "[DISABLED]") |
             ftxui::color(config.dangerouslySkipPermissions ? ftxui::Color::Red
                                                            : ftxui::Color::Green)}));

    if (!apiKeyNames.empty()) {
      rows.push_back(ftxui::separator());
      rows.push_back(ftxui::text("API Keys:") | ftxui::bold);
      for (const auto &k : apiKeyNames) {
        rows.push_back(ftxui::text("  " + k) | ftxui::dim);
      }
    }

    if (!provOptLines.empty()) {
      rows.push_back(ftxui::separator());
      rows.push_back(ftxui::text("Provider Options:") | ftxui::bold);
      for (const auto &o : provOptLines) {
        rows.push_back(ftxui::text("  " + o) | ftxui::dim);
      }
    }

    rows.push_back(ftxui::text(""));
    rows.push_back(ftxui::hbox({ftxui::text(" [C] ") | ftxui::bold |
                                    ftxui::color(ftxui::Color::Blue),
                                ftxui::text("Change Model   "),
                                ftxui::text(" [P] ") | ftxui::bold |
                                    ftxui::color(ftxui::Color::Yellow),
                                ftxui::text("Toggle Permissions   "),
                                ftxui::text(" [ESC] ") | ftxui::bold |
                                    ftxui::color(ftxui::Color::GrayDark),
                                ftxui::text("Close")}) |
                   ftxui::center);

    return ftxui::window(ftxui::text(" Configuration ") | ftxui::bold |
                             ftxui::color(ftxui::Color::Cyan),
                         ftxui::vbox(rows) | ftxui::yframe |
                             ftxui::vscroll_indicator | ftxui::yflex |
                             ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 60) |
                             ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 20)) |
           ftxui::clear_under | ftxui::center;
  });

  return ftxui::CatchEvent(component, [&state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModalImmediate();
      return true;
    }
    if (event == ftxui::Event::Character('c') ||
        event == ftxui::Event::Character('C')) {
      state.popModalImmediate();
      state.openModal("model_picker");
      return true;
    }
    if (event == ftxui::Event::Character('p') ||
        event == ftxui::Event::Character('P')) {
      auto &h = firmius::core::Harness::instance();
      auto cfg = h.getConfig();
      cfg.dangerouslySkipPermissions = !cfg.dangerouslySkipPermissions;
      h.updateConfig(cfg);
      h.saveConfig();
      // Re-trigger modal to refresh view
      state.popModalImmediate();
      state.openModal("config_display");
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui
