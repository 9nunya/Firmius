#include "modals/RouterModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

namespace {

enum class RouterEditMode {
  Browse,
  AddName,
  AddProvider,
  AddModel,
  AddVariant,
  EditProvider,
  EditModel,
  EditVariant,
  Rename,
  ConfirmDelete
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

} // namespace

ftxui::Component RouterModal::create(TuiState &state) {
  auto categories = std::make_shared<std::vector<std::string>>();
  auto selected = std::make_shared<int>(0);
  auto mode = std::make_shared<RouterEditMode>(RouterEditMode::Browse);
  auto message = std::make_shared<std::string>();

  auto category_name = std::make_shared<std::string>();
  auto provider_id = std::make_shared<std::string>();
  auto model_id = std::make_shared<std::string>();
  auto variant_name = std::make_shared<std::string>();

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
    h.updateConfig(cfg);
    h.saveConfig();
    *message = ok_message;
  };

  refresh();

  auto component =
      ftxui::Renderer([categories, selected, mode, message, category_name,
                       provider_id, model_id, variant_name, selectedCategory] {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        const auto cfg = firmius::core::Harness::instance().getConfig();

        ftxui::Elements rows;
        rows.push_back(ftxui::text("Model Routing Categories") | ftxui::bold |
                       ftxui::color(theme.modals.title));
        rows.push_back(ftxui::text("Priority: explicit category -> purpose route -> default route -> default model") |
                       ftxui::color(theme.base.dim));
        rows.push_back(ftxui::separatorLight() | ftxui::color(theme.modals.border));

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
            const std::string variant =
                it->second.variantName.empty() ? "" : " (" + it->second.variantName + ")";
            const std::string line = (is_selected ? "> " : "  ") + name +
                                     (is_default ? " [default]" : "") + " -> " +
                                     it->second.providerId + "/" + it->second.modelId +
                                     variant;
            rows.push_back(ftxui::text(line) |
                           ftxui::color(is_selected ? theme.modals.highlight_fg
                                                    : theme.modals.fg));
          }
        }

        rows.push_back(ftxui::separatorLight() | ftxui::color(theme.modals.border));
        if (!message->empty()) {
          rows.push_back(ftxui::text(*message) | ftxui::color(theme.base.dim));
        }

        switch (*mode) {
        case RouterEditMode::Browse:
          rows.push_back(
              ftxui::text("↑↓ select | A add | E edit model | R rename | D delete | S set default | X clear default | Esc close") |
              ftxui::color(theme.base.dim));
          break;
        case RouterEditMode::AddName:
          rows.push_back(ftxui::text("Add Category: name") | ftxui::bold);
          rows.push_back(ftxui::text(*category_name) | ftxui::underlined);
          break;
        case RouterEditMode::AddProvider:
          rows.push_back(ftxui::text("Add Category: provider id") | ftxui::bold);
          rows.push_back(ftxui::text(*provider_id) | ftxui::underlined);
          break;
        case RouterEditMode::AddModel:
          rows.push_back(ftxui::text("Add Category: model id") | ftxui::bold);
          rows.push_back(ftxui::text(*model_id) | ftxui::underlined);
          break;
        case RouterEditMode::AddVariant:
          rows.push_back(ftxui::text("Add Category: variant (optional)") | ftxui::bold);
          rows.push_back(ftxui::text(*variant_name) | ftxui::underlined);
          rows.push_back(ftxui::text("Enter on empty keeps no variant") | ftxui::color(theme.base.dim));
          break;
        case RouterEditMode::EditProvider:
          rows.push_back(ftxui::text("Edit Category: provider id") | ftxui::bold);
          rows.push_back(ftxui::text(*provider_id) | ftxui::underlined);
          break;
        case RouterEditMode::EditModel:
          rows.push_back(ftxui::text("Edit Category: model id") | ftxui::bold);
          rows.push_back(ftxui::text(*model_id) | ftxui::underlined);
          break;
        case RouterEditMode::EditVariant:
          rows.push_back(ftxui::text("Edit Category: variant (optional)") | ftxui::bold);
          rows.push_back(ftxui::text(*variant_name) | ftxui::underlined);
          break;
        case RouterEditMode::Rename:
          rows.push_back(ftxui::text("Rename Category: new name") | ftxui::bold);
          rows.push_back(ftxui::text(*category_name) | ftxui::underlined);
          break;
        case RouterEditMode::ConfirmDelete:
          rows.push_back(ftxui::text("Delete '" + selectedCategory() + "'? [y/N]") |
                         ftxui::bold | ftxui::color(theme.status_bar.error.normal.fg));
          break;
        }

