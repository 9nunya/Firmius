#include "environment/Permissions.hpp"

#include "environment/PermissionSuggestionEngine.hpp"
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

namespace {

PolicyRequest buildCommandPolicyRequest(const std::string &command,
                                         const CommandIntent &intent,
                                         const std::string &cwd,
                                         const std::string &toolName) {
    PolicyRequest req;
    req.category = kCatProcessExec;
    req.command = command;
    req.commandPrimary = intent.primaryCommand;
    req.cwd = cwd;
    req.subcommands = intent.parsedCommands;
    req.toolName = toolName;
    return req;
}

PolicyRequest buildPathPolicyRequest(const char *category,
                                      const std::string &absolutePath,
                                      const std::string &toolName,
                                      bool isDirectory) {
    PolicyRequest req;
    req.category = category;
    req.path = absolutePath;
    req.isDirectory = isDirectory;
    req.toolName = toolName;
    return req;
}

PermissionEscalationRequest buildEscalationFromPolicyRequest(
    const PolicyRequest &policyReq,
    const std::string &threadId,
    const std::string &agentId,
    const CommandIntent *intent) {
    PermissionEscalationRequest e;
    e.threadId = threadId;
    e.agentId = agentId;
    e.toolName = policyReq.toolName;
    e.category = policyReq.category;
    e.cwd = policyReq.cwd;
    e.command = policyReq.command;
    e.commandPrimary = policyReq.commandPrimary;
    e.subcommands = policyReq.subcommands;
    e.targetPath = policyReq.path;
    e.isDirectory = policyReq.isDirectory;
    e.url = policyReq.url;
    e.host = policyReq.host;
    e.scheme = policyReq.scheme;
    e.query = policyReq.query;
    e.persona = policyReq.persona;
    e.parentPersona = policyReq.parentPersona;
    e.toolScopes = policyReq.toolScopes;
    e.allowAlways = true;

    if (policyReq.category == kCatProcessExec) {
        e.requestType = PermissionRequestType::Command;
        e.title = "Run command?";
        e.message = "Approve this command for the agent.";
        e.severity = intent ? intent->severity : CommandSeverity::MEDIUM;
    } else if (policyReq.category == kCatFileRead ||
               policyReq.category == kCatNetworkSearch ||
               policyReq.category == kCatNetworkFetch) {
        e.requestType = PermissionRequestType::Read;
        e.title = "Approve access?";
        e.message = "Approve this access request.";
        e.severity = CommandSeverity::LOW;
    } else if (policyReq.category == kCatAgentSpawn) {
        e.requestType = PermissionRequestType::Read;
        e.title = "Spawn subagent?";
        e.message = "Approve this subagent spawn.";
        e.severity = CommandSeverity::LOW;
    } else {
        e.requestType = PermissionRequestType::Edit;
        e.title = "Approve edit?";
        e.message = "Approve this file modification.";
        e.severity = CommandSeverity::MEDIUM;
    }
    return e;
}

PermissionResponse evaluateAndEscalate(
    const PolicyRequest &policyReq,
    const std::string &threadId,
    const std::string &agentId,
    const CommandIntent *intent) {
    auto eval = Harness::instance().policyEngine().evaluate(policyReq);
    if (eval.decision == PolicyDecision::Allow) {
        return PermissionResponse::AllowAlways;
    }
    if (eval.decision == PolicyDecision::Deny) {
        return PermissionResponse::Deny;
    }

    // Need to ask. Build escalation + suggestions.
    auto escalation =
        buildEscalationFromPolicyRequest(policyReq, threadId, agentId, intent);
    CommandIntent emptyIntent;
    auto suggestions = PermissionSuggestionEngine::generate(
        policyReq, intent ? *intent : emptyIntent);

    return Harness::instance().requestPermissionEscalationWithSuggestions(
        std::move(escalation), std::move(suggestions));
}

} // namespace

PermissionResponse Permissions::requestCommandApproval(
    const std::string& command,
    const CommandIntent& intent,
    const std::string& toolName) {
    if (intent.severity == CommandSeverity::VULNERABLE) {
        return PermissionResponse::Deny;
    }

    std::string cwd;
    if (context_) cwd = context_->environment.cwd;
    auto req = buildCommandPolicyRequest(command, intent, cwd, toolName);
    return evaluateAndEscalate(req, threadId_, agentId_, &intent);
}

