#include "audits/CodexProviderAudit.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

std::string CodexProviderAudit::getId() const { return "codex_provider"; }

std::string CodexProviderAudit::getDescription() const { return "Codex provider tool-call audit"; }

shared::AuditResult CodexProviderAudit::run(const std::vector<std::string>&) {
    AuditResult result;
    result.auditId = getId();
    std::cout << "Starting Codex Provider Audit..." << std::endl;
    Panic::init();
    EnvLoader::load(".env.local");
    setenv("FIRMIUS_PRETTY_PRINT", "1", 1);
    auto& harness = Harness::instance();
    harness.init();
    HostCreationOptions opts;
    opts.type = HostType::Docker;
    opts.containerName = "codex-audit-sandbox";
    opts.deleteOnExit = true;
    int exitCode = 0;
    do {
        std::string threadId = harness.newThread(opts, "/work", "firmius");
        if (threadId.empty()) {
            std::cerr << "Failed to create Docker thread." << std::endl;
            exitCode = 1;
            break;
        }
        std::cout << "Docker thread created: " << threadId << std::endl;
        harness.switchModel("codex", "gpt-5.1-codex-mini");
        auto runTurn = [&](const std::string& prompt) -> bool {
            std::mutex mtx;
            std::condition_variable cv;
            bool done = false;
            bool hadOutput = false;
            std::string agentResponse;
            int subId = harness.subscribe([&](const AppEvent& ev) {
                std::visit([&](auto&& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, AgentError>) {
                        std::cout << "[Event] AgentError: " << e.message << std::endl;
                        done = true;
                        cv.notify_one();
                    } else if constexpr (std::is_same_v<T, AgentTurnCompleted>) {
                        std::cout << "[Event] AgentTurnCompleted" << std::endl;
                        done = true;
                        cv.notify_one();
                    } else if constexpr (std::is_same_v<T, AgentText>) {
                        std::cout << e.delta << std::flush;
                        agentResponse += e.delta;
                        hadOutput = true;
                    } else if constexpr (std::is_same_v<T, AgentThinking>) {
                        std::cout << "\x1B[3m" << e.delta << "\x1B[0m" << std::flush;
                        hadOutput = true;
                    } else if constexpr (std::is_same_v<T, AgentToolCall>) {
                        std::cout << "\n[Tool Call] " << e.toolName << "(" << e.toolArgs << ")" << std::endl;
                        std::ofstream out("/tmp/codex_request.json", std::ios::app);
                        out << "\n--- TOOL CALL ---\n" << e.toolName << "(" << e.toolArgs << ")\n";
                    } else if constexpr (std::is_same_v<T, AgentProcessSpawned>) {
                        std::cout << "[Process Spawned] " << e.command << std::endl;
                        std::ofstream out("/tmp/codex_request.json", std::ios::app);
                        out << "\n--- PROCESS SPAWNED ---\n" << e.command << "\n";
                    } else if constexpr (std::is_same_v<T, AgentProcessOutput>) {
                        std::cout << e.output << std::flush;
                        std::ofstream out("/tmp/codex_request.json", std::ios::app);
                        out << "\n--- PROCESS OUTPUT ---\n" << e.output << "\n";
                    }
                }, ev);
            });
            std::cout << "\n> User: " << prompt << std::endl;
            {
                std::ofstream out("/tmp/codex_request.json", std::ios::app);
                out << "\n--- PROMPT ---\n" << prompt << "\n";
            }
            harness.send(prompt);
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&] { return done; });
            harness.unsubscribe(subId);
            std::cout << std::endl;
            {
                std::ofstream out("/tmp/codex_request.json", std::ios::app);
                out << "\n--- FULL RESPONSE ---\n" << agentResponse << "\n";
            }
            return hadOutput;
        };
        std::cout << "--- Phase 1: Host-Side Environment Setup ---" << std::endl;
        runTurn("Prepare to audit.");
        std::cout << "Creating audit environment via docker exec..." << std::endl;
        std::string setupCmd =
            "docker exec codex-audit-sandbox sh -c '"
            "mkdir -p /work/audit_dir1 /work/audit_dir2 /work/audit_dir3 && "
            "echo \"TOKEN_1_1\" > /work/audit_dir1/file1.txt && "
            "echo \"TOKEN_1_2\" > /work/audit_dir1/file2.txt && "
            "echo \"TOKEN_1_3\" > /work/audit_dir1/file3.txt && "
            "echo \"TOKEN_2_1\" > /work/audit_dir2/file4.txt && "
            "echo \"TOKEN_2_2\" > /work/audit_dir2/file5.txt && "
            "echo \"TOKEN_2_3\" > /work/audit_dir2/file6.txt && "
            "echo \"TOKEN_3_1\" > /work/audit_dir3/file7.txt && "
            "echo \"TOKEN_3_2\" > /work/audit_dir3/file8.txt && "
            "echo \"TOKEN_3_3\" > /work/audit_dir3/file9.txt && "
            "echo \"TOKEN_3_4\" > /work/audit_dir3/file10.txt'";
        if (system(setupCmd.c_str()) != 0) {
            std::cerr << "Failed to set up environment via docker exec." << std::endl;
            exitCode = 1;
            break;
        }
        std::cout << "--- Phase 2: Agent Verification ---" << std::endl;
        runTurn("Explain the Riemann Hypothesis and its connection to the Prime Number Theorem in detail. Then, list the current directory using list_directory.");
        std::cout << "\nAudit complete." << std::endl;
    } while (false);
    harness.shutdown();
    result.exitCode = exitCode;
    result.passed = (exitCode == 0);
    return result;
}

}
