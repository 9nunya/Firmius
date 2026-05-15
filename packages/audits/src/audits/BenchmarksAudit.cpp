#include "audits/BenchmarksAudit.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "AgentRegistry.hpp"
#include "harness/Harness.hpp"
#include "Panic.hpp"
#include "agents/ContextBudget.hpp"
#include "providers/ProviderRegistry.hpp"
#include "benchmarks/BenchmarkFactory.hpp"
#include "benchmarks/BenchmarkSession.hpp"
#include "utils/Logger.hpp"
#include "utils/ToolSummaries.hpp"
#include "utils/ToolView.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <map>
#include <mutex>

namespace firmius::audits {

using namespace firmius::provider;
using namespace firmius::shared;
using namespace firmius::core;

namespace {

std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

void printUsage() {
  std::cout << "Usage: firmius_audit --audit benchmarks --bench <name> [--provider <id>] [--model <id>] [--variant <v>]\n"
            << "\nExamples:\n"
            << "  firmius_audit --audit benchmarks --bench mbpp --provider antigravity --model gemini-3-flash\n"
            << std::endl;
}

// ANSI color codes
constexpr const char* COLOR_RESET = "\x1b[0m";
constexpr const char* COLOR_BOLD = "\x1b[1m";
constexpr const char* COLOR_DIM = "\x1b[2m";
constexpr const char* COLOR_ITALIC = "\x1b[3m";
constexpr const char* COLOR_RED = "\x1b[31m";
constexpr const char* COLOR_GREEN = "\x1b[32m";
constexpr const char* COLOR_YELLOW = "\x1b[33m";
constexpr const char* COLOR_MAGENTA = "\x1b[35m";
constexpr const char* COLOR_CYAN = "\x1b[36m";
constexpr const char* COLOR_WHITE = "\x1b[37m";
constexpr const char* COLOR_BG_BLUE = "\x1b[44m";

// Pretty CLI Renderer - streams events in real-time.
//
// `onEvent` is invoked from MULTIPLE threads — the main benchmark loop, the
// per-agent run loops, and async provider model-discovery threads (e.g.
// NvidiaProvider::discoverModels emits `ModelDiscovered` from a worker
// thread spawned during Engine init). Without serialisation those calls
// race on `std::cout` and the internal toolCalls map, which is a hard
// SIGSEGV under high event volume. The mutex is recursive because some
// handlers re-enter onEvent indirectly via ensureTurnHeader → AgentRegistry
// queries that internally call back through the harness.
class BenchmarkCLIRenderer {
public:
  void onEvent(const AppEvent& event) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::visit([this](auto&& ev) { handleEvent(ev); }, event);
  }

private:
  std::recursive_mutex mutex_;
  std::map<std::string, ToolCallView> toolCalls;  // Use ToolCallView from shared
  int turnCount = 0;
  bool turnHeaderPrinted = false;
  bool thinkingOpen = false;
  bool textOpen = false;
  uint32_t currentContextSize = 0;

  void ensureTurnHeader(const std::string& agentId, uint32_t contextSize = 0) {
    if (turnHeaderPrinted) return;
    
    auto agent = AgentRegistry::instance().getAgent(agentId);
    std::string displayName = agentId.substr(0, 8);
    std::string modelId;
    if (agent) {
      const auto& ctx = agent->getContext();
      if (!ctx.identity.friendlyName.empty()) {
        displayName = ctx.identity.friendlyName;
      }
      modelId = ctx.config.modelId;
    }

    turnCount++;
    if (contextSize > 0) currentContextSize = contextSize;
    
    std::cout << "\n" << COLOR_BOLD << COLOR_BG_BLUE << COLOR_WHITE;
    std::cout << " ── " << displayName << " │ Turn " << turnCount << " │ CTX: " << currentContextSize;
    if (!modelId.empty()) std::cout << " │ " << modelId;
    std::cout << " ──" << COLOR_RESET << "\n";
    turnHeaderPrinted = true;
  }

  void handleEvent(const AgentThinking& ev) {
    ensureTurnHeader(ev.agentId, currentContextSize);
    if (!thinkingOpen) {
      std::cout << COLOR_DIM << COLOR_ITALIC;
      thinkingOpen = true;
    }
    std::cout << ev.delta << std::flush;
  }

