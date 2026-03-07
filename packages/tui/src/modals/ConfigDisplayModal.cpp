#include "modals/ConfigDisplayModal.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "AgentRegistry.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>

namespace firmius::tui {

ftxui::Component ConfigDisplayModal::create(TuiState &state) {
  auto &h = firmius::core::Harness::instance();
  auto offset = std::make_shared<int>(0);

  auto component = ftxui::Renderer([=, &h]() {
    const auto &config = h.getConfig();
    std::string focusedAgent = h.focusedAgentId();
    std::string currentThread = h.currentThreadId();

    std::string providerId = config.defaultProviderId;
    std::string modelId = config.defaultModelId;
    float tempVal = config.defaultTemperature;
    std::optional<uint32_t> maxTokensVal = config.defaultMaxTokens;

    auto agent = firmius::core::AgentRegistry::instance().getAgent(focusedAgent);
    if (agent) {
      const auto &agentConfig = agent->getContext().config;
      providerId = agentConfig.providerId;
      modelId = agentConfig.modelId;
      tempVal = agentConfig.temperature;
      maxTokensVal = agentConfig.maxTokens;
    }

    std::string temperature = std::to_string(tempVal);
    temperature.erase(temperature.find_last_not_of('0') + 1, std::string::npos);
    if (temperature.back() == '.')
      temperature += '0';

    std::string maxTokens = maxTokensVal.has_value()
                                ? std::to_string(maxTokensVal.value())
                                : "default";

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

    ftxui::Elements rows;
    rows.push_back(ftxui::hbox(
        {ftxui::text("Provider:    ") | ftxui::bold, ftxui::text(providerId)}));
    rows.push_back(ftxui::hbox(
        {ftxui::text("Model:       ") | ftxui::bold, ftxui::text(modelId)}));
    rows.push_back(ftxui::hbox({ftxui::text("Temperature: ") | ftxui::bold,
                                ftxui::text(temperature)}));
    rows.push_back(ftxui::hbox(
        {ftxui::text("Max Tokens:  ") | ftxui::bold, ftxui::text(maxTokens)}));

    if (!currentThread.empty()) {
      rows.push_back(ftxui::separator());
      rows.push_back(ftxui::hbox({ftxui::text("Thread:      ") | ftxui::bold,
                                  ftxui::text(currentThread.substr(0, 12))}));
      if (!focusedAgent.empty()) {
        rows.push_back(ftxui::hbox({ftxui::text("Agent:       ") | ftxui::bold,
                                    ftxui::text(focusedAgent.substr(0, 12))}));
      }
    }

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
    rows.push_back(ftxui::text("Press ESC to close.") | ftxui::dim);

    return ftxui::window(ftxui::text(" Configuration ") | ftxui::bold |
                             ftxui::color(ftxui::Color::Cyan),
                         ftxui::vbox(rows) | ftxui::yframe |
                             ftxui::vscroll_indicator | ftxui::yflex |
                             ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 60) |
                             ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 20)) |
           ftxui::clear_under | ftxui::center;
  });

  return ftxui::CatchEvent(component, [offset, &state](ftxui::Event event) {
    if (event == ftxui::Event::Escape || event == ftxui::Event::Return) {
      state.popModal();
      return true;
    }
    if (event == ftxui::Event::ArrowUp) {
      (*offset)--;
      return true;
    }
    if (event == ftxui::Event::ArrowDown) {
      (*offset)++;
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui
