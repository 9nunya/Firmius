#include "modals/McpModal.hpp"

#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/McpModalModel.hpp"
#include "modals/ModalLayout.hpp"

#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

namespace {

enum class McpModalMode {
  Browse,
  Edit,
  ConfirmDelete,
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

ftxui::Component McpModal::create(TuiState &state) {
  auto server_names = std::make_shared<std::vector<std::string>>();
  auto selected = std::make_shared<int>(0);
  auto mode = std::make_shared<McpModalMode>(McpModalMode::Browse);
  auto message = std::make_shared<std::string>();

  auto edit_original_name = std::make_shared<std::string>();
  auto form = std::make_shared<McpModalForm>();
  auto edit_field = std::make_shared<int>(0);

  auto refresh_names = [server_names, selected]() {
    const auto cfg = firmius::core::Harness::instance().getConfig();
    server_names->clear();
    server_names->reserve(cfg.mcpServers.size());
    for (const auto &[name, _] : cfg.mcpServers) {
      server_names->push_back(name);
    }
    std::sort(server_names->begin(), server_names->end());
    if (*selected >= static_cast<int>(server_names->size())) {
      *selected = server_names->empty() ? 0 : static_cast<int>(server_names->size() - 1);
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
    h.updateConfig(cfg);
    h.saveConfig();
    *message = ok_message;
  };

  auto begin_add = [form, edit_original_name, edit_field, mode, message]() {
    *form = McpModalForm{};
    form->name = "filesystem";
    applyMcpTemplate(*form, McpTemplateKind::StdioFilesystem);
    edit_original_name->clear();
    *edit_field = 0;
    *mode = McpModalMode::Edit;
    *message = "Adding a new MCP server. Save writes persistent config only.";
  };

  auto begin_edit = [selected_name, form, edit_original_name, edit_field, mode,
                     message]() {
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
    *edit_field = 0;
    *mode = McpModalMode::Edit;
    *message = "Editing server '" + name + "'.";
  };

  refresh_names();

  auto component = ftxui::Renderer(
      [server_names, selected, mode, message, selected_name, form, edit_field,
       edit_original_name]() {
        const auto &theme = ThemeManager::instance().getCurrentTheme();
        const auto cfg = firmius::core::Harness::instance().getConfig();

        ftxui::Elements rows;
        rows.push_back(ftxui::text("MCP servers") | ftxui::bold |
                       ftxui::color(theme.modals.title));
        rows.push_back(ftxui::text("Saved config only. Runtime-loaded MCP state is managed separately.") |
                       ftxui::color(theme.base.dim));
        rows.push_back(
            ftxui::separatorLight() | ftxui::color(theme.modals.border));

        if (*mode == McpModalMode::Browse || *mode == McpModalMode::ConfirmDelete) {
          if (server_names->empty()) {
            rows.push_back(ftxui::text("No MCP servers configured yet.") |
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
                  (it->second.enabled ? "enabled" : "disabled") + "] " +
                  "(" + (it->second.transport.empty() ? "stdio" : it->second.transport) + ")";
              rows.push_back(ftxui::text(line) |
                             ftxui::color(is_selected ? theme.modals.highlight_fg
                                                      : theme.modals.fg));
            }
          }

          rows.push_back(
              ftxui::separatorLight() | ftxui::color(theme.modals.border));
          if (*mode == McpModalMode::ConfirmDelete) {
            rows.push_back(ftxui::text("Delete '" + selected_name() + "'? [y/N]") |
                           ftxui::bold |
                           ftxui::color(theme.status_bar.error.normal.fg));
          } else {
            rows.push_back(ftxui::text(
                               "↑↓ select | A add | E edit | X enable/disable | D delete | Enter edit | Esc close") |
                           ftxui::color(theme.base.dim));
          }
        } else {
          const bool is_http = form->transport == "http";
          const std::vector<std::string> fields =
              is_http
                  ? std::vector<std::string>{"Name", "Transport", "Enabled", "URL",
                                             "Auth header", "Bearer token",
                                             "Allow insecure TLS", "CA cert path"}
                  : std::vector<std::string>{"Name", "Transport", "Enabled", "Command",
                                             "Args (comma)", "Env (K=V; K2=V2)", "CWD"};
          if (*edit_field >= static_cast<int>(fields.size())) {
            *edit_field = static_cast<int>(fields.size() - 1);
          }

          rows.push_back(ftxui::text(edit_original_name->empty()
                                         ? "Create server"
                                         : "Edit server") |
                         ftxui::bold);

          auto render_field = [&rows, &theme, edit_field](int idx,
                                                           const std::string &label,
                                                           const std::string &value) {
            const bool active = idx == *edit_field;
            const std::string prefix = active ? "> " : "  ";
            rows.push_back(
                ftxui::text(prefix + label + ": " + value) |
                ftxui::color(active ? theme.modals.highlight_fg : theme.modals.fg));
          };

          render_field(0, "Name", form->name.empty() ? "<required>" : form->name);
          render_field(1, "Transport", form->transport);
          render_field(2, "Enabled", form->enabled ? "true" : "false");

          if (is_http) {
            render_field(3, "URL", form->url);
            render_field(4, "Auth header", form->auth_header);
            render_field(5, "Bearer token",
                         form->auth_bearer_token.empty() ? "<empty>" : "***");
            render_field(6, "Allow insecure TLS",
                         form->allow_insecure_tls ? "true" : "false");
            render_field(7, "CA cert path",
                         form->ca_cert_path.empty() ? "<empty>" : form->ca_cert_path);
          } else {
            render_field(3, "Command", form->command);
            render_field(4, "Args (comma)", form->args_text);
            render_field(5, "Env (K=V;...)", form->env_text);
            render_field(6, "CWD", form->cwd);
          }

          rows.push_back(
              ftxui::separatorLight() | ftxui::color(theme.modals.border));
          rows.push_back(ftxui::text("Templates: [1] stdio filesystem  [2] stdio custom  [3] HTTP generic") |
                         ftxui::color(theme.base.dim));
          rows.push_back(ftxui::text("Arrows move field | text keys edit text | Space toggles booleans") |
                         ftxui::color(theme.base.dim));
          rows.push_back(ftxui::text("W save | Esc cancel edit") |
                         ftxui::color(theme.base.dim));
        }

        if (!message->empty()) {
          rows.push_back(
              ftxui::separatorLight() | ftxui::color(theme.modals.border));
          rows.push_back(ftxui::text(*message) | ftxui::color(theme.base.dim));
        }

        return FlatModalPanel(
            theme, "MCP",
            ModalSection(theme, ftxui::vbox(std::move(rows)) | ftxui::yframe |
                                    ftxui::vscroll_indicator | ftxui::yflex,
                         theme.modals.bg),
            112, 30);
      });

  return ftxui::CatchEvent(
      component,
      [server_names, selected, mode, message, selected_name, save_config,
       refresh_names, begin_add, begin_edit, form, edit_field, edit_original_name,
       &state](ftxui::Event event) {
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
              event == ftxui::Event::Character('E') ||
              event == ftxui::Event::Return) {
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
            save_config(cfg, "Toggled enabled state for '" + selected_name() + "'.");
            refresh_names();
            return true;
          }
          return false;
        }

        const bool is_http = form->transport == "http";
        const int max_field = is_http ? 7 : 6;

        if (event == ftxui::Event::ArrowUp) {
          if (*edit_field > 0) {
            --(*edit_field);
          }
          return true;
        }
        if (event == ftxui::Event::ArrowDown) {
          if (*edit_field < max_field) {
            ++(*edit_field);
          }
          return true;
        }

        if (event == ftxui::Event::Character('1')) {
          applyMcpTemplate(*form, McpTemplateKind::StdioFilesystem);
          *message = "Applied stdio filesystem template.";
          return true;
        }
        if (event == ftxui::Event::Character('2')) {
          applyMcpTemplate(*form, McpTemplateKind::StdioCustom);
          *message = "Applied stdio custom template.";
          return true;
        }
        if (event == ftxui::Event::Character('3')) {
          applyMcpTemplate(*form, McpTemplateKind::HttpGeneric);
          *message = "Applied HTTP generic template.";
          return true;
        }

        if (event == ftxui::Event::Character('w') ||
            event == ftxui::Event::Character('W')) {
          auto cfg = firmius::core::Harness::instance().getConfig();
          const auto result = upsertMcpServer(cfg.mcpServers, *edit_original_name, *form);
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

        if (*edit_field == 1) {
          if (event == ftxui::Event::Character('h') ||
              event == ftxui::Event::Character('H')) {
            form->transport = "http";
            if (form->auth_header.empty()) {
              form->auth_header = "Authorization";
            }
            return true;
          }
          if (event == ftxui::Event::Character('s') ||
              event == ftxui::Event::Character('S')) {
            form->transport = "stdio";
            return true;
          }
        }

        if (*edit_field == 2 || (is_http && *edit_field == 6)) {
          if (event == ftxui::Event::Character(' ') || event == ftxui::Event::Return) {
            if (*edit_field == 2) {
              form->enabled = !form->enabled;
            } else {
              form->allow_insecure_tls = !form->allow_insecure_tls;
            }
            return true;
          }
        }

        if (*edit_field == 0) {
          return applyTextEdit(event, form->name);
        }

        if (!is_http) {
          if (*edit_field == 3) {
            return applyTextEdit(event, form->command);
          }
          if (*edit_field == 4) {
            return applyTextEdit(event, form->args_text);
          }
          if (*edit_field == 5) {
            return applyTextEdit(event, form->env_text);
          }
          if (*edit_field == 6) {
            return applyTextEdit(event, form->cwd);
          }
          return false;
        }

        if (*edit_field == 3) {
          return applyTextEdit(event, form->url);
        }
        if (*edit_field == 4) {
          return applyTextEdit(event, form->auth_header);
        }
        if (*edit_field == 5) {
          return applyTextEdit(event, form->auth_bearer_token);
        }
        if (*edit_field == 7) {
          return applyTextEdit(event, form->ca_cert_path);
        }

        return false;
      });
}

} // namespace firmius::tui