  void handleEvent(const AgentText& ev) {
    ensureTurnHeader(ev.agentId, currentContextSize);
    if (thinkingOpen) {
      std::cout << COLOR_RESET << "\n";
      thinkingOpen = false;
    }
    if (!textOpen) {
      std::cout << COLOR_WHITE;
      textOpen = true;
    }
    std::cout << ev.delta << std::flush;
  }

  void handleEvent(const AgentToolCallChunk& ev) {
    ensureTurnHeader(ev.agentId, currentContextSize);
    
    auto& view = toolCalls[ev.toolCallId];
    view.toolCallId = ev.toolCallId;
    view.agentId = ev.agentId;
    
    if (view.name.empty() && !ev.nameDelta.empty()) {
      view.name = ev.nameDelta;
      view.phase = ToolPhase::Preparing;
      std::cout << COLOR_RESET << "\n" << COLOR_CYAN << "  → " << COLOR_BOLD 
                << view.name << COLOR_DIM << "(...) " << COLOR_RESET << std::flush;
    }
    
    view.args += ev.argsDelta;
    
  }

  void handleEvent(const AgentToolCall& ev) {
    ensureTurnHeader(ev.agentId, currentContextSize);
    
    auto& view = toolCalls[ev.toolCallId];
    view.toolCallId = ev.toolCallId;
    view.agentId = ev.agentId;
    
    if (view.name.empty()) {
      view.name = ev.toolName;
      view.phase = ToolPhase::Preparing;
      std::cout << COLOR_RESET << "\n" << COLOR_CYAN << "  → " << COLOR_BOLD 
                << view.name << COLOR_DIM << "(...) " << COLOR_RESET << std::flush;
    }
    
    if (!ev.toolArgs.empty()) {
      view.args = ev.toolArgs;
    }
    const bool transitioned = view.phase == ToolPhase::Preparing;
    view.phase = ToolPhase::Called;
    std::string summary =
        SummarizeToolCall(view.name, view.args, ToolPhase::Called);
    if (transitioned) {
      std::cout << "\r" << COLOR_CYAN << "  → " << COLOR_GREEN << summary
                << COLOR_RESET << "\n";
    }
  }

  void handleEvent(const AgentTurnCompleted& ev) {
    if (ev.aggregateMetrics.tokens.contextSize > 0) {
      currentContextSize = ev.aggregateMetrics.tokens.contextSize;
    }
    
    ensureTurnHeader(ev.agentId, currentContextSize);
    
    // Close any open styling
    if (thinkingOpen || textOpen) {
      std::cout << COLOR_RESET;
      thinkingOpen = false;
      textOpen = false;
    }

    const std::string contextSummary =
        summarizeContextWindowMetrics(ev.aggregateMetrics.context, 3);
    if (!contextSummary.empty() && contextSummary != "sent=0") {
      std::cout << COLOR_DIM << "  context> " << contextSummary << COLOR_RESET
                << "\n";
    }
    
    // Print tool results
    for (const auto& msg : ev.turn.messages) {
      if (msg.role == Role::ToolResult) {
        for (const auto& content : msg.content) {
          if (auto* res = std::get_if<ToolResultContent>(&content)) {
            auto it = toolCalls.find(res->toolCallId);
            if (it != toolCalls.end()) {
              it->second.phase = ToolPhase::Finished;
              it->second.success = res->success;
              it->second.result = res->result;
              
              if (res->success) {
                printToolResult(it->second);
              } else {
                std::cout << COLOR_RED << "  ✗ [" << res->toolCallId << "] " 
                          << trimPreview(res->result, 120) << COLOR_RESET << "\n";
              }
            }
          }
        }
      }
    }
    
    // Reset for next turn
    turnHeaderPrinted = false;
    
    // Clear finished tool calls
    for (auto it = toolCalls.begin(); it != toolCalls.end();) {
      if (it->second.phase == ToolPhase::Finished) {
        it = toolCalls.erase(it);
      } else {
        ++it;
      }
    }
  }

  void handleEvent(const AgentRetrying& ev) {
    std::cout << "\n" << COLOR_YELLOW << "  ⚠ Retry " << ev.accountLocator 
              << " attempt " << ev.attempt << "/" << ev.maxAttempts 
              << ": " << ev.reason << COLOR_RESET << "\n";
  }

  void handleEvent(const AgentRetryFailed& ev) {
    std::cout << "\n" << COLOR_RED << "  ✖ Retry failed: " << ev.reason << COLOR_RESET << "\n";
  }

