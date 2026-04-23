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
    : request_(std::move(request))
    , onResult_(std::move(onResult))
{}

std::vector<std::string> PermissionPromptModal::getEditOptions() const {
    std::vector<std::string> options = {
        "Allow once",
        "Always allow writes in this location for this session",
        "Always allow writes in this location globally",
        "No, don't allow"
    };
    if (request_.requestType == firmius::shared::PermissionRequestType::Read) {
        options = {
            "Allow once",
            "Always allow this location for this session",
            "Always allow this location globally",
            "Allow all directory reads this session",
            "No, don't allow"
        };
    }
    return options;
}

std::vector<std::string> PermissionPromptModal::getCommandOptions() const {
    return {
        "Run once",
        "Allow this command for this session",
        "Allow entire tool for this session",
        "Allow this command globally",
        "No, don't run it"
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
            PermissionResponse::Deny
        };
    }
    return {
        PermissionResponse::AllowOnce,
        PermissionResponse::AllowPathSession,
        PermissionResponse::AllowPathGlobal,
        PermissionResponse::Deny
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
        PermissionResponse::Deny
    };
}

std::string PermissionPromptModal::severityToString(firmius::shared::CommandSeverity severity) const {
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

ftxui::Color PermissionPromptModal::severityToColor(firmius::shared::CommandSeverity severity) const {
    using firmius::shared::CommandSeverity;
    switch (severity) {
        case CommandSeverity::VULNERABLE:
            return ftxui::Color::Red;
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

ftxui::Component PermissionPromptModal::create(TuiState &state) {
    auto options = std::make_shared<std::vector<std::string>>(
        request_.requestType != firmius::shared::PermissionRequestType::Command
            ? getEditOptions()
            : getCommandOptions());
    auto responses = std::make_shared<std::vector<firmius::shared::PermissionResponse>>(
        request_.requestType != firmius::shared::PermissionRequestType::Command
            ? getEditResponses()
            : getCommandResponses());
    auto selectedOption = std::make_shared<int>(0);
    auto request = request_;
    auto onResult = onResult_;
    auto severityColor = severityToColor(request.severity);
    auto severityLabel = severityToString(request.severity);

    auto radiobox = ftxui::Radiobox(options.get(), selectedOption.get());

    auto component = ftxui::Renderer(radiobox, [radiobox, options, request, severityColor, severityLabel]() {
        const auto &theme = ThemeManager::instance().getCurrentTheme();

        ftxui::Element severityIndicator;
        if (request.requestType == firmius::shared::PermissionRequestType::Command &&
            request.severity == firmius::shared::CommandSeverity::VULNERABLE) {
            severityIndicator = ftxui::text(" VULNERABLE - SYSTEM DESTRUCTIVE ")
                | ftxui::bgcolor(ftxui::Color::Red)
                | ftxui::color(ftxui::Color::White)
                | ftxui::center;
        } else {
            severityIndicator = ftxui::text("");
        }

        std::string detail = request.command.empty() ? request.targetPath : request.command;
        ftxui::Element severityText = ftxui::text("");
        if (request.requestType == firmius::shared::PermissionRequestType::Command) {
            severityText = ftxui::text("Severity: " + severityLabel)
                | ftxui::color(severityColor) | ftxui::center;
        }

        ftxui::Elements content = {
            severityIndicator,
            ftxui::text(request.message) | ftxui::center | ftxui::color(theme.modals.fg),
            ftxui::text(""),
            severityText,
            ftxui::text(""),
            ftxui::paragraph(detail) | ftxui::color(theme.base.fg),
            ftxui::text(""),
            radiobox->Render() | ftxui::center,
            ftxui::text(""),
            ftxui::text("(Press Enter to confirm, Esc to cancel)")
                | ftxui::color(theme.base.dim) | ftxui::center
        };

        return FlatModalPanel(
            theme, request.title,
            ModalSection(
                theme,
                ftxui::vbox(std::move(content)),
                theme.modals.bg),
            72, 22, severityColor);
    });

    return ftxui::CatchEvent(component, [selectedOption, responses, onResult, &state](ftxui::Event event) {
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
        if (event == ftxui::Event::Escape) {
            state.popModal();
            if (onResult) {
                onResult(firmius::shared::PermissionResponse::Deny);
            }
            return true;
        }
        return false;
    });
}

} // namespace firmius::tui
