#include "modals/McpModal.hpp"

#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/McpModalModel.hpp"
#include "modals/ModalLayout.hpp"

#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

namespace {

enum class McpModalMode {
  Browse,
  Edit,
  ConfirmDelete,
};

// Ctrl+S is ASCII 0x13.
bool IsCtrlSaveEvent(const ftxui::Event &event) {
  return event.is_character() && event.character() == std::string(1, '\x13');
}

ftxui::Component OneLineInput(std::string *content, const std::string &placeholder,
                              bool password = false) {
  auto option = ftxui::InputOption::Default();
  option.content = content;
  option.placeholder = placeholder;
  option.password = password;
  return ftxui::Input(option);
}

} // namespace

ftxui::Component McpModal::create(TuiState &state) {
  auto server_names = std::make_shared<std::vector<std::string>>();
  auto selected = std::make_shared<int>(0);
  auto mode = std::make_shared<McpModalMode>(McpModalMode::Browse);
  auto message = std::make_shared<std::string>();

  auto edit_original_name = std::make_shared<std::string>();
  auto form = std::make_shared<McpModalForm>();

  // Edit mode UI backing state.
  auto transport_labels = std::make_shared<std::vector<std::string>>(
      std::vector<std::string>{"stdio", "http"});
  auto transport_selected = std::make_shared<int>(0);

  auto enabled_labels = std::make_shared<std::vector<std::string>>(
      std::vector<std::string>{"enabled", "disabled"});
  auto enabled_selected = std::make_shared<int>(0);

  auto tls_labels = std::make_shared<std::vector<std::string>>(
      std::vector<std::string>{"verify TLS", "allow insecure"});
  auto tls_selected = std::make_shared<int>(0);

  auto edit_container = std::make_shared<ftxui::Component>();
  auto edit_renderer = std::make_shared<ftxui::Component>();

  // Individual edit components (rebuilt when switching transport).
  auto name_input = std::make_shared<ftxui::Component>();
  auto transport_radio = std::make_shared<ftxui::Component>();
  auto enabled_radio = std::make_shared<ftxui::Component>();

  auto cmd_input = std::make_shared<ftxui::Component>();
  auto args_input = std::make_shared<ftxui::Component>();
  auto env_input = std::make_shared<ftxui::Component>();
  auto cwd_input = std::make_shared<ftxui::Component>();

  auto url_input = std::make_shared<ftxui::Component>();
  auto header_input = std::make_shared<ftxui::Component>();
  auto token_input = std::make_shared<ftxui::Component>();
  auto tls_radio = std::make_shared<ftxui::Component>();
  auto ca_input = std::make_shared<ftxui::Component>();

  auto refresh_names = [server_names, selected]() {
    const auto cfg = firmius::core::Harness::instance().getConfig();
    server_names->clear();
    server_names->reserve(cfg.mcpServers.size());
    for (const auto &[name, _] : cfg.mcpServers) {
      server_names->push_back(name);
    }
    std::sort(server_names->begin(), server_names->end());
    if (*selected >= static_cast<int>(server_names->size())) {
      *selected = server_names->empty()
                      ? 0
                      : static_cast<int>(server_names->size() - 1);
    }
  };

  auto selected_name = [server_names, selected]() -> std::string {
    if (server_names->empty() || *selected < 0 ||
        *selected >= static_cast<int>(server_names->size())) {
      return "";
    }
    return (*server_names)[*selected];
  };

  auto save_config = [message](const shared::UserConfig &cfg,
                               const std::string &ok_message) {
    auto &h = firmius::core::Harness::instance();
    try {
      h.updateConfig(cfg);
      h.saveConfig();
      *message = ok_message;
    } catch (const std::exception &ex) {
      NotificationManager::instance().notifyError("MCP", ex.what(), false);
    }
  };

  auto syncEditStateFromForm =
      [form, transport_selected, enabled_selected, tls_selected]() {
        *transport_selected = (form->transport == "http") ? 1 : 0;
        *enabled_selected = form->enabled ? 0 : 1;
        *tls_selected = form->allow_insecure_tls ? 1 : 0;
      };

  auto applyEditStateToForm =
      [form, transport_selected, enabled_selected, tls_selected]() {
        form->transport = (*transport_selected == 1) ? "http" : "stdio";
        form->enabled = (*enabled_selected == 0);
        form->allow_insecure_tls = (*tls_selected == 1);
        if (form->auth_header.empty()) {
          form->auth_header = "Authorization";
        }
      };

  auto rebuildEditComponents =
      [=]() {
        // Build components.
        *name_input = OneLineInput(&form->name, "required (e.g. playwright)");
        *transport_radio =
            ftxui::Radiobox(transport_labels.get(), transport_selected.get());
        *enabled_radio =
            ftxui::Radiobox(enabled_labels.get(), enabled_selected.get());

        *cmd_input = OneLineInput(&form->command, "npx / node / python / …");
        *args_input = OneLineInput(&form->args_text,
                                   "-y, package, arg1, arg2 (comma separated)");
        *env_input = OneLineInput(&form->env_text, "K=V; K2=V2 (semicolon separated)");
        *cwd_input = OneLineInput(&form->cwd, "(optional) working directory");

        *url_input = OneLineInput(&form->url, "https://…");
        *header_input = OneLineInput(&form->auth_header, "Authorization");
        *token_input =
            OneLineInput(&form->auth_bearer_token, "(optional)", true);
        *tls_radio = ftxui::Radiobox(tls_labels.get(), tls_selected.get());
        *ca_input = OneLineInput(&form->ca_cert_path, "(optional) /path/to/ca.pem");

        // Build container in a fixed order for tab navigation.
        std::vector<ftxui::Component> fields;
        fields.push_back(*name_input);
        fields.push_back(*transport_radio);
        fields.push_back(*enabled_radio);

        const bool is_http = (*transport_selected == 1);
        if (is_http) {
          fields.push_back(*url_input);
          fields.push_back(*header_input);
          fields.push_back(*token_input);
          fields.push_back(*tls_radio);
          fields.push_back(*ca_input);
        } else {
          fields.push_back(*cmd_input);
          fields.push_back(*args_input);
          fields.push_back(*env_input);
          fields.push_back(*cwd_input);
        }

        *edit_container = ftxui::Container::Vertical(std::move(fields));

        // Render helper.
        *edit_renderer = ftxui::Renderer(
            *edit_container,
            [=] {
              const auto &theme = ThemeManager::instance().getCurrentTheme();

              auto row = [&](const std::string &label,
                             const ftxui::Component &component) {
                return ftxui::hbox({
                    ftxui::text(label) | ftxui::color(theme.base.dim) |
                        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 18),
                    component->Render() | ftxui::xflex,
                });
              };

              ftxui::Elements rows;
              rows.push_back(
                  ftxui::text(edit_original_name->empty() ? "Create MCP server"
                                                         : "Edit MCP server") |
                  ftxui::bold | ftxui::color(theme.modals.title));
              rows.push_back(ftxui::separatorLight() |
                             ftxui::color(theme.modals.border));

              rows.push_back(row("Name", *name_input));
              rows.push_back(row("Transport", *transport_radio));
              rows.push_back(row("Enabled", *enabled_radio));

              const bool is_http = (*transport_selected == 1);
              if (is_http) {
                rows.push_back(row("URL", *url_input));
                rows.push_back(row("Auth header", *header_input));
                rows.push_back(row("Bearer token", *token_input));
                rows.push_back(row("TLS", *tls_radio));
                rows.push_back(row("CA cert", *ca_input));
              } else {
                rows.push_back(row("Command", *cmd_input));
                rows.push_back(row("Args", *args_input));
                rows.push_back(row("Env", *env_input));
                rows.push_back(row("CWD", *cwd_input));
              }

              return ftxui::vbox(std::move(rows));
            });
      };

  auto begin_add = [=]() {
    *form = McpModalForm{};
    form->name = "filesystem";
    applyMcpTemplate(*form, McpTemplateKind::StdioFilesystem);
    edit_original_name->clear();
    *mode = McpModalMode::Edit;
    *message = "Adding a new MCP server (saved to config only).";

    syncEditStateFromForm();
    rebuildEditComponents();
    if (*name_input) {
      (*name_input)->TakeFocus();
    }
  };

  auto begin_edit = [=]() {
    const auto name = selected_name();
    if (name.empty()) {
      return;
    }

    const auto cfg = firmius::core::Harness::instance().getConfig();
    auto it = cfg.mcpServers.find(name);
    if (it == cfg.mcpServers.end()) {
      return;
    }

    *form = mcpFormFromServer(name, it->second);
    *edit_original_name = name;
    *mode = McpModalMode::Edit;
    *message = "Editing server '" + name + "'.";

    syncEditStateFromForm();
    rebuildEditComponents();
    if (*name_input) {
      (*name_input)->TakeFocus();
    }
  };

  refresh_names();
  syncEditStateFromForm();
  rebuildEditComponents();

  auto renderer = ftxui::Renderer([=] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const auto cfg = firmius::core::Harness::instance().getConfig();

    ftxui::Elements rows;
    rows.push_back(ftxui::text("MCP servers") | ftxui::bold |
                   ftxui::color(theme.modals.title));
    rows.push_back(
        ftxui::text("Edits here update your saved config (~/.firmius/config.json). (Tip: for Playwright, set Name=playwright, Command=npx, Args=-y, @playwright/mcp@latest)") |
        ftxui::color(theme.base.dim));
    rows.push_back(ftxui::separatorLight() | ftxui::color(theme.modals.border));

    if (*mode == McpModalMode::Browse || *mode == McpModalMode::ConfirmDelete) {
      if (server_names->empty()) {
        rows.push_back(ftxui::text("No MCP servers configured.") |
                       ftxui::color(theme.base.dim));
      } else {
        for (size_t i = 0; i < server_names->size(); ++i) {
          const std::string &name = (*server_names)[i];
          auto it = cfg.mcpServers.find(name);
          if (it == cfg.mcpServers.end()) {
            continue;
          }

          const bool is_selected = static_cast<int>(i) == *selected;
          const std::string line =
              (is_selected ? "> " : "  ") + name + " [" +
              (it->second.enabled ? "enabled" : "disabled") + "] (" +
              (it->second.transport.empty() ? "stdio" : it->second.transport) +
              ")";
          rows.push_back(
              ftxui::text(line) |
              ftxui::color(is_selected ? theme.modals.highlight_fg
                                       : theme.modals.fg));
        }
      }

      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.modals.border));
      if (*mode == McpModalMode::ConfirmDelete) {
        rows.push_back(ftxui::text("Delete '" + selected_name() + "'? [y/N]") |
                       ftxui::bold |
                       ftxui::color(theme.status_bar.error.normal.fg));
      } else {
        rows.push_back(
            ftxui::text(
                "↑↓ select | A add | Enter/E edit | X enable/disable | D delete | Esc close") |
            ftxui::color(theme.base.dim));
      }
    } else {
      rows.push_back((*edit_renderer)->Render() | ftxui::color(theme.modals.fg));
      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.modals.border));
      rows.push_back(
          ftxui::text(
              "Tab/Shift+Tab move | Ctrl+S save | Esc cancel | F1 stdio filesystem | F2 stdio custom | F3 HTTP | (no letter hotkeys while typing)") |
          ftxui::color(theme.base.dim));
    }

    if (!message->empty()) {
      rows.push_back(ftxui::separatorLight() | ftxui::color(theme.modals.border));
      rows.push_back(ftxui::text(*message) | ftxui::color(theme.base.dim));
    }

    return FlatModalPanel(
        theme, "MCP",
        ModalSection(theme,
                     ftxui::vbox(std::move(rows)) | ftxui::yframe |
                         ftxui::vscroll_indicator | ftxui::yflex,
                     theme.modals.bg),
        112, 30);
  });

  return ftxui::CatchEvent(renderer, [=, &state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      if (*mode == McpModalMode::Browse) {
        state.popModal();
      } else {
        *mode = McpModalMode::Browse;
        *message = "Edit cancelled.";
      }
      return true;
    }

    if (*mode == McpModalMode::ConfirmDelete) {
      if (event == ftxui::Event::Character('y') ||
          event == ftxui::Event::Character('Y')) {
        const std::string name = selected_name();
        if (!name.empty()) {
          auto cfg = firmius::core::Harness::instance().getConfig();
          deleteMcpServer(cfg.mcpServers, name);
          save_config(cfg, "Deleted server '" + name + "'.");
          refresh_names();
        }
      }
      *mode = McpModalMode::Browse;
      return true;
    }

    if (*mode == McpModalMode::Browse) {
      if (event == ftxui::Event::ArrowUp) {
        if (*selected > 0) {
          --(*selected);
        }
        return true;
      }
      if (event == ftxui::Event::ArrowDown) {
        if (*selected + 1 < static_cast<int>(server_names->size())) {
          ++(*selected);
        }
        return true;
      }
      if (event == ftxui::Event::Character('a') ||
          event == ftxui::Event::Character('A')) {
        begin_add();
        return true;
      }
      if (event == ftxui::Event::Character('e') ||
          event == ftxui::Event::Character('E') || event == ftxui::Event::Return) {
        begin_edit();
        return true;
      }
      if ((event == ftxui::Event::Character('d') ||
           event == ftxui::Event::Character('D')) &&
          !selected_name().empty()) {
        *mode = McpModalMode::ConfirmDelete;
        return true;
      }
      if ((event == ftxui::Event::Character('x') ||
           event == ftxui::Event::Character('X')) &&
          !selected_name().empty()) {
        auto cfg = firmius::core::Harness::instance().getConfig();
        toggleMcpServerEnabled(cfg.mcpServers, selected_name());
        save_config(cfg,
                    "Toggled enabled state for '" + selected_name() + "'.");
        refresh_names();
        return true;
      }
      return false;
    }

    // Edit mode.
    if (event == ftxui::Event::F1) {
      applyMcpTemplate(*form, McpTemplateKind::StdioFilesystem);
      syncEditStateFromForm();
      rebuildEditComponents();
      *message = "Applied stdio filesystem template.";
      if (*name_input) {
        (*name_input)->TakeFocus();
      }
      return true;
    }
    if (event == ftxui::Event::F2) {
      applyMcpTemplate(*form, McpTemplateKind::StdioCustom);
      syncEditStateFromForm();
      rebuildEditComponents();
      *message = "Applied stdio custom template.";
      if (*name_input) {
        (*name_input)->TakeFocus();
      }
      return true;
    }
    if (event == ftxui::Event::F3) {
      applyMcpTemplate(*form, McpTemplateKind::HttpGeneric);
      syncEditStateFromForm();
      rebuildEditComponents();
      *message = "Applied HTTP generic template.";
      if (*name_input) {
        (*name_input)->TakeFocus();
      }
      return true;
    }

    if (IsCtrlSaveEvent(event)) {
      applyEditStateToForm();

      auto cfg = firmius::core::Harness::instance().getConfig();
      const auto result =
          upsertMcpServer(cfg.mcpServers, *edit_original_name, *form);
      if (result == McpSaveResult::EmptyName) {
        *message = "Server name is required.";
        return true;
      }
      if (result == McpSaveResult::RenameCollision) {
        *message = "A server with that name already exists.";
        return true;
      }

      save_config(cfg, "Saved server '" + trimMcpText(form->name) + "'.");
      refresh_names();
      const auto it = std::find(server_names->begin(), server_names->end(),
                                trimMcpText(form->name));
      if (it != server_names->end()) {
        *selected = static_cast<int>(std::distance(server_names->begin(), it));
      }
      *mode = McpModalMode::Browse;
      return true;
    }

    // Let focused widget handle keypresses.
    if (*edit_container && (*edit_container)->OnEvent(event)) {
      const std::string desired_transport =
          (*transport_selected == 1) ? "http" : "stdio";
      if (form->transport != desired_transport) {
        form->transport = desired_transport;
        rebuildEditComponents();
        // Keep focus roughly stable after rebuilding.
        if (*edit_container) {
          (*edit_container)->TakeFocus();
        }
      }
      return true;
    }

    return false;
  });
}

} // namespace firmius::tui
