#ifndef FIRMIUS_CORE_PERMISSIONS_HPP
#define FIRMIUS_CORE_PERMISSIONS_HPP

#include "IPermissions.hpp"
#include "Events.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "environment/CommandIntentAnalyzer.hpp"
#include "persistence/ThreadManager.hpp"
#include "Context.hpp"

#include <memory>
#include <string>

namespace firmius::core {

using namespace firmius::shared;

class Permissions : public IPermissions {
public:
    Permissions(std::string threadId = "", std::string agentId = "");
    void bindContext(const AgentContext& context);

    PermissionResponse requestCommandApproval(
        const std::string& command,
        const CommandIntent& intent,
        const std::string& toolName = "") override;

    PermissionResponse requestReadApproval(
        const std::string& absolutePath) override;

    PermissionResponse requestEditApproval(
        const std::string& absolutePath) override;

    bool checkPathAccess(
        const std::string& absolutePath,
        AccessMode mode) const override;

    void validatePathAccess(
        const std::string& absolutePath,
        AccessMode mode) const override;

    bool isCommandAllowed(const CommandIntent& intent) const override;

    void allowCommandAlways(const std::string& pattern) override;
    void denyCommandAlways(const std::string& pattern) override;

    const ICommandIntentAnalyzer& getIntentAnalyzer() const override;

    void setApprovalMode(ThreadPermissionMode mode) override;

private:
    const AgentContext* context_ = nullptr;
    std::unique_ptr<AgentPermissionChecks> permissionChecks_;
    std::unique_ptr<CommandIntentAnalyzer> intentAnalyzer_;
    std::string threadId_;
    std::string agentId_;

    static std::string normalizeCommand(const std::string& command);
    static CommandAllowRule makeCommandAllowRule(const std::string& command,
                                                 const CommandIntent& intent);
    static std::string deriveAllowPathPrefix(const std::string& absolutePath);
    static bool isDirectoryPath(const std::string& absolutePath);

    PermissionEscalationRequest buildCommandRequest(const std::string& command,
                                                    const CommandIntent& intent,
                                                    const std::string& toolName) const;
    PermissionEscalationRequest buildPathRequest(PermissionRequestType type,
                                                 const std::string& absolutePath) const;
    void persistResponse(const PermissionEscalationRequest& request,
                         PermissionResponse response) const;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_PERMISSIONS_HPP