  void handleEvent(const AgentAccountSwitched& ev) {
    std::cout << "\n" << COLOR_MAGENTA << "  ⇄ Account switched: " << ev.accountLocator << COLOR_RESET << "\n";
  }

  void handleEvent(const AgentError& ev) {
    std::cout << "\n" << COLOR_RED << "  ! Error: " << ev.message << COLOR_RESET << "\n";
  }

  void handleEvent(const AgentProcessSpawned& ev) {
    std::cout << "\n" << COLOR_CYAN << "  ⚙ Process spawned: " << ev.command << COLOR_RESET << "\n";
  }

  void handleEvent(const AgentProcessOutput& ev) {
    if (ev.finished) {
      std::cout << COLOR_DIM << "  Process " << ev.processId << " exited with code " << ev.exitCode 
                << " (" << std::fixed << std::setprecision(0) << ev.durationMs << "ms)" << COLOR_RESET << "\n";
    } else if (!ev.output.empty()) {
      std::cout << ev.output << std::flush;
    }
  }

  void handleEvent(const AgentSpawned& ev) {
    std::cout << "\n" << COLOR_BOLD << COLOR_MAGENTA << "  ┌─ Subagent spawned: " << ev.friendlyName 
              << " (ID: " << ev.agentId.substr(0, 8) << ") ──┐" << COLOR_RESET << "\n";
  }

  void handleEvent(const AgentFinished& ev) {
    std::string outcomeStr;
    switch (ev.outcome.kind) {
      case AgentOutcome::Kind::Response: outcomeStr = "Response"; break;
      case AgentOutcome::Kind::NoSummary: outcomeStr = "No summary"; break;
      case AgentOutcome::Kind::Cancelled: outcomeStr = "Cancelled"; break;
      case AgentOutcome::Kind::Failed: outcomeStr = "Failed"; break;
    }
    std::cout << COLOR_BOLD << "\n  ══ Agent finished: " << outcomeStr;
    if (!ev.outcome.text.empty()) {
      std::string preview = trim(ev.outcome.text);
      if (preview.size() > 60) preview = preview.substr(0, 57) + "...";
      std::cout << " - " << preview;
    }
    std::cout << COLOR_RESET << "\n";
  }

  void handleEvent(const ContextCompacted& ev) {
    std::cout << "\n" << COLOR_MAGENTA << "  ≡ Context compacted: saved " << ev.tokensSaved << " tokens" << COLOR_RESET << "\n";
  }

  void handleEvent(const ModelSwitched& ev) {
    std::cout << "\n" << COLOR_MAGENTA << "  ~ Model switched: " << ev.oldModelId 
              << " → " << ev.newModelId << COLOR_RESET << "\n";
  }

  void handleEvent(const AgentProviderWaiting&) {
    std::cout << COLOR_DIM << "  ⏳ Waiting for provider..." << COLOR_RESET << "\n";
  }

  template<typename T>
  void handleEvent(const T&) {}

  bool hasJsonBraces(const std::string& s) {
    return s.find('{') != std::string::npos && s.find('}') != std::string::npos;
  }

  std::string trimPreview(const std::string& s, size_t maxLen) {
    std::string t = trim(s);
    return t.size() > maxLen ? t.substr(0, maxLen - 3) + "..." : t;
  }

  void printToolResult(const ToolCallView& view) {
    const std::string& toolName = view.name;
    
    if (toolName == "file_edit") {
      printFileEditResult(view);
      return;
    }
    
    if (toolName == "file_read") {
      std::cout << COLOR_GREEN << "  ✓ [" << view.toolCallId << "] Read file" << COLOR_RESET << "\n";
      return;
    }
    
    std::string preview = trimPreview(view.result, 100);
    std::cout << COLOR_GREEN << "  ✓ [" << view.toolCallId << "] " << preview << COLOR_RESET << "\n";
  }

