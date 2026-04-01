#include "modals/ConfigDisplayModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include "components/ScrollableBox.hpp"
#include "utils/ModelUtil.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace firmius::tui {

namespace {

enum class SettingType {
  Bool,
  Info,
};

struct SettingEntry {
  std::string label;
  std::string description;
  SettingType type;
  std::string info_value;
  bool bool_value = false;
  bool read_only = false;
};

std::string formatApiKey(const std::string &val) {
  return val.size() > 8 ? val.substr(0, 4) + "..." + val.substr(val.size() - 4) : "****";
}

} // namespace

ftxui::Component ConfigDisplayModal::create(TuiState &state) {
  auto settings = std::make_shared<std::vector<SettingEntry>>();
  auto selected = std::make_shared<int>(0);
  auto message = std::make_shared<std::string>();
  auto rowBoxes = std::make_shared<std::vector<ftxui::Box>>();

  auto rebuildSettings = [settings, selected]() {
    auto &h = firmius::core::Harness::instance();
    auto config = h.getConfig();

    settings->clear();

    SettingEntry providerEntry;
    providerEntry.label = "Provider";
    providerEntry.description = "Default LLM provider";
    providerEntry.type = SettingType::Info;
    providerEntry.info_value = config.defaultProviderId;
    providerEntry.read_only = true;
    settings->push_back(providerEntry);

    std::string modelLabel = firmius::shared::PrettifyModelName(config.defaultModelId);
    if (modelLabel.empty() || modelLabel == config.defaultModelId) {
      modelLabel = config.defaultModelId;
    } else {
      modelLabel += " (" + config.defaultModelId + ")";
    }
    SettingEntry modelEntry;
    modelEntry.label = "Model";
    modelEntry.description = "Default model ID";
    modelEntry.type = SettingType::Info;
    modelEntry.info_value = modelLabel;
    modelEntry.read_only = true;
    settings->push_back(modelEntry);

    SettingEntry variantEntry;
    variantEntry.label = "Variant";
    variantEntry.description = "Model variant/routing";
    variantEntry.type = SettingType::Info;
    variantEntry.info_value = config.defaultModelVariant.empty() ? "(default)" : config.defaultModelVariant;
    variantEntry.read_only = true;
    settings->push_back(variantEntry);

    SettingEntry nudgesEntry;
    nudgesEntry.label = "Show Internal Nudges";
    nudgesEntry.description = "Display internal system messages";
    nudgesEntry.type = SettingType::Bool;
    nudgesEntry.bool_value = config.showInternalNudges;
    nudgesEntry.read_only = false;
    settings->push_back(nudgesEntry);

    SettingEntry hideErrorsEntry;
    hideErrorsEntry.label = "Hide Errors";
    hideErrorsEntry.description = "Hide error messages in chat";
    hideErrorsEntry.type = SettingType::Bool;
    hideErrorsEntry.bool_value = config.hideErrors;
    hideErrorsEntry.read_only = false;
    settings->push_back(hideErrorsEntry);

    SettingEntry permsEntry;
    permsEntry.label = "Dangerously Skip Permissions";
    permsEntry.description = "Bypass all permission prompts (unsafe)";
    permsEntry.type = SettingType::Bool;
    permsEntry.bool_value = config.dangerouslySkipPermissions;
    permsEntry.read_only = false;
    settings->push_back(permsEntry);

    if (!config.apiKeys.empty()) {
      for (const auto &[key, val] : config.apiKeys) {
        SettingEntry apiKeyEntry;
        apiKeyEntry.label = "API Key: " + key;
        apiKeyEntry.description = "Configured API key";
        apiKeyEntry.type = SettingType::Info;
        apiKeyEntry.info_value = formatApiKey(val);
        apiKeyEntry.read_only = true;
        settings->push_back(apiKeyEntry);
      }
    }

    if (!config.providerOptions.empty()) {
      for (const auto &[key, val] : config.providerOptions) {
        SettingEntry provOptEntry;
        provOptEntry.label = key;
        provOptEntry.description = "Provider option";
        provOptEntry.type = SettingType::Info;
        provOptEntry.info_value = val;
        provOptEntry.read_only = true;
        settings->push_back(provOptEntry);
      }
    }

    if (settings->empty()) {
      SettingEntry emptyEntry;
      emptyEntry.label = "No settings";
      emptyEntry.description = "";
      emptyEntry.type = SettingType::Info;
      emptyEntry.info_value = "";
      emptyEntry.read_only = true;
      settings->push_back(emptyEntry);
    }

    *selected = std::clamp(*selected, 0, static_cast<int>(settings->size() - 1));
  };

  rebuildSettings();

  auto saveConfig = [&state, settings, message]() {
    auto &h = firmius::core::Harness::instance();
    auto config = h.getConfig();
    for (const auto &setting : *settings) {
      if (setting.label == "Show Internal Nudges") {
        config.showInternalNudges = setting.bool_value;
      } else if (setting.label == "Hide Errors") {
        config.hideErrors = setting.bool_value;
      } else if (setting.label == "Dangerously Skip Permissions") {
        config.dangerouslySkipPermissions = setting.bool_value;
      }
    }
    h.updateConfig(config);
    h.saveConfig();
    *message = "Configuration saved!";
    state.deferUiMutation([&state]() {
      state.popModal();
    });
  };

  auto listContent = ftxui::Renderer([settings, selected, rowBoxes]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    rowBoxes->assign(settings->size(), ftxui::Box{});

    ftxui::Elements rows;
    for (size_t i = 0; i < settings->size(); ++i) {
      const auto &setting = (*settings)[i];
      const bool is_selected = static_cast<int>(i) == *selected;

      ftxui::Elements rowElements;

      rowElements.push_back(ftxui::text(is_selected ? "> " : "  "));

      rowElements.push_back(ftxui::text(setting.label + ":") |
                           ftxui::bold |
                           ftxui::color(is_selected ? theme.modals.highlight_fg : theme.modals.fg));

      rowElements.push_back(ftxui::text(" "));

      std::string valueStr;
      ftxui::Color valueColor = theme.base.dim;

      if (setting.type == SettingType::Bool) {
        valueStr = setting.bool_value ? "[✓] Yes" : "[ ] No";
        valueColor = setting.bool_value ? theme.modals.highlight_fg : theme.base.dim;
        if (is_selected && !setting.read_only) {
          valueStr = setting.bool_value ? "[●] Yes" : "[○] No";
          valueColor = theme.modals.highlight_fg;
        }
      } else if (setting.type == SettingType::Info) {
        valueStr = setting.info_value;
        valueColor = theme.base.dim;
      }

      rowElements.push_back(ftxui::text(valueStr) | ftxui::color(valueColor));

      if (setting.read_only) {
        rowElements.push_back(ftxui::text(" (read-only)") | ftxui::color(theme.base.dim));
      }

      auto row = ftxui::hbox(rowElements) |
                 ftxui::reflect(rowBoxes->at(i));

      if (is_selected) {
        row = row | ftxui::bgcolor(theme.modals.highlight_bg);
      }

      rows.push_back(row);

      if (is_selected && !setting.description.empty()) {
        rows.push_back(ftxui::text("  " + setting.description) |
                      ftxui::color(theme.base.dim));
      }

      rows.push_back(ftxui::text(""));
    }

    return ftxui::vbox(std::move(rows));
  });

  auto scrollable = ScrollableBox(listContent);

  auto component = ftxui::Renderer(scrollable, [scrollable, settings, selected, message]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const auto terminal = ftxui::Terminal::Size();
    const int panelWidth = std::clamp(std::max(0, terminal.dimx - 8), 60, 96);
    const int panelHeight = std::clamp(std::max(0, terminal.dimy - 6), 18, 28);
    const int listHeight = std::max(6, panelHeight - 11);

    auto body = ftxui::vbox({
        scrollable->Render() | ftxui::xflex |
            ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, listHeight),
        ftxui::text(""),
        ftxui::hbox({
            ftxui::text(" [S] ") | ftxui::bold |
                ftxui::color(theme.modals.highlight_fg),
            ftxui::text(" Save   ") | ftxui::color(theme.modals.fg),
            ftxui::filler(),
            ftxui::text(" [ESC] ") | ftxui::bold |
                ftxui::color(theme.base.dim),
            ftxui::text(" Close ") | ftxui::color(theme.modals.fg),
        }) | ftxui::center,
        ftxui::separatorLight() | ftxui::color(theme.modals.border),
        ftxui::text("↑↓ navigate, Enter toggle, wheel scroll") |
            ftxui::center | ftxui::color(theme.base.dim),
    });

    return FlatModalPanel(theme, "Configuration",
                          ModalSection(theme, std::move(body), theme.modals.bg),
                          panelWidth, panelHeight);
  });

  return ftxui::CatchEvent(component, [scrollable, settings, selected, saveConfig, rowBoxes, &state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModal();
      return true;
    }

    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k')) {
      if (*selected > 0) {
        (*selected)--;
      }
      return true;
    }

    if (event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
      if (*selected < static_cast<int>(settings->size()) - 1) {
        (*selected)++;
      }
      return true;
    }

    if (event == ftxui::Event::Return || event == ftxui::Event::Character(' ')) {
      auto &setting = (*settings)[*selected];
      if (setting.type == SettingType::Bool && !setting.read_only) {
        setting.bool_value = !setting.bool_value;
        saveConfig();
      }
      return true;
    }

    if (event == ftxui::Event::Character('s') || event == ftxui::Event::Character('S')) {
      saveConfig();
      return true;
    }

    if (event.is_mouse()) {
      const auto mouse = event.mouse();
      if (mouse.button == ftxui::Mouse::Left &&
          mouse.motion == ftxui::Mouse::Pressed) {
        for (size_t i = 0; i < rowBoxes->size(); ++i) {
          if (rowBoxes->at(i).Contain(mouse.x, mouse.y)) {
            *selected = static_cast<int>(i);
            auto &setting = (*settings)[i];
            if (setting.type == SettingType::Bool && !setting.read_only) {
              setting.bool_value = !setting.bool_value;
              saveConfig();
            }
            return true;
          }
        }
      }
      return scrollable->OnEvent(event);
    }

    return scrollable->OnEvent(event);
  });
}

} // namespace firmius::tui
