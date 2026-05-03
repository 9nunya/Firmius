#include "modals/RollingMemorySettingsModal.hpp"

#include "NotificationManager.hpp"
#include "ThemeManager.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include "utils/ModelPickerEntries.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::tui {

namespace {

enum class MemoryModalMode { Browse, PickModel };
enum class ModelTarget { Observer, Reflector, WorkingMemory };

bool applyTextEdit(ftxui::Event event, std::string &buffer) {
  if (event == ftxui::Event::Backspace) {
    if (!buffer.empty()) {
      buffer.pop_back();
    }
    return true;
  }
  if (event.is_character()) {
    buffer += event.character();
    return true;
  }
  return false;
}

std::string formatProviderList(std::vector<std::string> providerIds) {
  std::sort(providerIds.begin(), providerIds.end());
  std::string out;
  for (std::size_t i = 0; i < providerIds.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += providerIds[i];
  }
  return out;
}

void applyPreset(shared::AgentConfig::RollingMemoryConfig &cfg,
                 const std::string &preset) {
  cfg.preset = preset;
  if (preset == "aggressive") {
    cfg.targetOccupancyRatio = 0.48f;
    cfg.bufferOccupancyRatio = 0.38f;
    cfg.emergencyOccupancyRatio = 0.57f;
    cfg.reflectionOccupancyRatio = 0.24f;
    cfg.retainTailRatio = 0.14f;
    return;
  }
  if (preset == "extended") {
    cfg.targetOccupancyRatio = 0.68f;
    cfg.bufferOccupancyRatio = 0.58f;
    cfg.emergencyOccupancyRatio = 0.77f;
    cfg.reflectionOccupancyRatio = 0.40f;
    cfg.retainTailRatio = 0.22f;
    return;
  }
  if (preset == "custom") {
    return;
  }
  cfg.targetOccupancyRatio = 0.57f;
  cfg.bufferOccupancyRatio = 0.47f;
  cfg.emergencyOccupancyRatio = 0.66f;
  cfg.reflectionOccupancyRatio = 0.32f;
  cfg.retainTailRatio = 0.18f;
}

std::string modeLabel(const std::string &mode) {
  if (mode == "legacy_compaction") {
    return "legacy_compaction";
  }
  if (mode == "disabled") {
    return "disabled";
  }
  return "rolling_forever";
}

std::string formatRatio(float value) {
  std::ostringstream out;
  out << static_cast<int>(std::round(value * 100.0f)) << "%";
  return out.str();
}

std::string modelLabel(const shared::AgentConfig::RollingModelConfig &model) {
  if (!model.enabled || model.providerId.empty() || model.modelId.empty()) {
    return "(actor default)";
  }
  return model.providerId + "/" + model.modelId +
         (model.variantName.empty() ? "" : (" (" + model.variantName + ")"));
}

} // namespace

