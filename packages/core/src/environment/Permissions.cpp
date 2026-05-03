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
      threadId_(std::move(threadId)), agentId_(std::move(agentId)) {}

void Permissions::bindContext(const AgentContext& context) {
    context_ = &context;
    permissionChecks_ = std::make_unique<AgentPermissionChecks>(context);
}

PermissionResponse Permissions::requestCommandApproval(
    const std::string& command,
    const CommandIntent& intent,
    const std::string& toolName) {
    if (intent.severity == CommandSeverity::VULNERABLE) {
        return PermissionResponse::Deny;
    }

    const auto mode = Harness::instance().threadPermissionMode(threadId_);
    if (mode == ThreadPermissionMode::DenyAll) {
        return PermissionResponse::Deny;
    }
    if (mode == ThreadPermissionMode::AlwaysAllow) {
        return PermissionResponse::AllowAlways;
    }

    if (Harness::instance().toolHasSessionAllowance(threadId_, toolName) ||
        Harness::instance().commandMatchesPersistedAllowRule(threadId_, command, toolName)) {
        return PermissionResponse::AllowAlways;
    }

    const auto request = buildCommandRequest(command, intent, toolName);
    const auto response = Harness::instance().requestPermissionEscalation(request);
    persistResponse(request, response);
    return response;
}

PermissionResponse Permissions::requestReadApproval(
    const std::string& absolutePath) {
    const auto mode = Harness::instance().threadPermissionMode(threadId_);
    if (mode == ThreadPermissionMode::DenyAll) {
        return PermissionResponse::Deny;
    }
    if (mode == ThreadPermissionMode::AlwaysAllow) {
        return PermissionResponse::AllowAlways;
    }

    if (Harness::instance().readHasSessionAllowance(threadId_) ||
        Harness::instance().pathMatchesPersistedAllowRule(threadId_, absolutePath, true, "")) {
        return PermissionResponse::AllowAlways;
    }

    const auto request = buildPathRequest(PermissionRequestType::Read, absolutePath);
    const auto response = Harness::instance().requestPermissionEscalation(request);
    persistResponse(request, response);
    return response;
}

PermissionResponse Permissions::requestEditApproval(
    const std::string& absolutePath) {
    const auto mode = Harness::instance().threadPermissionMode(threadId_);
    if (mode == ThreadPermissionMode::DenyAll) {
        return PermissionResponse::Deny;
    }
    if (mode == ThreadPermissionMode::AlwaysAllow) {
        return PermissionResponse::AllowAlways;
    }

    if (Harness::instance().pathMatchesPersistedAllowRule(threadId_, absolutePath, false, "")) {
        return PermissionResponse::AllowAlways;
    }

    const auto request = buildPathRequest(PermissionRequestType::Edit, absolutePath);
    const auto response = Harness::instance().requestPermissionEscalation(request);
    persistResponse(request, response);
    return response;
}

bool Permissions::checkPathAccess(
    const std::string& absolutePath,
    AccessMode mode) const {
    if (!permissionChecks_) {
        return true;
    }

    if (permissionChecks_->checkPathAccess(absolutePath)) {
        return true;
    }

    const bool readOnly = mode == AccessMode::READ || mode == AccessMode::EXECUTE;
    if ((readOnly && Harness::instance().readHasSessionAllowance(threadId_)) ||
        Harness::instance().pathMatchesPersistedAllowRule(threadId_, absolutePath, readOnly, "")) {
        return true;
    }

    const auto threadMode = Harness::instance().threadPermissionMode(threadId_);
    if (threadMode == ThreadPermissionMode::DenyAll) {
        return false;
    }
    if (threadMode == ThreadPermissionMode::AlwaysAllow) {
        return true;
    }
    return false;
}

void Permissions::validatePathAccess(
    const std::string& absolutePath,
    AccessMode mode) const {
    if (checkPathAccess(absolutePath, mode)) {
        return;
    }

    if (mode == AccessMode::READ || mode == AccessMode::EXECUTE) {
        const auto response = const_cast<Permissions*>(this)->requestReadApproval(absolutePath);
        if (response == PermissionResponse::Deny) {
            throw std::runtime_error("Read access denied: " + absolutePath);
        }
        return;
    }

    const auto response = const_cast<Permissions*>(this)->requestEditApproval(absolutePath);
    if (response == PermissionResponse::Deny) {
        throw std::runtime_error("Write access denied: " + absolutePath);
    }
}