  void printFileEditResult(const ToolCallView& view) {
    const std::string& result = view.result;
    
    bool isOverwrite = result.find("\"content\"") != std::string::npos;
    bool hasEdits = result.find("\"edits\"") != std::string::npos || 
                    result.find("\"operations\"") != std::string::npos;
    
    std::cout << COLOR_GREEN << "  ✓ [" << view.toolCallId << "] file_edit" << COLOR_RESET << "\n";
    
    std::string path = extractPathFromArgs(view.args);
    if (!path.empty()) {
      std::cout << COLOR_DIM << "    Path: " << path << COLOR_RESET << "\n";
    }
    
    if (isOverwrite) {
      std::cout << COLOR_DIM << "    Mode: overwrite" << COLOR_RESET << "\n";
    } else if (hasEdits) {
      std::cout << COLOR_DIM << "    Mode: LineRange edits" << COLOR_RESET << "\n";
    }
  }

  std::string extractPathFromArgs(const std::string& argsJson) {
    size_t pos = argsJson.find("\"path\"");
    if (pos == std::string::npos) return "";
    
    size_t colonPos = argsJson.find(':', pos);
    if (colonPos == std::string::npos) return "";
    
    size_t quoteStart = argsJson.find('"', colonPos);
    if (quoteStart == std::string::npos) return "";
    
    size_t quoteEnd = argsJson.find('"', quoteStart + 1);
    if (quoteEnd == std::string::npos) return "";
    
    return argsJson.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
  }
};

} // namespace

std::string BenchmarksAudit::getId() const { return "benchmarks"; }
std::string BenchmarksAudit::getDescription() const {
  return "Run benchmark evaluations (MBPP, SWE-bench, AgentBench)";
}