ftxui::Component RollingMemorySettingsModal::create(TuiState &state) {
  auto config =
      std::make_shared<firmius::shared::UserConfig>(
          firmius::core::Harness::instance().getConfig());
  auto selected = std::make_shared<int>(0);
  auto message = std::make_shared<std::string>();
  auto mode = std::make_shared<MemoryModalMode>(MemoryModalMode::Browse);
  auto activeTarget = std::make_shared<ModelTarget>(ModelTarget::Observer);

  auto modelEntries = std::make_shared<std::vector<ModelPickerEntry>>();
  auto filteredIndices = std::make_shared<std::vector<int>>();
  auto displayEntries = std::make_shared<std::vector<std::string>>();
  auto modelFilter = std::make_shared<std::string>();
  auto selectedModelIndex = std::make_shared<int>(0);
  auto modelsLoading = std::make_shared<bool>(true);
  auto fetchingProviders = std::make_shared<std::vector<std::string>>();
  auto rowBoxes = std::make_shared<std::vector<ftxui::Box>>();
  auto selectionCommitted = std::make_shared<std::atomic<bool>>(false);
  auto modalActive = std::make_shared<std::atomic<bool>>(true);

  auto saveConfig = [config, message]() {
    auto &h = firmius::core::Harness::instance();
    try {
      h.updateConfig(*config);
      h.saveConfig();
      *message = "Rolling memory settings saved.";
    } catch (const std::exception &ex) {
      NotificationManager::instance().notifyError("Rolling Memory", ex.what(),
                                                  false);
    }
  };

  auto refreshModels =
      [modelEntries, filteredIndices, displayEntries, modelFilter,
       selectedModelIndex, modelsLoading, fetchingProviders]() {
        auto &h = firmius::core::Harness::instance();
        *modelEntries = BuildModelPickerEntries(h.cachedModelsSnapshot(), true);
        *modelsLoading = !h.isModelsLoaded();
        *fetchingProviders = h.listProvidersFetchingModels();
        *filteredIndices =
            FilterModelPickerEntries(*modelEntries, *modelFilter);
        displayEntries->clear();
        for (int index : *filteredIndices) {
          displayEntries->push_back((*modelEntries)[index].label);
        }
        if (*selectedModelIndex >=
            static_cast<int>(filteredIndices->size())) {
          *selectedModelIndex = filteredIndices->empty()
                                    ? 0
                                    : static_cast<int>(filteredIndices->size() - 1);
        }
      };

  refreshModels();
  state.runBackgroundTask([]() { firmius::core::Harness::instance().listAllModels(); });

  int subId = firmius::core::Harness::instance().subscribe(
      [refreshModels, modalActive, &state](
          const firmius::shared::AppEvent &event) {
        if (std::holds_alternative<firmius::shared::ModelsRefreshed>(event) ||
            std::holds_alternative<firmius::shared::ProviderModelsFetchStarted>(
                event) ||
            std::holds_alternative<firmius::shared::ProviderModelsFetchFinished>(
                event) ||
            std::holds_alternative<firmius::shared::ModelDiscovered>(event)) {
          state.deferUiMutation([refreshModels, modalActive]() {
            if (!modalActive->load(std::memory_order_relaxed)) {
              return;
            }
            refreshModels();
          });
        }
      });

  auto cycleMode = [config]() {
    static const std::vector<std::string> modes = {"rolling_forever",
                                                   "legacy_compaction",
                                                   "disabled"};
    auto it = std::find(modes.begin(), modes.end(),
                        config->rollingMemory.mode);
    std::size_t index = it == modes.end() ? 0 : std::distance(modes.begin(), it);
    config->rollingMemory.mode = modes[(index + 1) % modes.size()];
    config->rollingMemory.enabled =
        config->rollingMemory.mode != "disabled";
  };

  auto cyclePreset = [config]() {
    static const std::vector<std::string> presets = {"aggressive", "balanced",
                                                     "extended", "custom"};
    auto it = std::find(presets.begin(), presets.end(),
                        config->rollingMemory.preset);
    std::size_t index =
        it == presets.end() ? 1 : std::distance(presets.begin(), it);
    applyPreset(config->rollingMemory,
                presets[(index + 1) % presets.size()]);
  };

  auto adjustNumeric = [config](int row, int direction) {
    auto &rolling = config->rollingMemory;
    switch (row) {
    case 3:
      rolling.targetOccupancyRatio =
          std::clamp(rolling.targetOccupancyRatio + direction * 0.01f, 0.15f,
                     0.95f);
      rolling.preset = "custom";
      break;
    case 4:
      rolling.bufferOccupancyRatio =
          std::clamp(rolling.bufferOccupancyRatio + direction * 0.01f, 0.10f,
                     0.90f);
      rolling.preset = "custom";
      break;
    case 5:
      rolling.emergencyOccupancyRatio =
          std::clamp(rolling.emergencyOccupancyRatio + direction * 0.01f, 0.20f,
                     0.99f);
      rolling.preset = "custom";
      break;
    case 6:
      rolling.retainTailRatio =
          std::clamp(rolling.retainTailRatio + direction * 0.01f, 0.02f, 0.60f);
      rolling.preset = "custom";
      break;
    case 7:
      rolling.minimumRetainedTailTokens = std::max<int>(
          1024, static_cast<int>(rolling.minimumRetainedTailTokens) +
                    direction * 1024);
      break;
    case 8:
      rolling.minimumChunkTokens = std::max<int>(
          1024, static_cast<int>(rolling.minimumChunkTokens) + direction * 1024);
      break;
    default:
      break;
    }
  };

  auto component = ftxui::Renderer([config, selected, message, mode,
                                    activeTarget, modelFilter,
                                    selectedModelIndex,
                                    modelsLoading, fetchingProviders,
                                    refreshModels,
                                    rowBoxes, displayEntries]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();

    ftxui::Elements rows;
    rows.push_back(ftxui::text("Rolling Memory Settings") | ftxui::bold |
                   ftxui::color(theme.modals.title));
    rows.push_back(ftxui::text(
                       "Dynamic thresholds scale with the actor model context window.") |
                   ftxui::color(theme.base.dim));
    rows.push_back(ftxui::separatorLight() |
                   ftxui::color(theme.modals.border));

    if (*mode == MemoryModalMode::Browse) {
      struct Row {
        std::string label;
        std::string value;
        std::string help;
      };
      const std::vector<Row> settingRows = {
          {"Enabled", config->rollingMemory.enabled ? "yes" : "no",
           "Master toggle for rolling memory."},
          {"Mode", modeLabel(config->rollingMemory.mode),
           "rolling_forever is the new append-only default."},
          {"Preset", config->rollingMemory.preset,
           "Presets scale thresholds by actor-model context size."},
          {"Target occupancy", formatRatio(config->rollingMemory.targetOccupancyRatio),
           "Default target is 57% to avoid context rot."},
          {"Buffer occupancy", formatRatio(config->rollingMemory.bufferOccupancyRatio),
           "Buffer before target is crossed."},
          {"Emergency occupancy", formatRatio(config->rollingMemory.emergencyOccupancyRatio),
           "Synchronous fallback only beyond this point."},
          {"Retained tail", formatRatio(config->rollingMemory.retainTailRatio),
           "Recent raw tail retained after activation."},
          {"Min retained tail tokens",
           std::to_string(config->rollingMemory.minimumRetainedTailTokens),
           "Lower bound even on small-context models."},
          {"Min chunk tokens",
           std::to_string(config->rollingMemory.minimumChunkTokens),
           "Smallest chunk size worth buffering."},
          {"Observer model", modelLabel(config->rollingMemory.observer),
           "Model used to build rolling observation chunks."},
          {"Reflector model", modelLabel(config->rollingMemory.reflector),
           "Model used to condense older observations."},
          {"Working-memory model",
           modelLabel(config->rollingMemory.workingMemoryUpdater),
           "Reserved updater model for structured memory flows."},
      };

      for (std::size_t i = 0; i < settingRows.size(); ++i) {
        const bool isSelected = static_cast<int>(i) == *selected;
        rows.push_back(
            ftxui::text((isSelected ? "> " : "  ") + settingRows[i].label +
                        ": " + settingRows[i].value) |
            ftxui::color(isSelected ? theme.modals.highlight_fg
                                    : theme.modals.fg));
        if (isSelected) {
          rows.push_back(ftxui::text("  " + settingRows[i].help) |
                         ftxui::color(theme.base.dim));
        }
      }

      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.modals.border));
      if (!message->empty()) {
        rows.push_back(ftxui::text(*message) |
                       ftxui::color(theme.base.dim));
      }
      rows.push_back(ftxui::text(
                         "↑↓ select · Enter/←/→ edit · model rows open picker · Esc close") |
                     ftxui::color(theme.base.dim));
    } else {
      const std::string targetName =
          *activeTarget == ModelTarget::Observer
              ? "Observer"
              : (*activeTarget == ModelTarget::Reflector ? "Reflector"
                                                         : "Working memory");
      rows.push_back(ftxui::text(targetName + " model") | ftxui::bold);
      rows.push_back(ftxui::hbox({
          ftxui::text("Filter: "),
          ftxui::text(*modelFilter) | ftxui::underlined,
      }));
      if (*modelsLoading) {
        const auto providerLabel = formatProviderList(*fetchingProviders);
        rows.push_back(ftxui::text(providerLabel.empty()
                                       ? "Scanning providers..."
                                       : ("Fetching: " + providerLabel)) |
                       ftxui::color(theme.base.dim));
      }
      rowBoxes->assign(displayEntries->size(), ftxui::Box{});
      ftxui::Elements pickerRows;
      for (int i = 0; i < static_cast<int>(displayEntries->size()); ++i) {
        auto row = ftxui::text((i == *selectedModelIndex ? "> " : "  ") +
                               (*displayEntries)[i]) |
                   ftxui::reflect(rowBoxes->at(i));
        if (i == *selectedModelIndex) {
          row = row | ftxui::bgcolor(theme.modals.highlight_bg) |
                ftxui::color(theme.modals.highlight_fg);
        } else {
          row = row | ftxui::color(theme.modals.fg);
        }
        pickerRows.push_back(row);
      }
      rows.push_back(ftxui::vbox(std::move(pickerRows)) |
                     ftxui::vscroll_indicator | ftxui::frame |
                     ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 10));
      rows.push_back(ftxui::text("Enter select · Esc back") |
                     ftxui::color(theme.base.dim));
    }

    return FlatModalPanel(
        theme, "Rolling Memory",
        ModalSection(theme, ftxui::vbox(std::move(rows)) | ftxui::yframe |
                                ftxui::vscroll_indicator | ftxui::yflex,
                     theme.modals.bg),
        112, 30);
  });

  return ftxui::CatchEvent(
      component,
      [config, selected, saveConfig, message, mode, activeTarget, modelFilter,
       selectedModelIndex, refreshModels, filteredIndices, rowBoxes,
       selectionCommitted, modelEntries, cycleMode, cyclePreset, adjustNumeric,
       subId, modalActive, &state](ftxui::Event event) {
        const auto closeModal = [&]() {
          modalActive->store(false, std::memory_order_relaxed);
          firmius::core::Harness::instance().unsubscribe(subId);
          state.popModal();
        };

        if (selectionCommitted->load()) {
          return true;
        }

        if (event == ftxui::Event::Escape) {
          if (*mode == MemoryModalMode::PickModel) {
            *mode = MemoryModalMode::Browse;
            modelFilter->clear();
            *selectedModelIndex = 0;
            refreshModels();
            return true;
          }
          closeModal();
          return true;
        }

        if (*mode == MemoryModalMode::PickModel) {
          if (event == ftxui::Event::ArrowUp && *selectedModelIndex > 0) {
            --(*selectedModelIndex);
            return true;
          }
          if (event == ftxui::Event::ArrowDown) {
            if (*selectedModelIndex + 1 <
                static_cast<int>(filteredIndices->size())) {
              ++(*selectedModelIndex);
            }
            return true;
          }
          if (event == ftxui::Event::Return) {
            if (filteredIndices->empty() ||
                *selectedModelIndex >=
                    static_cast<int>(filteredIndices->size())) {
              return true;
            }
            bool expected = false;
            if (!selectionCommitted->compare_exchange_strong(expected, true)) {
              return true;
            }
            const auto entry =
                (*modelEntries)[(*filteredIndices)[*selectedModelIndex]];
            const auto target = *activeTarget;
            state.deferUiMutation([config, saveConfig, subId, modalActive, &state, entry,
                                   target]() {
              auto assign = [&](shared::AgentConfig::RollingModelConfig &model) {
                model.enabled = true;
                model.providerId = entry.provider_id;
                model.modelId = entry.model_id;
                model.variantName = entry.variant_name;
              };
              switch (target) {
              case ModelTarget::Observer:
                assign(config->rollingMemory.observer);
                break;
              case ModelTarget::Reflector:
                assign(config->rollingMemory.reflector);
                break;
              case ModelTarget::WorkingMemory:
                assign(config->rollingMemory.workingMemoryUpdater);
                break;
              }
              saveConfig();
              modalActive->store(false, std::memory_order_relaxed);
              firmius::core::Harness::instance().unsubscribe(subId);
              state.popModal();
            });
            return true;
          }
          if (event.is_mouse()) {
            const auto mouse = event.mouse();
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
              for (int i = 0; i < static_cast<int>(rowBoxes->size()); ++i) {
                if (rowBoxes->at(i).Contain(mouse.x, mouse.y)) {
                  *selectedModelIndex = i;
                  if (filteredIndices->empty() ||
                      *selectedModelIndex >=
                          static_cast<int>(filteredIndices->size())) {
                    return true;
                  }
                  bool expected = false;
                  if (!selectionCommitted->compare_exchange_strong(expected,
                                                                   true)) {
                    return true;
                  }
                  const auto entry =
                      (*modelEntries)[(*filteredIndices)[*selectedModelIndex]];
                  const auto target = *activeTarget;
                  state.deferUiMutation([config, saveConfig, subId, modalActive, &state,
                                         entry, target]() {
                    auto assign =
                        [&](shared::AgentConfig::RollingModelConfig &model) {
                          model.enabled = true;
                          model.providerId = entry.provider_id;
                          model.modelId = entry.model_id;
                          model.variantName = entry.variant_name;
                        };
                    switch (target) {
                    case ModelTarget::Observer:
                      assign(config->rollingMemory.observer);
                      break;
                    case ModelTarget::Reflector:
                      assign(config->rollingMemory.reflector);
                      break;
                    case ModelTarget::WorkingMemory:
                      assign(config->rollingMemory.workingMemoryUpdater);
                      break;
                    }
                    saveConfig();
                    modalActive->store(false, std::memory_order_relaxed);
                    firmius::core::Harness::instance().unsubscribe(subId);
                    state.popModal();
                  });
                  return true;
                }
              }
            }
          }
          if (applyTextEdit(event, *modelFilter)) {
            *selectedModelIndex = 0;
            refreshModels();
            return true;
          }
          return false;
        }

        if (event == ftxui::Event::ArrowUp && *selected > 0) {
          --(*selected);
          return true;
        }
        if (event == ftxui::Event::ArrowDown && *selected < 11) {
          ++(*selected);
          return true;
        }

        auto handleEdit = [&](int direction) {
          switch (*selected) {
          case 0:
            config->rollingMemory.enabled = !config->rollingMemory.enabled;
            if (!config->rollingMemory.enabled &&
                config->rollingMemory.mode == "rolling_forever") {
              config->rollingMemory.mode = "disabled";
            } else if (config->rollingMemory.enabled &&
                       config->rollingMemory.mode == "disabled") {
              config->rollingMemory.mode = "rolling_forever";
            }
            break;
          case 1:
            cycleMode();
            break;
          case 2:
            cyclePreset();
            break;
          case 9:
            *activeTarget = ModelTarget::Observer;
            *mode = MemoryModalMode::PickModel;
            refreshModels();
            return;
          case 10:
            *activeTarget = ModelTarget::Reflector;
            *mode = MemoryModalMode::PickModel;
            refreshModels();
            return;
          case 11:
            *activeTarget = ModelTarget::WorkingMemory;
            *mode = MemoryModalMode::PickModel;
            refreshModels();
            return;
          default:
            adjustNumeric(*selected, direction);
            break;
          }
          saveConfig();
        };

        if (event == ftxui::Event::Return || event == ftxui::Event::Character(' ')) {
          handleEdit(+1);
          return true;
        }
        if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::Character('h')) {
          handleEdit(-1);
          return true;
        }
        if (event == ftxui::Event::ArrowRight || event == ftxui::Event::Character('l')) {
          handleEdit(+1);
          return true;
        }
        if (event == ftxui::Event::Character('s') || event == ftxui::Event::Character('S')) {
          saveConfig();
          return true;
        }
        return false;
      });
}

} // namespace firmius::tui
