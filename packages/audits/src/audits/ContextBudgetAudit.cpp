#include "audits/ContextBudgetAudit.hpp"

#include "EnvLoader.hpp"
#include "agents/ContextBudget.hpp"
#include "benchmarks/BenchmarkSession.hpp"
#include "harness/Harness.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

namespace {

std::string trim(const std::string &value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void printUsage() {
  std::cout << "Usage: firmius_audit --audit context_budget "
               "[--cwd <path>] [--prompt-file <path>] [--prompt <text>] "
               "[--persona <name>] [--provider <id>] [--model <id>] "
               "[--variant <name>]\n";
}

} // namespace

std::string ContextBudgetAudit::getId() const { return "context_budget"; }

std::string ContextBudgetAudit::getDescription() const {
  return "Run a real Firmius agent on a workspace/prompt and print context budgeting";
}

shared::AuditResult ContextBudgetAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();
  EnvLoader::load(".env.local");

  std::string cwd = std::filesystem::current_path().string();
  std::string persona = "lead";
  std::string providerId;
  std::string modelId;
  std::string modelVariant;
  std::string prompt;
  std::string promptFile;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto &arg = args[i];
    if (arg == "--cwd" && i + 1 < args.size()) {
      cwd = args[++i];
    } else if (arg == "--persona" && i + 1 < args.size()) {
      persona = args[++i];
    } else if (arg == "--provider" && i + 1 < args.size()) {
      providerId = args[++i];
    } else if (arg == "--model" && i + 1 < args.size()) {
      modelId = args[++i];
    } else if (arg == "--variant" && i + 1 < args.size()) {
      modelVariant = args[++i];
    } else if (arg == "--prompt-file" && i + 1 < args.size()) {
      promptFile = args[++i];
    } else if (arg == "--prompt" && i + 1 < args.size()) {
      prompt = args[++i];
    }
  }

  if (!promptFile.empty()) {
    prompt = readFile(promptFile);
  }

  if (trim(prompt).empty()) {
    printUsage();
    result.exitCode = 1;
    result.passed = false;
    result.output = "Missing prompt";
    return result;
  }

  BenchmarkConfig config;
  config.hostOptions.type = HostType::Local;
  config.cwd = cwd;
  config.personaName = persona;
  config.providerId = providerId;
  config.modelId = modelId;
  config.modelVariant = modelVariant;
  config.initializeHarness = true;
  config.logCallback = [](const std::string &message) {
    std::cout << "[context-budget] " << message << "\n";
  };

  BenchmarkSession session(config);
  auto outcome = session.runAgentTask(prompt);
  auto &agent = session.getAgent();
  const auto &ctx = agent.getContext();
  const auto &metrics = ctx.aggregateMetrics;

  std::ostringstream out;
  out << "Thread: " << ctx.history->threadId << "\n";
  out << "Agent: " << ctx.identity.id << "\n";
  out << "Persona: " << ctx.config.personaName << "\n";
  out << "Model: " << ctx.config.providerId << "/" << ctx.config.modelId;
  if (!ctx.config.modelVariant.empty()) {
    out << " (" << ctx.config.modelVariant << ")";
  }
  out << "\n";
  out << "Prompt tokens: " << metrics.tokens.prompt << "\n";
  out << "Completion tokens: " << metrics.tokens.completion << "\n";
  out << "Reasoning tokens: " << metrics.tokens.reasoning << "\n";
  out << "Total billed tokens: " << metrics.tokens.total << "\n";
  out << "Estimated cost: $" << std::fixed << std::setprecision(4)
      << metrics.estimatedCostUsd << "\n";
  out << "Context: "
      << firmius::core::summarizeContextWindowMetrics(metrics.context, 6)
      << "\n";
  out << "Outcome kind: " << static_cast<int>(outcome.kind) << "\n";
  out << "Outcome text:\n" << outcome.text << "\n";

  std::cout << out.str() << std::flush;
  result.output = out.str();
  result.exitCode = 0;
  result.passed = true;

  Harness::instance().shutdown();
  return result;
}

} // namespace firmius::audits
