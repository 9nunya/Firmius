#include "environment/Permissions.hpp"
#include "harness/Harness.hpp"
#include "utils/StringUtil.hpp"
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace firmius::core {

Permissions::Permissions(std::string threadId, std::string agentId)
    : intentAnalyzer_(std::make_unique<CommandIntentAnalyzer>()),
      threadId_(std::move(threadId)), agentId_(std::move(agentId))
{
}

void Permissions::bindContext(const AgentContext& context) {
    context_ = &context;
    permissionChecks_ = std::make_unique<AgentPermissionChecks>(context);
}

PermissionResponse Permissions::requestCommandApproval(
    const std::string& command,
    const CommandIntent& intent) {
    if (intent.severity == CommandSeverity::VULNERABLE) {
        return PermissionResponse::Deny;
    }

    auto mode = Harness::instance().threadPermissionMode(threadId_);
    if (mode == ThreadPermissionMode::DenyAll) {
        return PermissionResponse::Deny;
    }
    if (mode == ThreadPermissionMode::AlwaysAllow) {
        return PermissionResponse::AllowAlways;
    }

    if (Harness::instance().commandMatchesPersistedAllowRule(threadId_, command)) {
        return PermissionResponse::AllowAlways;
    }

    PermissionEscalationRequest request;
    request.threadId = threadId_;
    request.agentId = agentId_;
    request.requestType = PermissionRequestType::Command;
    request.title = "Command permission required";
    request.message = "Approve this command execution request.";
    request.command = command;
    request.severity = intent.severity;
    request.allowAlways = true;
    auto response = Harness::instance().requestPermissionEscalation(std::move(request));
    if (response == PermissionResponse::AllowAlways) {
        Harness::instance().persistCommandAllowRule(
            threadId_, makeCommandAllowRule(command, intent));
    }
    return response;
}

PermissionResponse Permissions::requestEditApproval(
    const std::string& absolutePath) {
    if (permissionChecks_ && !permissionChecks_->checkPathAccess(absolutePath)) {
        return PermissionResponse::Deny;
    }

    auto mode = Harness::instance().threadPermissionMode(threadId_);
    if (mode == ThreadPermissionMode::DenyAll) {
        return PermissionResponse::Deny;
    }
    if (mode == ThreadPermissionMode::AlwaysAllow) {
        return PermissionResponse::AllowAlways;
    }

    if (Harness::instance().pathMatchesPersistedWriteAllowRule(threadId_, absolutePath)) {
        return PermissionResponse::AllowAlways;
    }

    PermissionEscalationRequest request;
    request.threadId = threadId_;
    request.agentId = agentId_;
    request.requestType = PermissionRequestType::Edit;
    request.title = "Write permission required";
    request.message = "Approve this file edit or write request.";
    request.targetPath = absolutePath;
    request.severity = CommandSeverity::MEDIUM;
    request.allowAlways = true;
    auto response = Harness::instance().requestPermissionEscalation(std::move(request));
    if (response == PermissionResponse::AllowAlways) {
        Harness::instance().persistWriteAllowPath(
            threadId_, deriveWriteAllowPathPrefix(absolutePath));
    }
    return response;
}

bool Permissions::checkPathAccess(
    const std::string& absolutePath,
    AccessMode mode) const {
    if (!permissionChecks_) {
        return true;
    }

    if (!permissionChecks_->checkPathAccess(absolutePath)) {
        return false;
    }

    if (mode != AccessMode::WRITE) {
        return true;
    }

    auto threadMode = Harness::instance().threadPermissionMode(threadId_);
    if (threadMode == ThreadPermissionMode::DenyAll) {
        return false;
    }

    return threadMode == ThreadPermissionMode::AlwaysAllow ||
           Harness::instance().pathMatchesPersistedWriteAllowRule(threadId_, absolutePath);
}

void Permissions::validatePathAccess(
    const std::string& absolutePath,
    AccessMode mode) const {
    if (permissionChecks_) {
        permissionChecks_->validatePathAccess(absolutePath);
    }

    if (mode != AccessMode::WRITE) {
        return;
    }

    auto response = const_cast<Permissions*>(this)->requestEditApproval(absolutePath);
    if (response == PermissionResponse::Deny) {
        throw std::runtime_error("Write access denied: " + absolutePath);
    }
}

bool Permissions::isCommandAllowed(const CommandIntent& intent) const {
    if (intent.severity == CommandSeverity::VULNERABLE) {
        return false;
    }

    auto mode = Harness::instance().threadPermissionMode(threadId_);
    if (mode == ThreadPermissionMode::DenyAll) {
        return false;
    }
    if (mode == ThreadPermissionMode::AlwaysAllow) {
        return true;
    }

    return Harness::instance().commandMatchesPersistedAllowRule(
        threadId_, intent.originalCommand);
}

void Permissions::allowCommandAlways(const std::string& pattern) {
    CommandIntent intent = intentAnalyzer_->analyze(pattern);
    Harness::instance().persistCommandAllowRule(
        threadId_, makeCommandAllowRule(pattern, intent));
}

void Permissions::denyCommandAlways(const std::string& pattern) {
    (void)pattern;
}

const ICommandIntentAnalyzer& Permissions::getIntentAnalyzer() const {
    return *intentAnalyzer_;
}

void Permissions::setApprovalMode(ThreadPermissionMode mode) {
    if (threadId_.empty()) {
        return;
    }

    if (Harness::instance().currentThreadId() == threadId_) {
        Harness::instance().setCurrentThreadPermissionMode(mode);
    }
}

std::string Permissions::normalizeCommand(const std::string& command) {
    std::stringstream normalized;
    bool previousWasSpace = false;
    for (char ch : command) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!previousWasSpace) {
                normalized << ' ';
                previousWasSpace = true;
            }
        } else {
            normalized << ch;
            previousWasSpace = false;
        }
    }

    return shared::StringUtil::trim(normalized.str());
}

CommandAllowRule Permissions::makeCommandAllowRule(const std::string& command,
                                                   const CommandIntent& intent) {
    CommandAllowRule rule;
    rule.exactCommand = command;
    rule.normalizedCommand = normalizeCommand(command);
    rule.primaryCommand = intent.primaryCommand;
    rule.severity = intent.severity;
    return rule;
}

std::string Permissions::deriveWriteAllowPathPrefix(const std::string& absolutePath) {
    if (absolutePath.empty()) {
        return "";
    }

    std::filesystem::path path(absolutePath);
    std::filesystem::path scope = path;

    if (absolutePath.back() != std::filesystem::path::preferred_separator) {
        if (path.has_filename() && path.parent_path() != path) {
            scope = path.parent_path();
        }
    }

    auto normalized = scope.lexically_normal().string();
    if (normalized.empty() || normalized == "/") {
        return "/**";
    }
    if (normalized.ends_with("/**")) {
        return normalized;
    }
    if (normalized.back() == std::filesystem::path::preferred_separator) {
        normalized.pop_back();
    }
    return normalized + "/**";
}

} // namespace firmius::core