shared::AuditResult BenchmarksAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();

  std::string benchmarkName;
  std::string providerName = "nanogpt";
  std::string modelId;
  std::string modelVariant;

  for (size_t i = 0; i < args.size(); ++i) {
    std::string arg = args[i];
    if (arg == "--bench" && i + 1 < args.size()) {
      benchmarkName = trim(args[++i]);
    } else if (arg == "--provider" && i + 1 < args.size()) {
      providerName = trim(args[++i]);
    } else if (arg == "--model" && i + 1 < args.size()) {
      modelId = trim(args[++i]);
    } else if (arg == "--variant" && i + 1 < args.size()) {
      modelVariant = trim(args[++i]);
    } else if (arg == "--help" || arg == "-h") {
      printUsage();
      result.exitCode = 0;
      result.passed = true;
      return result;
    }
  }

  if (benchmarkName.empty()) {
    Logger::instance().logError("--bench argument is required");
    printUsage();
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  const auto canonicalId = canonicalBenchmarkId(benchmarkName);
  if (!canonicalId.has_value()) {
    Logger::instance().logError("Unknown benchmark: " + benchmarkName);
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  EnvLoader::load(".env.local");
  Panic::init();
  (void)firmius::core::Engine::instance();
  
  auto& harness = Harness::instance();
  harness.init();

  auto provider = ProviderRegistry::instance().getProvider(providerName);
  if (!provider) {
    Logger::instance().logError("Unknown provider: " + providerName);
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  std::cout << "\n" << COLOR_BOLD << COLOR_GREEN;
  std::cout << "╔═══════════════════════════════════════════════════════════╗\n";
  std::cout << "║              BENCHMARK AUDIT RUN                          ║\n";
  std::cout << "╠═══════════════════════════════════════════════════════════╣\n";
  std::cout << "║ Benchmark: " << std::left << std::setw(48) << benchmarkName << "║\n";
  std::cout << "║ Provider:  " << std::left << std::setw(48) << providerName << "║\n";
  if (!modelId.empty()) {
    std::cout << "║ Model:     " << std::left << std::setw(48) << modelId << "║\n";
  }
  if (!modelVariant.empty()) {
    std::cout << "║ Variant:   " << std::left << std::setw(48) << modelVariant << "║\n";
  }
  std::cout << "╚═══════════════════════════════════════════════════════════╝" << COLOR_RESET << "\n\n";

  BenchmarkCLIRenderer renderer;
  int subscriptionId = harness.subscribe([&renderer](const AppEvent& event) {
    renderer.onEvent(event);
  });

  BenchmarkConfig config;
  config.providerId = providerName;
  config.modelId = modelId;
  config.modelVariant = modelVariant;

  // For SWE-family benchmarks, use Docker with volume mount for the repo cache
  if (*canonicalId == "swebench" || *canonicalId == "turingswebenchpp") {
    config.hostOptions.type = HostType::Docker;
    config.hostOptions.deleteOnExit = true;
    std::string home = getenv("HOME") ? getenv("HOME") : "/root";
    std::string cacheDir = home + "/.firmius/cache/" + *canonicalId + "/repos";
    config.hostOptions.volumeMounts.push_back(cacheDir + ":/host_cache");
  }

  config.logCallback = [](const std::string& msg) {
    std::cout << COLOR_DIM << "  [bench] " << msg << COLOR_RESET << "\n";
  };

  Logger::instance().logInfo("Initializing benchmark runner...");
  auto benchmark = makeBenchmark(*canonicalId, config);
  if (!benchmark) {
    Logger::instance().logError("Failed to initialize benchmark: " + *canonicalId);
    harness.unsubscribe(subscriptionId);
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  Logger::instance().logInfo("Loading benchmark tasks...");
  auto tasks = benchmark->listTasks();
  Logger::instance().logInfo("Found " + std::to_string(tasks.size()) + " tasks");

  if (tasks.empty()) {
    Logger::instance().logWarning("No tasks available in benchmark");
    harness.unsubscribe(subscriptionId);
    result.exitCode = 0;
    result.passed = true;
    result.output = "No tasks available";
    return result;
  }

  std::string taskId = tasks[0];
  
  std::cout << COLOR_BOLD << "\n┌─────────────────────────────────────────────────────────┐\n";
  std::cout << "│ TASK: " << std::left << std::setw(54) << taskId << "│\n";
  std::cout << "└─────────────────────────────────────────────────────────┘" << COLOR_RESET << "\n";

  Logger::instance().logInfo("Preparing task: " + taskId);

  if (!benchmark->prepareTask(taskId)) {
    Logger::instance().logError("Failed to prepare task " + taskId);
    harness.unsubscribe(subscriptionId);
    result.exitCode = 1;
    result.passed = false;
    result.output = "Failed to prepare task: " + taskId;
    return result;
  }

  Logger::instance().logInfo("Running task: " + taskId);
  auto taskResult = benchmark->runTask(taskId);

  harness.unsubscribe(subscriptionId);

  std::cout << "\n" << COLOR_BOLD << COLOR_GREEN;
  std::cout << "╔═══════════════════════════════════════════════════════════╗\n";
  std::cout << "║                    TASK RESULT                            ║\n";
  std::cout << "╠═══════════════════════════════════════════════════════════╣\n";
  std::cout << "║ Task ID:   " << std::left << std::setw(48) << taskResult.taskId << "║\n";
  
  std::string passedStr = taskResult.passed ? "YES ✓" : "NO ✗";
  std::string passedColor = taskResult.passed ? COLOR_GREEN : COLOR_RED;
  std::cout << "║ Passed:    " << passedColor << std::left << std::setw(48) << passedStr 
            << COLOR_RESET << COLOR_GREEN << "║\n";
  
  std::cout << "║ Tokens:    " << std::left << std::setw(48) << taskResult.metrics.tokens.total << "║\n";
  std::cout << "║ Prompt:    " << std::left << std::setw(48) << taskResult.metrics.tokens.prompt << "║\n";
  std::cout << "║ Complete:  " << std::left << std::setw(48) << taskResult.metrics.tokens.completion << "║\n";
  std::cout << "║ Sent ctx:  " << std::left << std::setw(48) << taskResult.metrics.context.sentTokens << "║\n";
  std::cout << "║ Raw ctx:   " << std::left << std::setw(48) << taskResult.metrics.context.rawPromptTokens << "║\n";
  std::cout << "╚═══════════════════════════════════════════════════════════╝" << COLOR_RESET << "\n";

  const std::string contextSummary =
      summarizeContextWindowMetrics(taskResult.metrics.context, 4);
  if (!contextSummary.empty() && contextSummary != "sent=0") {
    std::cout << COLOR_DIM << "Context breakdown: " << contextSummary
              << COLOR_RESET << "\n";
  }

  if (!taskResult.output.empty()) {
    std::cout << "\n" << COLOR_BOLD << "Output:" << COLOR_RESET << "\n";
    std::cout << COLOR_DIM << taskResult.output << COLOR_RESET << "\n";
  }

  result.exitCode = taskResult.passed ? 0 : 1;
  result.passed = taskResult.passed;
  result.output = taskResult.output;

  std::cout << COLOR_DIM << "\nBenchmark audit completed.\n" << COLOR_RESET;

  return result;
}

} // namespace firmius::audits
