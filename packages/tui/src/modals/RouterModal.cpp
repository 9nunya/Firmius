#include "modals/RouterModal.hpp"
#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include "utils/ModelPickerEntries.hpp"
#include <algorithm>
#include <atomic>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

namespace {

enum class RouterEditMode {
  Browse,
  AddName,
  AddPickModel,
  Detail,
  DetailPickModel,
  Rename,
  ConfirmDelete,
  ConfirmDeleteModel
};

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
  if (providerIds.empty()) {
    return "";
  }

  std::string joined;
  for (size_t i = 0; i < providerIds.size(); ++i) {
    if (i > 0) {
      joined += ", ";
    }
    joined += providerIds[i];
  }
  return joined;
}

} // namespace

ftxui::Component RouterModal::create(TuiState &state) {
  auto categories = std::make_shared<std::vector<std::string>>();
  auto selected = std::make_shared<int>(0);
  auto mode = std::make_shared<RouterEditMode>(RouterEditMode::Browse);
  auto message = std::make_shared<std::string>();

  auto category_name = std::make_shared<std::string>();
  auto model_filter = std::make_shared<std::string>();
  auto model_entries = std::make_shared<std::vector<ModelPickerEntry>>();
  auto filtered_model_indices = std::make_shared<std::vector<int>>();
  auto selected_model_index = std::make_shared<int>(0);
  auto display_model_entries = std::make_shared<std::vector<std::string>>();
  auto models_loading = std::make_shared<bool>(true);
  auto fetching_providers = std::make_shared<std::vector<std::string>>();
  auto model_menu =
      ftxui::Menu(display_model_entries.get(), selected_model_index.get());

  auto selected_detail_index = std::make_shared<int>(0);

  auto refresh = [categories, selected]() {
    const auto cfg = firmius::core::Harness::instance().getConfig();
    categories->clear();
    categories->reserve(cfg.modelRouterCategories.size());
    for (const auto &[name, _] : cfg.modelRouterCategories) {
      categories->push_back(name);
    }
    std::sort(categories->begin(), categories->end());
    if (*selected >= static_cast<int>(categories->size())) {
      *selected =
          categories->empty() ? 0 : static_cast<int>(categories->size() - 1);
    }
  };

  auto rebuildModelFilter = [model_entries, model_filter,
                             filtered_model_indices, display_model_entries,
                             selected_model_index]() {
    *filtered_model_indices =
        FilterModelPickerEntries(*model_entries, *model_filter);

    display_model_entries->clear();
    for (int index : *filtered_model_indices) {
      display_model_entries->push_back((*model_entries)[index].label);
    }

    if (*selected_model_index >=
        static_cast<int>(filtered_model_indices->size())) {
      *selected_model_index =
          filtered_model_indices->empty()
              ? 0
              : static_cast<int>(filtered_model_indices->size() - 1);
    }
  };

  auto refreshModelEntries =
      [model_entries, rebuildModelFilter, models_loading, fetching_providers]() {
    auto &h = firmius::core::Harness::instance();
    auto models = h.cachedModelsSnapshot();
    *model_entries = BuildModelPickerEntries(models, true);
    *models_loading = !h.isModelsLoaded();
    *fetching_providers = h.listProvidersFetchingModels();
    rebuildModelFilter();
  };

  auto selectedCategory = [categories, selected]() -> std::string {
    if (categories->empty() || *selected < 0 ||
        *selected >= static_cast<int>(categories->size())) {
      return "";
    }
    return (*categories)[*selected];
  };

  auto saveConfig = [message](const firmius::shared::UserConfig &cfg,
                              const std::string &ok_message) {
    auto &h = firmius::core::Harness::instance();
    try {
      h.updateConfig(cfg);
      h.saveConfig();
      *message = ok_message;
    } catch (const std::exception &ex) {
      NotificationManager::instance().notifyError("Router", ex.what(), false);
    }
  };

  refresh();
  refreshModelEntries();
  state.runBackgroundTask([]() { firmius::core::Harness::instance().listAllModels(); });

  auto modalActive = std::make_shared<std::atomic<bool>>(true);
  int subId = firmius::core::Harness::instance().subscribe(
      [refreshModelEntries, modalActive, &state](
          const firmius::shared::AppEvent &event) {
        if (std::holds_alternative<firmius::shared::ModelsRefreshed>(event) ||
            std::holds_alternative<firmius::shared::ProviderModelsFetchStarted>(
                event) ||
            std::holds_alternative<firmius::shared::ProviderModelsFetchFinished>(
                event) ||
            std::holds_alternative<firmius::shared::ModelDiscovered>(event)) {
          state.deferUiMutation([refreshModelEntries, modalActive]() {
            if (!modalActive->load(std::memory_order_relaxed)) {
              return;
            }
            refreshModelEntries();
          });
        }
      });

  auto component = ftxui::Renderer(
      [categories, selected, mode, message, category_name, model_filter,
       selectedCategory, model_menu, rebuildModelFilter, refreshModelEntries,
       models_loading, fetching_providers,
       selected_detail_index]() {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        const auto cfg = firmius::core::Harness::instance().getConfig();
        rebuildModelFilter();
        const std::string fetchingLabel =
            formatProviderList(*fetching_providers);

        ftxui::Elements rows;
        rows.push_back(ftxui::text("Model Routing Categories") | ftxui::bold |
                       ftxui::color(theme.modals.title));
        rows.push_back(ftxui::text("Priority: explicit category -> purpose route -> default route -> default model") |
                       ftxui::color(theme.base.dim));
        rows.push_back(
            ftxui::separatorLight() | ftxui::color(theme.modals.border));

        if (*mode == RouterEditMode::Browse || *mode == RouterEditMode::AddName ||
            *mode == RouterEditMode::ConfirmDelete ||
            *mode == RouterEditMode::Rename) {
          if (categories->empty()) {
            rows.push_back(ftxui::text("No categories configured yet.") |
                           ftxui::color(theme.base.dim));
          } else {
            for (size_t i = 0; i < categories->size(); ++i) {
              const auto &name = (*categories)[i];
              auto it = cfg.modelRouterCategories.find(name);
              if (it == cfg.modelRouterCategories.end()) {
                continue;
              }
              const bool is_selected = static_cast<int>(i) == *selected;
              const bool is_default = cfg.defaultRouteCategory == name;

              std::string model_info;
              if (it->second.models.empty()) {
                model_info = "no models";
              } else if (it->second.models.size() == 1) {
                const auto &m = it->second.models.front();
                model_info = m.providerId + "/" + m.modelId;
                if (!m.variantName.empty()) {
                  model_info += " (" + m.variantName + ")";
                }
              } else {
                model_info =
                    std::to_string(it->second.models.size()) + " models";
              }

              const std::string line = (is_selected ? "> " : "  ") + name +
                                       (is_default ? " [default]" : "") +
                                       " -> " + model_info;
              rows.push_back(ftxui::text(line) |
                             ftxui::color(is_selected
                                              ? theme.modals.highlight_fg
                                              : theme.modals.fg));
            }
          }
        } else if (*mode == RouterEditMode::Detail ||
                   *mode == RouterEditMode::ConfirmDeleteModel) {
          const std::string name = selectedCategory();
          auto it = cfg.modelRouterCategories.find(name);
          if (it != cfg.modelRouterCategories.end()) {
            rows.push_back(ftxui::text("Category: " + name) | ftxui::bold |
                           ftxui::color(theme.modals.fg));
            rows.push_back(ftxui::text("Models in this category:") |
                           ftxui::color(theme.base.dim));

            const auto &models = it->second.models;
            if (models.empty()) {
              rows.push_back(ftxui::text("  No models added yet.") |
                             ftxui::color(theme.base.dim));
            } else {
              for (size_t i = 0; i < models.size(); ++i) {
                const auto &m = models[i];
                const bool is_selected = static_cast<int>(i) == *selected_detail_index;
                std::string line = (is_selected ? "> " : "  ");
                line += m.providerId + "/" + m.modelId;
                if (!m.variantName.empty()) {
                  line += " (" + m.variantName + ")";
                }
                rows.push_back(ftxui::text(line) |
                               ftxui::color(is_selected
                                                ? theme.modals.highlight_fg
                                                : theme.modals.fg));
              }
            }
          }
        }

        rows.push_back(
            ftxui::separatorLight() | ftxui::color(theme.modals.border));
        if (!message->empty()) {
          rows.push_back(ftxui::text(*message) | ftxui::color(theme.base.dim));
        }

        switch (*mode) {
        case RouterEditMode::Browse:
          rows.push_back(
              ftxui::text("↑↓ select | Enter view detail | A add category | R rename | D delete | S set default | X clear default | Esc close") |
              ftxui::color(theme.base.dim));
          break;
        case RouterEditMode::Detail:
          rows.push_back(
              ftxui::text("↑↓ select model | A add model | D remove model | Esc back") |
              ftxui::color(theme.base.dim));
          break;
        case RouterEditMode::AddName:
          rows.push_back(ftxui::text("Add Category: name") | ftxui::bold);
          rows.push_back(ftxui::text(*category_name) | ftxui::underlined);
          rows.push_back(ftxui::text("Enter to continue to model picker") |
                         ftxui::color(theme.base.dim));
          break;
        case RouterEditMode::AddPickModel:
        case RouterEditMode::DetailPickModel:
          rows.push_back(ftxui::text("Pick model + variant") | ftxui::bold);
          rows.push_back(ftxui::hbox({
              ftxui::text("Filter: ") | ftxui::color(theme.modals.fg),
              ftxui::text(*model_filter) | ftxui::underlined |
                  ftxui::color(theme.modals.fg),
          }));
          if (*models_loading) {
            const std::string loadingText =
                fetchingLabel.empty()
                    ? "Scanning providers... results populate as they arrive."
                    : "Fetching: " + fetchingLabel;
            rows.push_back(ftxui::text(loadingText) |
                           ftxui::color(theme.base.dim));
          }
          rows.push_back(model_menu->Render() | ftxui::vscroll_indicator |
                         ftxui::frame |
                         ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 10));
          rows.push_back(ftxui::text("Enter to add selected model/variant") |
                         ftxui::color(theme.base.dim));
          break;
        case RouterEditMode::Rename:
          rows.push_back(ftxui::text("Rename Category: new name") |
                         ftxui::bold);
          rows.push_back(ftxui::text(*category_name) | ftxui::underlined);
          break;
        case RouterEditMode::ConfirmDelete:
          rows.push_back(ftxui::text("Delete category '" + selectedCategory() +
                                     "'? [y/N]") |
                         ftxui::bold |
                         ftxui::color(theme.status_bar.error.normal.fg));
          break;
        case RouterEditMode::ConfirmDeleteModel: {
          const std::string name = selectedCategory();
          auto it = cfg.modelRouterCategories.find(name);
          std::string modelName = "model";
          if (it != cfg.modelRouterCategories.end() &&
              *selected_detail_index >= 0 &&
              *selected_detail_index < static_cast<int>(it->second.models.size())) {
            const auto &m = it->second.models[*selected_detail_index];
            modelName = m.providerId + "/" + m.modelId;
          }
          rows.push_back(ftxui::text("Remove '" + modelName + "' from '" +
                                     name + "'? [y/N]") |
                         ftxui::bold |
                         ftxui::color(theme.status_bar.error.normal.fg));
          break;
        }
        }

        return FlatModalPanel(
            theme, "Router",
            ModalSection(theme, ftxui::vbox(std::move(rows)) | ftxui::yframe |
                                    ftxui::vscroll_indicator | ftxui::yflex,
                         theme.modals.bg),
            112, 30);
      });

  return ftxui::CatchEvent(
      component,
      [categories, selected, mode, message, category_name, model_filter,
       model_entries, filtered_model_indices, selected_model_index, model_menu,
       selectedCategory, refresh, saveConfig, rebuildModelFilter,
       refreshModelEntries, subId, modalActive, &state,
       selected_detail_index](ftxui::Event event) {
        const auto closeModal = [&]() {
          modalActive->store(false, std::memory_order_relaxed);
          firmius::core::Harness::instance().unsubscribe(subId);
          state.popModal();
        };

        if (event == ftxui::Event::Escape) {
          if (*mode == RouterEditMode::Browse) {
            closeModal();
            return true;
          }
          *mode = RouterEditMode::Browse;
          return true;
        }

        if (*mode == RouterEditMode::Browse) {
          if (event == ftxui::Event::ArrowUp) {
            if (*selected > 0) {
              --(*selected);
            }
            return true;
          }
          if (event == ftxui::Event::ArrowDown) {
            if (*selected + 1 < static_cast<int>(categories->size())) {
              ++(*selected);
            }
            return true;
          }
          if (event == ftxui::Event::Return) {
            if (!categories->empty()) {
              *selected_detail_index = 0;
              *mode = RouterEditMode::Detail;
            }
            return true;
          }
          if (event == ftxui::Event::Character('a') ||
              event == ftxui::Event::Character('A')) {
            category_name->clear();
            model_filter->clear();
            *selected_model_index = 0;
            *message = "";
            refreshModelEntries();
            *mode = RouterEditMode::AddName;
            return true;
          }
          if ((event == ftxui::Event::Character('r') ||
               event == ftxui::Event::Character('R')) &&
              !categories->empty()) {
            *category_name = selectedCategory();
            *mode = RouterEditMode::Rename;
            return true;
          }
          if ((event == ftxui::Event::Character('d') ||
               event == ftxui::Event::Character('D')) &&
              !categories->empty()) {
            *mode = RouterEditMode::ConfirmDelete;
            return true;
          }
          if ((event == ftxui::Event::Character('s') ||
               event == ftxui::Event::Character('S')) &&
              !categories->empty()) {
            auto cfg = firmius::core::Harness::instance().getConfig();
            cfg.defaultRouteCategory = selectedCategory();
            saveConfig(cfg, "Default route category set to '" +
                               cfg.defaultRouteCategory + "'.");
            return true;
          }
          if (event == ftxui::Event::Character('x') ||
              event == ftxui::Event::Character('X')) {
            auto cfg = firmius::core::Harness::instance().getConfig();
            cfg.defaultRouteCategory.clear();
            saveConfig(cfg, "Cleared default route category.");
            return true;
          }
          return false;
        }

        if (*mode == RouterEditMode::Detail) {
          const std::string name = selectedCategory();
          auto cfg = firmius::core::Harness::instance().getConfig();
          auto it = cfg.modelRouterCategories.find(name);
          if (it == cfg.modelRouterCategories.end()) {
            *mode = RouterEditMode::Browse;
            return true;
          }

          if (event == ftxui::Event::ArrowUp) {
            if (*selected_detail_index > 0) {
              --(*selected_detail_index);
            }
            return true;
          }
          if (event == ftxui::Event::ArrowDown) {
            if (*selected_detail_index + 1 < static_cast<int>(it->second.models.size())) {
              ++(*selected_detail_index);
            }
            return true;
          }
          if (event == ftxui::Event::Character('a') ||
              event == ftxui::Event::Character('A')) {
            model_filter->clear();
            *selected_model_index = 0;
            *message = "";
            refreshModelEntries();
            *mode = RouterEditMode::DetailPickModel;
            return true;
          }
          if (event == ftxui::Event::Character('d') ||
              event == ftxui::Event::Character('D')) {
            if (!it->second.models.empty()) {
              *mode = RouterEditMode::ConfirmDeleteModel;
            }
            return true;
          }
          return false;
        }

        if (*mode == RouterEditMode::ConfirmDeleteModel) {
          if (event == ftxui::Event::Character('y') ||
              event == ftxui::Event::Character('Y')) {
            const std::string name = selectedCategory();
            auto cfg = firmius::core::Harness::instance().getConfig();
            auto it = cfg.modelRouterCategories.find(name);
            if (it != cfg.modelRouterCategories.end()) {
              if (*selected_detail_index >= 0 &&
                  *selected_detail_index < static_cast<int>(it->second.models.size())) {
                it->second.models.erase(it->second.models.begin() + *selected_detail_index);
                if (*selected_detail_index >= static_cast<int>(it->second.models.size())) {
                  *selected_detail_index = std::max(0, static_cast<int>(it->second.models.size()) - 1);
                }
                saveConfig(cfg, "Removed model from '" + name + "'.");
              }
            }
          }
          *mode = RouterEditMode::Detail;
          return true;
        }

        if (*mode == RouterEditMode::ConfirmDelete) {
          if (event == ftxui::Event::Character('y') ||
              event == ftxui::Event::Character('Y')) {
            auto cfg = firmius::core::Harness::instance().getConfig();
            const std::string cat = selectedCategory();
            cfg.modelRouterCategories.erase(cat);
            if (cfg.defaultRouteCategory == cat) {
              cfg.defaultRouteCategory.clear();
            }
            for (auto it = cfg.purposeRoutes.begin();
                 it != cfg.purposeRoutes.end();) {
              if (it->second != cat) {
                ++it;
                continue;
              }
              if (!cfg.defaultRouteCategory.empty()) {
                it->second = cfg.defaultRouteCategory;
                ++it;
              } else {
                it = cfg.purposeRoutes.erase(it);
              }
            }
            saveConfig(cfg, "Deleted category '" + cat + "'.");
            refresh();
          }
          *mode = RouterEditMode::Browse;
          return true;
        }

        if (*mode == RouterEditMode::AddName) {
          if (event == ftxui::Event::Return) {
            if (category_name->empty()) {
              *message = "Category name cannot be empty.";
              return true;
            }
            auto cfg = firmius::core::Harness::instance().getConfig();
            if (cfg.modelRouterCategories.count(*category_name) > 0) {
              *message = "Category already exists.";
              *mode = RouterEditMode::Browse;
              return true;
            }
            *mode = RouterEditMode::AddPickModel;
            return true;
          }
          return applyTextEdit(event, *category_name);
        }

        if (*mode == RouterEditMode::Rename && !categories->empty()) {
          if (event == ftxui::Event::Return) {
            const std::string old_name = selectedCategory();
            if (category_name->empty()) {
              *message = "New category name cannot be empty.";
              return true;
            }
            auto cfg = firmius::core::Harness::instance().getConfig();
            if (old_name != *category_name &&
                cfg.modelRouterCategories.count(*category_name) > 0) {
              *message = "Category name already exists.";
              return true;
            }
            auto it = cfg.modelRouterCategories.find(old_name);
            if (it != cfg.modelRouterCategories.end()) {
              auto route = it->second;
              cfg.modelRouterCategories.erase(it);
              cfg.modelRouterCategories[*category_name] = route;
              if (cfg.defaultRouteCategory == old_name) {
                cfg.defaultRouteCategory = *category_name;
              }
              for (auto &[purpose, cat] : cfg.purposeRoutes) {
                (void)purpose;
                if (cat == old_name) {
                  cat = *category_name;
                }
              }
              saveConfig(cfg, "Renamed category '" + old_name + "' -> '" +
                                  *category_name + "'.");
              refresh();
            }
            *mode = RouterEditMode::Browse;
            return true;
          }
          return applyTextEdit(event, *category_name);
        }

        if (*mode == RouterEditMode::DetailPickModel) {
          if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown ||
              event.is_mouse()) {
            return model_menu->OnEvent(event);
          }
          if (event == ftxui::Event::Backspace) {
            if (!model_filter->empty()) {
              model_filter->pop_back();
              *selected_model_index = 0;
              rebuildModelFilter();
            }
            return true;
          }
          if (event.is_character()) {
            *model_filter += event.character();
            *selected_model_index = 0;
            rebuildModelFilter();
            return true;
          }
          if (event != ftxui::Event::Return) {
            return false;
          }

          if (filtered_model_indices->empty() ||
              *selected_model_index >=
                  static_cast<int>(filtered_model_indices->size())) {
            *message = "Select a model first.";
            return true;
          }

          const int modelIndex = (*filtered_model_indices)[*selected_model_index];
          const auto &entry = (*model_entries)[modelIndex];

          auto cfg = firmius::core::Harness::instance().getConfig();
          const std::string catName = selectedCategory();
          auto it = cfg.modelRouterCategories.find(catName);
          if (it != cfg.modelRouterCategories.end()) {
            it->second.models.push_back(
                {entry.provider_id, entry.model_id, entry.variant_name});
            saveConfig(cfg, "Added model to category '" + catName + "'.");
          }
          refresh();
          *mode = RouterEditMode::Detail;
          return true;
        }

        if (*mode == RouterEditMode::AddPickModel) {
          if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown ||
              event.is_mouse()) {
            return model_menu->OnEvent(event);
          }
          if (event == ftxui::Event::Backspace) {
            if (!model_filter->empty()) {
              model_filter->pop_back();
              *selected_model_index = 0;
              rebuildModelFilter();
            }
            return true;
          }
          if (event.is_character()) {
            *model_filter += event.character();
            *selected_model_index = 0;
            rebuildModelFilter();
            return true;
          }
          if (event != ftxui::Event::Return) {
            return false;
          }

          if (filtered_model_indices->empty() ||
              *selected_model_index >=
                  static_cast<int>(filtered_model_indices->size())) {
            *message = "Select a model first.";
            return true;
          }

          const int modelIndex = (*filtered_model_indices)[*selected_model_index];
          const auto &entry = (*model_entries)[modelIndex];

          auto cfg = firmius::core::Harness::instance().getConfig();
          shared::ModelRouteCategory cat;
          cat.models.push_back(
              {entry.provider_id, entry.model_id, entry.variant_name});
          cfg.modelRouterCategories[*category_name] = cat;
          saveConfig(cfg, "Added category '" + *category_name + "'.");

          refresh();
          *mode = RouterEditMode::Browse;
          return true;
        }

        return false;
      });
}

} // namespace firmius::tui
