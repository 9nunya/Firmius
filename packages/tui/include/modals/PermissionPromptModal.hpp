#pragma once

#include "Events.hpp"
#include "modals/IModal.hpp"
#include <functional>
#include <string>
#include <vector>

namespace firmius::tui {

class PermissionPromptModal : public IModal {
public:
    PermissionPromptModal(
        firmius::shared::PermissionEscalationRequest request,
        std::function<void(firmius::shared::PermissionResponse)> onResult);

    std::string name() const override { return "permission_prompt"; }
    ftxui::Component create(TuiState &state) override;

private:
    firmius::shared::PermissionEscalationRequest request_;
    std::function<void(firmius::shared::PermissionResponse)> onResult_;

    std::vector<std::string> getEditOptions() const;
    std::vector<std::string> getCommandOptions() const;
    std::vector<firmius::shared::PermissionResponse> getEditResponses() const;
    std::vector<firmius::shared::PermissionResponse> getCommandResponses() const;
    std::string severityToString(firmius::shared::CommandSeverity severity) const;
    ftxui::Color severityToColor(firmius::shared::CommandSeverity severity) const;
};

} // namespace firmius::tui