bool Permissions::isCommandAllowed(const CommandIntent& intent) const {
    if (intent.severity == CommandSeverity::VULNERABLE) {
        return false;
    }

    const auto mode = Harness::instance().threadPermissionMode(threadId_);
    if (mode == ThreadPermissionMode::DenyAll) {
        return false;
    }
    if (mode == ThreadPermissionMode::AlwaysAllow) {
        return true;
    }

    return Harness::instance().commandMatchesPersistedAllowRule(
        threadId_, intent.originalCommand, "");
}

void Permissions::allowCommandAlways(const std::string& pattern) {
    const auto intent = intentAnalyzer_->analyze(pattern);
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

std::string Permissions::deriveAllowPathPrefix(const std::string& absolutePath) {
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

bool Permissions::isDirectoryPath(const std::string& absolutePath) {
    if (absolutePath.empty()) {
        return false;
    }
    const std::filesystem::path path(absolutePath);
    return absolutePath.back() == std::filesystem::path::preferred_separator ||
           (!path.has_extension() && !path.has_filename());
}

PermissionEscalationRequest Permissions::buildCommandRequest(
    const std::string& command,
    const CommandIntent& intent,
    const std::string& toolName) const {
    PermissionEscalationRequest request;
    request.threadId = threadId_;
    request.agentId = agentId_;
    request.requestType = PermissionRequestType::Command;
    request.title = "Command permission required";
    request.message = "Approve this command execution request.";
    request.command = command;
    request.commandPrimary = intent.primaryCommand;
    request.toolName = toolName;
    request.severity = intent.severity;
    request.allowAlways = true;
    return request;
}

PermissionEscalationRequest Permissions::buildPathRequest(
    PermissionRequestType type,
    const std::string& absolutePath) const {
    PermissionEscalationRequest request;
    request.threadId = threadId_;
    request.agentId = agentId_;
    request.requestType = type;
    request.title = type == PermissionRequestType::Read
        ? "Read permission required"
        : "Write permission required";
    request.message = type == PermissionRequestType::Read
        ? "Approve this file or directory read request."
        : "Approve this file edit or write request.";
    request.targetPath = absolutePath;
    request.severity = type == PermissionRequestType::Read
        ? CommandSeverity::LOW
        : CommandSeverity::MEDIUM;
    request.allowAlways = true;
    request.isDirectory = isDirectoryPath(absolutePath);
    return request;
}

void Permissions::persistResponse(const PermissionEscalationRequest& request,
                                  PermissionResponse response) const {
    if (threadId_.empty()) {
        return;
    }

    switch (response) {
        case PermissionResponse::AllowCommandSession:
        case PermissionResponse::AllowCommandGlobal:
        case PermissionResponse::AllowAlways: {
            if (request.requestType != PermissionRequestType::Command) {
                PathAllowRule rule;
                rule.pathPrefix = deriveAllowPathPrefix(request.targetPath);
                rule.toolName = request.toolName;
                rule.readOnly = request.requestType == PermissionRequestType::Read;
                rule.isGlobal = false;
                Harness::instance().persistPathAllowRule(threadId_, rule);
                break;
            }
            const auto intent = intentAnalyzer_->analyze(request.command);
            auto rule = makeCommandAllowRule(request.command, intent);
            rule.toolName = request.toolName;
            rule.isGlobal = response == PermissionResponse::AllowCommandGlobal;
            Harness::instance().persistCommandAllowRule(threadId_, rule);
            break;
        }
        case PermissionResponse::AllowCommandToolSession:
        case PermissionResponse::AllowAllToolSession:
            Harness::instance().persistToolSessionAllowance(threadId_, request.toolName);
            break;
        case PermissionResponse::AllowPathSession:
        case PermissionResponse::AllowPathGlobal: {
            PathAllowRule rule;
            rule.pathPrefix = deriveAllowPathPrefix(request.targetPath);
            rule.toolName = request.toolName;
            rule.readOnly = request.requestType == PermissionRequestType::Read;
            rule.isGlobal = response == PermissionResponse::AllowPathGlobal;
            Harness::instance().persistPathAllowRule(threadId_, rule);
            break;
        }
        case PermissionResponse::AllowAllReadsSession:
            Harness::instance().persistReadSessionAllowance(threadId_);
            break;
        case PermissionResponse::AllowOnce:
        case PermissionResponse::Deny:
            break;
    }
}

} // namespace firmius::core