PermissionResponse Permissions::requestReadApproval(
    const std::string& absolutePath) {
    auto req = buildPathPolicyRequest(kCatFileRead, absolutePath, "",
                                       /*isDirectory=*/false);
    return evaluateAndEscalate(req, threadId_, agentId_, nullptr);
}

PermissionResponse Permissions::requestEditApproval(
    const std::string& absolutePath) {
    // Edit covers both write to existing files AND create new ones. We
    // pick the more permissive category by default (file.write); tools
    // that create can pre-check file.create explicitly via the policy
    // engine if they need separate semantics.
    const bool exists = std::filesystem::exists(absolutePath);
    const char *category = exists ? kCatFileWrite : kCatFileCreate;
    auto req = buildPathPolicyRequest(category, absolutePath, "", false);
    return evaluateAndEscalate(req, threadId_, agentId_, nullptr);
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

    // Path is outside the workspace allow-set. Consult policy.
    PolicyRequest req;
    req.path = absolutePath;
    if (mode == AccessMode::READ || mode == AccessMode::EXECUTE) {
        req.category = kCatFileRead;
    } else {
        req.category = std::filesystem::exists(absolutePath)
                            ? kCatFileWrite : kCatFileCreate;
    }
    auto eval = Harness::instance().policyEngine().evaluate(req);
    if (eval.decision == PolicyDecision::Allow) return true;
    if (eval.decision == PolicyDecision::Deny) return false;
    return false;
}

void Permissions::validatePathAccess(
    const std::string& absolutePath,
    AccessMode mode) const {
    if (checkPathAccess(absolutePath, mode)) return;

    if (mode == AccessMode::READ || mode == AccessMode::EXECUTE) {
        const auto response =
            const_cast<Permissions*>(this)->requestReadApproval(absolutePath);
        if (response == PermissionResponse::Deny) {
            throw std::runtime_error("Read access denied: " + absolutePath);
        }
        return;
    }

    const auto response =
        const_cast<Permissions*>(this)->requestEditApproval(absolutePath);
    if (response == PermissionResponse::Deny) {
        throw std::runtime_error("Write access denied: " + absolutePath);
    }
}

bool Permissions::isCommandAllowed(const CommandIntent& intent) const {
    if (intent.severity == CommandSeverity::VULNERABLE) return false;

    PolicyRequest req;
    req.category = kCatProcessExec;
    req.command = intent.originalCommand;
    req.commandPrimary = intent.primaryCommand;
    req.subcommands = intent.parsedCommands;
    if (context_) req.cwd = context_->environment.cwd;
    auto eval = Harness::instance().policyEngine().evaluate(req);
    return eval.decision == PolicyDecision::Allow;
}

const ICommandIntentAnalyzer& Permissions::getIntentAnalyzer() const {
    return *intentAnalyzer_;
}

namespace {
bool isDirectoryPath(const std::string& absolutePath) {
    if (absolutePath.empty()) return false;
    const std::filesystem::path path(absolutePath);
    return absolutePath.back() == std::filesystem::path::preferred_separator ||
           (!path.has_extension() && !path.has_filename());
}
} // namespace

PermissionEscalationRequest Permissions::buildCommandRequest(
    const std::string& command, const CommandIntent& intent,
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
    request.category = kCatProcessExec;
    if (context_) request.cwd = context_->environment.cwd;
    return request;
}

PermissionEscalationRequest Permissions::buildPathRequest(
    PermissionRequestType type, const std::string& absolutePath) const {
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
        ? CommandSeverity::LOW : CommandSeverity::MEDIUM;
    request.allowAlways = true;
    request.isDirectory = isDirectoryPath(absolutePath);
    request.category = type == PermissionRequestType::Read
        ? kCatFileRead
        : (std::filesystem::exists(absolutePath) ? kCatFileWrite : kCatFileCreate);
    return request;
}

} // namespace firmius::core