        return FlatModalPanel(
            theme, "Router",
            ModalSection(theme, ftxui::vbox(std::move(rows)) | ftxui::yframe |
                                    ftxui::vscroll_indicator | ftxui::yflex,
                         theme.modals.bg),
            112, 28);
      });

  return ftxui::CatchEvent(
      component, [&, categories, selected, mode, message, category_name,
                  provider_id, model_id, variant_name, selectedCategory,
                  refresh, saveConfig](ftxui::Event event) {
        if (event == ftxui::Event::Escape) {
          if (*mode == RouterEditMode::Browse) {
            state.popModal();
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
          if (event == ftxui::Event::Character('a') ||
              event == ftxui::Event::Character('A')) {
            category_name->clear();
            provider_id->clear();
            model_id->clear();
            variant_name->clear();
            *message = "";
            *mode = RouterEditMode::AddName;
            return true;
          }
          if ((event == ftxui::Event::Character('e') ||
               event == ftxui::Event::Character('E')) &&
              !categories->empty()) {
            const auto cfg = firmius::core::Harness::instance().getConfig();
            const std::string cat = selectedCategory();
            auto it = cfg.modelRouterCategories.find(cat);
            if (it != cfg.modelRouterCategories.end()) {
              *provider_id = it->second.providerId;
              *model_id = it->second.modelId;
              *variant_name = it->second.variantName;
              *mode = RouterEditMode::EditProvider;
            }
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

        if (event == ftxui::Event::Return) {
          if (*mode == RouterEditMode::AddName) {
            if (category_name->empty()) {
              *message = "Category name cannot be empty.";
              return true;
            }
            *mode = RouterEditMode::AddProvider;
            return true;
          }
          if (*mode == RouterEditMode::AddProvider) {
            if (provider_id->empty()) {
              *message = "Provider id cannot be empty.";
              return true;
            }
            *mode = RouterEditMode::AddModel;
            return true;
          }
          if (*mode == RouterEditMode::AddModel) {
            if (model_id->empty()) {
              *message = "Model id cannot be empty.";
              return true;
            }
            *mode = RouterEditMode::AddVariant;
            return true;
          }
          if (*mode == RouterEditMode::AddVariant) {
            auto cfg = firmius::core::Harness::instance().getConfig();
            if (cfg.modelRouterCategories.count(*category_name) > 0) {
              *message = "Category already exists.";
              *mode = RouterEditMode::Browse;
              return true;
            }
            cfg.modelRouterCategories[*category_name] = {
                *provider_id, *model_id, *variant_name};
            saveConfig(cfg, "Added category '" + *category_name + "'.");
            refresh();
            *mode = RouterEditMode::Browse;
            return true;
          }
          if (*mode == RouterEditMode::EditProvider) {
            if (provider_id->empty()) {
              *message = "Provider id cannot be empty.";
              return true;
            }
            *mode = RouterEditMode::EditModel;
            return true;
          }
          if (*mode == RouterEditMode::EditModel) {
            if (model_id->empty()) {
              *message = "Model id cannot be empty.";
              return true;
            }
            *mode = RouterEditMode::EditVariant;
            return true;
          }
          if (*mode == RouterEditMode::EditVariant && !categories->empty()) {
            auto cfg = firmius::core::Harness::instance().getConfig();
            const std::string cat = selectedCategory();
            cfg.modelRouterCategories[cat] = {*provider_id, *model_id,
                                              *variant_name};
            saveConfig(cfg, "Updated category '" + cat + "'.");
            refresh();
            *mode = RouterEditMode::Browse;
            return true;
          }
          if (*mode == RouterEditMode::Rename && !categories->empty()) {
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
        }

        switch (*mode) {
        case RouterEditMode::AddName:
        case RouterEditMode::Rename:
          return applyTextEdit(event, *category_name);
        case RouterEditMode::AddProvider:
        case RouterEditMode::EditProvider:
          return applyTextEdit(event, *provider_id);
        case RouterEditMode::AddModel:
        case RouterEditMode::EditModel:
          return applyTextEdit(event, *model_id);
        case RouterEditMode::AddVariant:
        case RouterEditMode::EditVariant:
          return applyTextEdit(event, *variant_name);
        case RouterEditMode::Browse:
        case RouterEditMode::ConfirmDelete:
          break;
        }
        return false;
      });
}

} // namespace firmius::tui

