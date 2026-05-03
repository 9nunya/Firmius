#include "modals/PermissionPromptModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "modals/ModalLayout.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>

namespace firmius::tui {

PermissionPromptModal::PermissionPromptModal(
    firmius::shared::PermissionEscalationRequest request,
    std::function<void(firmius::shared::PermissionResponse)> onResult)
    : request_(std::move(request)), onResult_(std::move(onResult)) {}

std::vector<std::string> PermissionPromptModal::getEditOptions() const {
  if (request_.requestType == firmius::shared::PermissionRequestType::Read) {
    return {
        "Allow once",
        "Allow this exact location for this session",
        "Allow this exact location globally",
        "Allow every read this session",
        "Deny this request",
    };
  }

  return {
      "Allow once",
      "Allow this exact location for this session",
      "Allow this exact location globally",
      "Deny this request",
  };
}

std::vector<std::string> PermissionPromptModal::getCommandOptions() const {
  const std::string command =
      request_.command.empty() ? "this command" : request_.command;
  const std::string tool =
      request_.toolName.empty() ? "unknown tool" : request_.toolName;
  return {
      "Run once: " + command,
      "Allow command this session: " + command,
      "Allow tool this session: " + tool,
      "Allow command globally: " + command,
      "Deny this command",
  };
}

std::vector<firmius::shared::PermissionResponse>
PermissionPromptModal::getEditResponses() const {
  using firmius::shared::PermissionResponse;
  if (request_.requestType == firmius::shared::PermissionRequestType::Read) {
    return {
        PermissionResponse::AllowOnce,
        PermissionResponse::AllowPathSession,
        PermissionResponse::AllowPathGlobal,
        PermissionResponse::AllowAllReadsSession,
        PermissionResponse::Deny,
    };
  }
  return {
      PermissionResponse::AllowOnce,
      PermissionResponse::AllowPathSession,
      PermissionResponse::AllowPathGlobal,
      PermissionResponse::Deny,
  };
}

std::vector<firmius::shared::PermissionResponse>
PermissionPromptModal::getCommandResponses() const {
  using firmius::shared::PermissionResponse;
  return {
      PermissionResponse::AllowOnce,
      PermissionResponse::AllowCommandSession,
      PermissionResponse::AllowCommandToolSession,
      PermissionResponse::AllowCommandGlobal,
      PermissionResponse::Deny,
  };
}

std::string PermissionPromptModal::severityToString(
    firmius::shared::CommandSeverity severity) const {
  using firmius::shared::CommandSeverity;
  switch (severity) {
  case CommandSeverity::VULNERABLE:
    return "VULNERABLE";
  case CommandSeverity::HIGH:
    return "HIGH";
  case CommandSeverity::MEDIUM:
    return "MEDIUM";
  case CommandSeverity::LOW:
    return "LOW";
  default:
    return "UNKNOWN";
  }
}

ftxui::Color PermissionPromptModal::severityToColor(
    firmius::shared::CommandSeverity severity) const {
  using firmius::shared::CommandSeverity;
  switch (severity) {
  case CommandSeverity::VULNERABLE:
  case CommandSeverity::HIGH:
    return ftxui::Color::Red;
  case CommandSeverity::MEDIUM:
    return ftxui::Color::Yellow;
  case CommandSeverity::LOW:
    return ftxui::Color::Green;
  default:
    return ftxui::Color::White;
  }
}

std::string PermissionPromptModal::detailSummary() const {
  if (request_.requestType == firmius::shared::PermissionRequestType::Command) {
    return "Grant permission to run the exact command shown below.";
  }
  return "Grant permission to access the exact path shown below.";
}

std::vector<std::string> PermissionPromptModal::detailLines() const {
  std::vector<std::string> lines;
  if (request_.requestType == firmius::shared::PermissionRequestType::Command) {
    lines.push_back("Tool: " +
                    (request_.toolName.empty() ? std::string("unknown")
                                               : request_.toolName));
    lines.push_back("Scope: exact command");
    lines.push_back("Primary command: " +
                    (request_.commandPrimary.empty() ? std::string("unknown")
                                                     : request_.commandPrimary));
    lines.push_back("Command: " +
                    (request_.command.empty() ? std::string("<empty>")
                                              : request_.command));
    if (!request_.toolCallId.empty()) {
      lines.push_back("Tool call id: " + request_.toolCallId);
    }
    return lines;
  }

  lines.push_back("Tool: " +
                  (request_.toolName.empty() ? std::string("unknown")
                                             : request_.toolName));
  lines.push_back("Scope: " +
                  std::string(request_.isDirectory ? "directory"
                                                  : "exact path"));
  lines.push_back("Access: " +
                  std::string(request_.requestType ==
                                      firmius::shared::PermissionRequestType::Read
                                  ? "read"
                                  : "write"));
  lines.push_back("Path: " +
                  (request_.targetPath.empty() ? std::string("<empty>")
                                               : request_.targetPath));
  if (!request_.toolCallId.empty()) {
    lines.push_back("Tool call id: " + request_.toolCallId);
  }
  return lines;
}

ftxui::Component PermissionPromptModal::create(TuiState &state) {
  auto options = std::make_shared<std::vector<std::string>>(
      request_.requestType != firmius::shared::PermissionRequestType::Command
          ? getEditOptions()
          : getCommandOptions());
  auto responses =
      std::make_shared<std::vector<firmius::shared::PermissionResponse>>(
          request_.requestType !=
                  firmius::shared::PermissionRequestType::Command
              ? getEditResponses()
              : getCommandResponses());
  auto selectedOption = std::make_shared<int>(0);
  auto request = request_;
  auto onResult = onResult_;
  auto severityColor = severityToColor(request.severity);
  auto severityLabel = severityToString(request.severity);
  auto details = std::make_shared<std::vector<std::string>>(detailLines());

  auto radiobox = ftxui::Radiobox(options.get(), selectedOption.get());

  auto component =
      ftxui::Renderer(radiobox, [radiobox, request, severityColor,
                                 severityLabel, details, options, this]() {
        const auto &theme = ThemeManager::instance().getCurrentTheme();

        ftxui::Element severityIndicator;
        if (request.requestType ==
                firmius::shared::PermissionRequestType::Command &&
            request.severity == firmius::shared::CommandSeverity::VULNERABLE) {
          severityIndicator =
              ftxui::text(" VULNERABLE - SYSTEM DESTRUCTIVE ") |
              ftxui::bgcolor(ftxui::Color::Red) |
              ftxui::color(ftxui::Color::White) | ftxui::center;
        } else {
          severityIndicator = ftxui::text("");
        }

        ftxui::Element severityText = ftxui::text("");
        if (request.requestType ==
            firmius::shared::PermissionRequestType::Command) {
          severityText = ftxui::text("Severity: " + severityLabel) |
                         ftxui::color(severityColor) | ftxui::center;
        }

        ftxui::Elements detailElements;
        for (const auto &line : *details) {
          detailElements.push_back(ftxui::paragraph(line) |
                                   ftxui::color(theme.base.fg));
        }

        ftxui::Elements content = {
            severityIndicator,
            ftxui::text(request.message) | ftxui::center |
                ftxui::color(theme.modals.fg),
            ftxui::text(""),
            severityText,
            ftxui::text(""),
            ftxui::paragraph(detailSummary()) | ftxui::center |
                ftxui::color(theme.base.fg),
            ftxui::text(""),
            ftxui::vbox(std::move(detailElements)),
            ftxui::text(""),
            radiobox->Render() | ftxui::center,
            ftxui::text(""),
            ftxui::text("(Arrow keys move, Enter confirms, Esc denies)") |
                ftxui::color(theme.base.dim) | ftxui::center,
        };

        return FlatModalPanel(
            theme, request.title,
            ModalSection(theme, ftxui::vbox(std::move(content)),
                         theme.modals.bg),
            72, 22, severityColor);
      });

  return ftxui::CatchEvent(
      component, [selectedOption, responses, onResult, &state,
                  radiobox, options](ftxui::Event event) {
        if (event == ftxui::Event::Escape) {
          state.popModal();
          if (onResult) {
            onResult(firmius::shared::PermissionResponse::Deny);
          }
          return true;
        }

        const bool handledByRadiobox = radiobox->OnEvent(event);
        if (event == ftxui::Event::Return) {
          state.popModal();
          if (onResult) {
            auto result = firmius::shared::PermissionResponse::Deny;
            if (*selectedOption >= 0 &&
                static_cast<size_t>(*selectedOption) < responses->size()) {
              result = responses->at(*selectedOption);
            }
            onResult(result);
          }
          return true;
        }

        if (handledByRadiobox) {
          return true;
        }
        return false;
      });
}

} // namespace firmius::tui
