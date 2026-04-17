#include "AuditCliUtils.hpp"
#include "AuditRegistry.hpp"
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::vector<std::string> toArgs(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

void printUsage(const std::string& exeName) {
    std::cout << "Usage: " << exeName << " --audit <id> [args...]" << std::endl;
    std::cout << "       " << exeName << " --list" << std::endl;
    std::cout << "       aliases: provider_stream_debug, provider_live_agent -> provider_full_range" << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    auto audits = firmius::audits::createAudits();
    std::unordered_map<std::string, firmius::shared::IAudit*> auditMap;
    for (auto& audit : audits) {
        auditMap[audit->getId()] = audit.get();
    }

    std::string exeName = (argc > 0 && argv[0]) ? argv[0] : "firmius_audit";
    std::vector<std::string> args = toArgs(argc, argv);
    bool listOnly = false;
    bool showHelp = false;
    std::string auditId;
    std::vector<std::string> auditArgs;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--list" || arg == "-l") {
            if (auditId.empty()) {
                listOnly = true;
            } else {
                auditArgs.push_back(arg);
            }
        } else if ((arg == "--help" || arg == "-h") && auditId.empty()) {
            showHelp = true;
        } else if (arg == "--audit" && i + 1 < args.size()) {
            auditId = args[++i];
        } else {
            auditArgs.push_back(arg);
        }
    }

    if (listOnly || showHelp || auditId.empty()) {
        printUsage(exeName);
        std::cout << "Available audits:" << std::endl;
        for (const auto& audit : audits) {
            std::cout << "  " << audit->getId() << " - " << audit->getDescription() << std::endl;
        }
        std::cout << "  provider_stream_debug - alias for provider_full_range" << std::endl;
        std::cout << "  provider_live_agent - alias for provider_full_range --live-agent" << std::endl;
        if (listOnly) {
            return 0;
        }
        return auditId.empty() ? 1 : 0;
    }

    auditArgs = firmius::audits::cli::normalizeAuditArgs(auditId, auditArgs);
    auditId = firmius::audits::cli::canonicalAuditId(auditId);
    auto it = auditMap.find(auditId);
    if (it == auditMap.end()) {
        std::cerr << "Unknown audit: " << auditId << std::endl;
        printUsage(exeName);
        return 1;
    }

    auto result = it->second->run(auditArgs);
    if (!result.output.empty()) {
        std::cout << result.output << std::endl;
    }
    return result.exitCode;
}
