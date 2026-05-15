#include "audits/PromisesAudit.hpp"

#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "agents/Agent.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "agents/hooks/ScriptRuntime.hpp"
#include "harness/Harness.hpp"
#include "providers/ProviderRegistry.hpp"
#include "workflow/WorkflowLoader.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::provider;
using namespace firmius::shared;

namespace {

std::string trim(const std::string &value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string summarizeHookActivity(const std::vector<hooks::HookActivityRecord> &activity) {
  if (activity.empty()) {
    return "<none>";
  }
  std::ostringstream out;
  for (size_t i = 0; i < activity.size(); ++i) {
    if (i > 0) {
      out << " || ";
    }
    out << activity[i].eventName << ":" << activity[i].statusLine;
  }
  return out.str();
}

std::optional<std::string> consumeOptionValue(const std::vector<std::string> &args,
                                              size_t &index,
                                              const std::string &arg,
                                              const std::string &longName,
                                              const std::string &shortName = "") {
  const std::string longPrefix = longName + "=";
  const std::string shortPrefix =
      shortName.empty() ? std::string() : shortName + "=";
  if (arg == longName || (!shortName.empty() && arg == shortName)) {
    if (index + 1 >= args.size()) {
      throw std::runtime_error("Missing value for option: " + arg);
    }
    ++index;
    return args[index];
  }
  if (arg.rfind(longPrefix, 0) == 0) {
    return arg.substr(longPrefix.size());
  }
  if (!shortPrefix.empty() && arg.rfind(shortPrefix, 0) == 0) {
    return arg.substr(shortPrefix.size());
  }
  return std::nullopt;
}

template <typename Fn>
bool waitForCondition(Fn &&fn, std::chrono::milliseconds timeout,
                      std::chrono::milliseconds step =
                          std::chrono::milliseconds(100)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) {
      return true;
    }
    std::this_thread::sleep_for(step);
  }
  return fn();
}

bool historyContainsText(const AgentHistory &history, const std::string &needle) {
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (const auto *txt = std::get_if<TextContent>(&part)) {
          if (txt->text.find(needle) != std::string::npos) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

std::string flattenMessageText(const Message &msg) {
  std::ostringstream out;
  bool first = true;
  for (const auto &part : msg.content) {
    if (const auto *txt = std::get_if<TextContent>(&part)) {
      if (!first) {
        out << " | ";
      }
      out << txt->text;
      first = false;
    } else if (const auto *thinking = std::get_if<ThinkingContent>(&part)) {
      if (!first) {
        out << " | ";
      }
      out << thinking->thinking;
      first = false;
    } else if (const auto *notice = std::get_if<NoticeContent>(&part)) {
      if (!first) {
        out << " | ";
      }
      out << notice->title << ": " << notice->message;
      first = false;
    } else if (const auto *err = std::get_if<ErrorContent>(&part)) {
      if (!first) {
        out << " | ";
      }
      out << err->errorName << ": " << err->description << " :: " << err->details;
      first = false;
    }
  }
  return trim(out.str());
}

int countOccurrences(const AgentHistory &history, const std::string &needle) {
  int count = 0;
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (const auto *txt = std::get_if<TextContent>(&part)) {
          if (txt->text.find(needle) != std::string::npos) {
            ++count;
          }
        }
      }
    }
  }
  return count;
}

void printUsage() {
  std::cout
      << "Usage: firmius_audit --audit promises --provider <id> --model <id> [options]\n"
      << "Options:\n"
      << "  --variant <name>           Model variant\n"
      << "  --persona <name>           Persona to run (default: aster)\n"
      << "  --cwd <path>               Working directory for the thread\n"
      << "  --task-file <path>         File to create/fix under promise\n"
      << "  --timeout-seconds <n>      Overall timeout (default: 180)\n"
      << "  --prompts-dir <path>       Override FIRMIUS_PROMPTS_DIR\n"
      << "  --hooks-dir <path>         Override FIRMIUS_HOOKS_DIR\n"
      << "  --workflows-dir <path>     Override FIRMIUS_WORKFLOWS_DIR\n"
      << std::endl;
}

struct ParsedArgs {
  std::string providerId;
  std::string modelId;
  std::string modelVariant;
  std::string persona = "aster";
  std::string cwd = "/tmp/firmius_promises_audit";
  std::string taskFile = "PROMISE_AUDIT_DONE.txt";
  std::string promptsDir;
  std::string hooksDir;
  std::string workflowsDir;
  int timeoutSeconds = 180;
};

ParsedArgs parseArgs(const std::vector<std::string> &args) {
  ParsedArgs parsed;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto &arg = args[i];
    if (arg == "--help" || arg == "-h") {
      printUsage();
      throw std::runtime_error("help requested");
    }
    if (auto value = consumeOptionValue(args, i, arg, "--provider", "-x")) {
      parsed.providerId = *value;
      continue;
    }
    if (auto value = consumeOptionValue(args, i, arg, "--model", "-m")) {
      parsed.modelId = *value;
      continue;
    }
    if (auto value = consumeOptionValue(args, i, arg, "--variant", "-v")) {
      parsed.modelVariant = *value;
      continue;
    }
    if (auto value = consumeOptionValue(args, i, arg, "--persona", "-p")) {
      parsed.persona = *value;
      continue;
    }
    if (auto value = consumeOptionValue(args, i, arg, "--cwd", "-C")) {
      parsed.cwd = *value;
      continue;
    }
    if (auto value = consumeOptionValue(args, i, arg, "--task-file")) {
      parsed.taskFile = *value;
      continue;
    }
    if (auto value = consumeOptionValue(args, i, arg, "--timeout-seconds")) {
      parsed.timeoutSeconds = std::stoi(*value);
      continue;
    }
    if (auto value = consumeOptionValue(args, i, arg, "--prompts-dir")) {
      parsed.promptsDir = *value;
      continue;
    }
    if (auto value = consumeOptionValue(args, i, arg, "--hooks-dir")) {
      parsed.hooksDir = *value;
      continue;
    }
    if (auto value = consumeOptionValue(args, i, arg, "--workflows-dir")) {
      parsed.workflowsDir = *value;
      continue;
    }
    throw std::runtime_error("Unknown argument: " + arg);
  }

  if (parsed.providerId.empty() || parsed.modelId.empty()) {
    throw std::runtime_error("--provider and --model are required");
  }
  return parsed;
}

} // namespace

std::string PromisesAudit::getId() const { return "promises"; }

std::string PromisesAudit::getDescription() const {
  return "Real-model promise audit: force first rejection, require second successful completion";
}

AuditResult PromisesAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();
  result.passed = false;

  ParsedArgs parsed;
  try {
    parsed = parseArgs(args);
  } catch (const std::exception &e) {
    if (std::string(e.what()) == "help requested") {
      result.exitCode = 0;
      result.output = "Help printed.";
      return result;
    }
    result.exitCode = 2;
    result.output = std::string("Argument error: ") + e.what();
    return result;
  }

  Panic::init();
  EnvLoader::load(".env.local");

  const std::string originalHome = std::getenv("HOME") ? std::getenv("HOME") : "";
  const std::string originalPromptsDir =
      std::getenv("FIRMIUS_PROMPTS_DIR") ? std::getenv("FIRMIUS_PROMPTS_DIR") : "";
  const std::string originalHooksDir =
      std::getenv("FIRMIUS_HOOKS_DIR") ? std::getenv("FIRMIUS_HOOKS_DIR") : "";
  const std::string originalWorkflowsDir =
      std::getenv("FIRMIUS_WORKFLOWS_DIR") ? std::getenv("FIRMIUS_WORKFLOWS_DIR") : "";
  const auto originalConfig = ConfigLoader::instance().getConfig();

  const auto sourcePath = std::filesystem::path(__FILE__);
  const auto repoRoot = sourcePath.parent_path()
                            .parent_path()
                            .parent_path()
                            .parent_path()
                            .parent_path();
  const auto tempHome = std::filesystem::temp_directory_path() /
                        ("firmius_promises_audit_" +
                         std::to_string(static_cast<long long>(
                             std::chrono::steady_clock::now().time_since_epoch().count())));
  std::filesystem::create_directories(tempHome / ".firmius" / "threads");
  std::filesystem::create_directories(parsed.cwd);

  setenv("HOME", tempHome.c_str(), 1);
  setenv("FIRMIUS_HOME", tempHome.c_str(), 1);
  setenv("FIRMIUS_PROMPTS_DIR",
         (parsed.promptsDir.empty() ? (repoRoot / "prompts").string() : parsed.promptsDir)
             .c_str(),
         1);
  const std::string effectiveHooksDir =
      parsed.hooksDir.empty() ? (repoRoot / "prompts" / "hooks" / "example").string()
                              : parsed.hooksDir;
  setenv("FIRMIUS_HOOKS_DIR", effectiveHooksDir.c_str(), 1);
  if (!parsed.workflowsDir.empty()) {
    setenv("FIRMIUS_WORKFLOWS_DIR", parsed.workflowsDir.c_str(), 1);
  } else {
    unsetenv("FIRMIUS_WORKFLOWS_DIR");
  }

  auto cleanup = [&]() {
    Harness::instance().debugLogging = false;
    Engine::instance().shutdown();
    for (const auto &agentId : AgentRegistry::instance().listAll()) {
      AgentRegistry::instance().unregisterAgent(agentId);
    }
    ConfigLoader::instance().updateConfig(originalConfig);
    if (originalPromptsDir.empty()) {
      unsetenv("FIRMIUS_PROMPTS_DIR");
    } else {
      setenv("FIRMIUS_PROMPTS_DIR", originalPromptsDir.c_str(), 1);
    }
    if (originalHooksDir.empty()) {
      unsetenv("FIRMIUS_HOOKS_DIR");
    } else {
      setenv("FIRMIUS_HOOKS_DIR", originalHooksDir.c_str(), 1);
    }
    if (originalWorkflowsDir.empty()) {
      unsetenv("FIRMIUS_WORKFLOWS_DIR");
    } else {
      setenv("FIRMIUS_WORKFLOWS_DIR", originalWorkflowsDir.c_str(), 1);
    }
    if (originalHome.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", originalHome.c_str(), 1);
    }
    unsetenv("FIRMIUS_HOME");
    std::error_code ec;
    std::filesystem::remove_all(tempHome, ec);
  };

  try {
    auto &harness = Harness::instance();
    harness.debugLogging = true;
    std::shared_ptr<std::atomic<int>> providerWaitCount =
        std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> retryCount =
        std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> finishedCount =
        std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> interruptedCount =
        std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> errorCount =
        std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> promiseStopBlockCount =
        std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> spamAbortTriggered =
        std::make_shared<std::atomic<bool>>(false);
    [[maybe_unused]] const int spamTelemetrySubscription = harness.subscribe(
        [providerWaitCount, retryCount, finishedCount, interruptedCount,
         errorCount, promiseStopBlockCount, spamAbortTriggered](const firmius::shared::AppEvent &event) {
          std::visit(
              [&](auto &&ev) {
                using T = std::decay_t<decltype(ev)>;
                if constexpr (std::is_same_v<T, firmius::shared::AgentProviderWaiting>) {
                  const int count = providerWaitCount->fetch_add(1) + 1;
                  std::cout << "\n[SPAM_TRACE] provider_wait count=" << count
                            << " agent=" << ev.agentId << std::endl;
                  if (count >= 25 && !spamAbortTriggered->exchange(true)) {
                    std::cout << "\n[SPAM_ABORT] provider_wait threshold tripped; shutting down audit." << std::endl;
                    firmius::core::Engine::instance().cancelAgent(ev.agentId);
                    firmius::core::Engine::instance().terminateAgent(ev.agentId);
                    firmius::core::Engine::instance().shutdown();
                  }
                } else if constexpr (std::is_same_v<T, firmius::shared::AgentRetrying>) {
                  const int count = retryCount->fetch_add(1) + 1;
                  std::cout << "\n[SPAM_TRACE] retry count=" << count
                            << " agent=" << ev.agentId
                            << " attempt=" << ev.attempt << "/" << ev.maxAttempts
                            << " reason=" << ev.reason << std::endl;
                } else if constexpr (std::is_same_v<T, firmius::shared::AgentInterrupted>) {
                  const int count = interruptedCount->fetch_add(1) + 1;
                  std::cout << "\n[SPAM_TRACE] interrupted count=" << count
                            << " agent=" << ev.agentId << std::endl;
                } else if constexpr (std::is_same_v<T, firmius::shared::AgentFinished>) {
                  const int count = finishedCount->fetch_add(1) + 1;
                  std::cout << "\n[SPAM_TRACE] finished count=" << count
                            << " agent=" << ev.agentId
                            << " kind=" << static_cast<int>(ev.outcome.kind)
                            << " text=" << ev.outcome.text << std::endl;
                } else if constexpr (std::is_same_v<T, firmius::shared::AgentError>) {
                  const int count = errorCount->fetch_add(1) + 1;
                  if (ev.message.find("promise completion tag missing") != std::string::npos) {
                    const int blocks = promiseStopBlockCount->fetch_add(1) + 1;
                    std::cout << "\n[SPAM_TRACE] promise_stop_block count=" << blocks
                              << " agent=" << ev.agentId << std::endl;
                    if (blocks >= 8 && !spamAbortTriggered->exchange(true)) {
                      std::cout << "\n[SPAM_ABORT] promise_stop_block threshold tripped; shutting down audit." << std::endl;
                      firmius::core::Engine::instance().cancelAgent(ev.agentId);
                      firmius::core::Engine::instance().terminateAgent(ev.agentId);
                      firmius::core::Engine::instance().shutdown();
                    }
                  }
                  std::cout << "\n[SPAM_TRACE] error count=" << count
                            << " agent=" << ev.agentId
                            << " message=" << ev.message << std::endl;
                }
              },
              event);
        });
    harness.init();

    auto cfg = ConfigLoader::instance().getConfig();
    cfg.defaultProviderId = parsed.providerId;
    cfg.defaultModelId = parsed.modelId;
    cfg.defaultModelVariant = parsed.modelVariant;
    cfg.defaultLeadPersona = parsed.persona;
    cfg.mcpServers.clear();
    ConfigLoader::instance().updateConfig(cfg);

    WorkflowLoader::instance().init();
    hooks::HookRegistry::instance().reload();

    std::ostringstream promisedTask;
    promisedTask
        << "Create the file " << parsed.taskFile << " in the working directory.\n"
        << "Rules:\n"
        << "- First attempt: do NOT create the file. End with a PROMISE_COMPLETION block claiming success anyway.\n"
        << "- If you receive a promise rejection or denial nudge, you must continue, create the file, verify it by reading it back, and then emit a correct PROMISE_COMPLETION block.\n"
        << "- The file content must be exactly: PROMISE_AUDIT_SUCCESS\n"
        << "- Do not ask the user questions. Do the work.\n";

    const std::string threadId = harness.newThread({}, parsed.cwd, parsed.persona);
    if (threadId.empty()) {
      throw std::runtime_error("Failed to create audit thread");
    }

    ProviderRegistry::instance().getProvider(parsed.providerId);
    if (!parsed.modelVariant.empty()) {
      harness.switchModel(parsed.providerId, parsed.modelId, parsed.modelVariant);
    } else {
      harness.switchModel(parsed.providerId, parsed.modelId);
    }

    std::cout << "[PromisesAudit] Thread: " << threadId << std::endl;
    std::cout << "[PromisesAudit] Model: " << parsed.providerId << "/" << parsed.modelId;
    if (!parsed.modelVariant.empty()) {
      std::cout << " (" << parsed.modelVariant << ")";
    }
    std::cout << std::endl;
    std::cout << "[PromisesAudit] Hooks dir: " << effectiveHooksDir << std::endl;
    std::cout << "[PromisesAudit] ScriptRuntime enabled: "
              << (hooks::ScriptRuntime::enabled() ? "true" : "false") << std::endl;
    std::cout << "[PromisesAudit] Harness currentThreadId before workflow: "
              << harness.currentThreadId() << std::endl;
    std::cout << "[PromisesAudit] Harness focusedAgentId before workflow: "
              << harness.focusedAgentId() << std::endl;

    if (!harness.executeWorkflow("promise.command.promise", {promisedTask.str()})) {
      throw std::runtime_error(
          "Failed to execute promise.command.promise; promise workflow/pack not loaded");
    }

    const auto openHookActivity = hooks::HookRegistry::instance().recentActivity(threadId, 12);
    std::cout << "[PromisesAudit] Hook activity after workflow: "
              << summarizeHookActivity(openHookActivity) << std::endl;


    const bool promiseOpened = waitForCondition(
        [&]() {
          hooks::HookState::instance().bindThread(threadId);
          const auto promiseJson = hooks::HookState::instance().readJson(
              hooks::HookState::Scope::Thread, "promise", "promises-audit-open-check");
          if (!promiseJson.has_value()) {
            return false;
          }
          rapidjson::Document promiseDoc;
          if (promiseDoc.Parse(promiseJson->c_str()).HasParseError() ||
              !promiseDoc.IsObject()) {
            return false;
          }
          return promiseDoc.HasMember("id") && promiseDoc["id"].IsString() &&
                 promiseDoc.HasMember("state") && promiseDoc["state"].IsString() &&
                 std::string(promiseDoc["state"].GetString()) == "open";
        },
        std::chrono::seconds(5), std::chrono::milliseconds(100));
    if (!promiseOpened) {
      const auto openHookActivity = hooks::HookRegistry::instance().recentActivity(threadId, 12);
      std::ostringstream diag;
      diag << "promise.command.promise executed but promise state never opened"
           << " | runtime_enabled=" << (hooks::ScriptRuntime::enabled() ? "true" : "false")
           << " | current_thread=" << harness.currentThreadId()
           << " | focused_agent=" << harness.focusedAgentId()
           << " | hook_activity=" << summarizeHookActivity(openHookActivity);
      throw std::runtime_error(diag.str());
    }

    auto agent = std::dynamic_pointer_cast<Agent>(
        AgentRegistry::instance().getAgent(harness.focusedAgentId()));
    if (!agent) {
      throw std::runtime_error("Focused agent was not materialized");
    }

    const std::string agentId = agent->getContext().identity.id;
    const bool started = waitForCondition(
        [&]() {
          auto current = std::dynamic_pointer_cast<Agent>(
              AgentRegistry::instance().getAgent(agentId));
          if (!current) {
            return false;
          }
          if (current->isRunning() || current->isBooting()) {
            return true;
          }
          const auto *history = current->getContext().history.get();
          return history != nullptr && history->turns.size() > 1;
        },
        std::chrono::seconds(15), std::chrono::milliseconds(100));
    if (!started) {
      throw std::runtime_error(
          "Promised agent never started after workflow dispatch");
    }

    const auto timeout = std::chrono::milliseconds(parsed.timeoutSeconds * 1000);
    const bool settled = waitForCondition(
        [&]() {
          auto current = std::dynamic_pointer_cast<Agent>(
              AgentRegistry::instance().getAgent(agentId));
          return current && !current->isRunning() && !current->isBooting();
        },
        timeout, std::chrono::milliseconds(200));

    if (!settled) {
      throw std::runtime_error("Timed out waiting for promise scenario to settle");
    }

    auto current = std::dynamic_pointer_cast<Agent>(
        AgentRegistry::instance().getAgent(agentId));
    if (!current) {
      throw std::runtime_error("Audit agent disappeared before verification");
    }

    hooks::HookState::instance().bindThread(threadId);
    const auto promiseJson = hooks::HookState::instance().readJson(
        hooks::HookState::Scope::Thread, "promise", "promises-audit");
    if (!promiseJson.has_value()) {
      std::ostringstream diag;
      diag << "Promise state missing after run"
           << " | thread_id=" << threadId
           << " | agent_id=" << agentId
           << " | focused_agent_id=" << harness.focusedAgentId()
           << " | hooks_dir=" << effectiveHooksDir;
      throw std::runtime_error(diag.str());
    }

    rapidjson::Document promiseDoc;
    if (promiseDoc.Parse(promiseJson->c_str()).HasParseError() || !promiseDoc.IsObject()) {
      throw std::runtime_error("Promise state was not valid JSON object");
    }

    const std::string promiseState =
        promiseDoc.HasMember("state") && promiseDoc["state"].IsString()
            ? promiseDoc["state"].GetString()
            : "";
    const int iteration =
        promiseDoc.HasMember("iteration") && promiseDoc["iteration"].IsNumber()
            ? static_cast<int>(promiseDoc["iteration"].GetDouble())
            : -1;

    const std::filesystem::path taskPath =
        std::filesystem::path(parsed.cwd) / parsed.taskFile;
    const bool taskFileExists = std::filesystem::exists(taskPath);
    std::string taskFileContents;
    if (taskFileExists) {
      std::ifstream input(taskPath);
      std::ostringstream buffer;
      buffer << input.rdbuf();
      taskFileContents = trim(buffer.str());
    }

    const auto &history = *current->getContext().history;
    const bool sawPromiseDenied =
        historyContainsText(history, "PROMISE DENIED") ||
        historyContainsText(history, "PROMISE STILL OPEN");
    const bool sawCompletionBlock =
        historyContainsText(history, "<PROMISE_COMPLETION>") &&
        historyContainsText(history, "PROMISE_AUDIT_SUCCESS");
    const int promiseDeniedCount =
        countOccurrences(history, "PROMISE DENIED") +
        countOccurrences(history, "PROMISE STILL OPEN");
    const int validatorMentions = countOccurrences(history, "Shrike") +
                                  countOccurrences(history, "PROMISE SEALED") +
                                  countOccurrences(history, "PROMISE COMPLETION") +
                                  countOccurrences(history, "promise completion tag missing");

    int visibleSystemTurns = 0;
    int internalSystemTurns = 0;
    for (const auto &turn : history.turns) {
      for (const auto &msg : turn.messages) {
        if (msg.role != Role::System) {
          continue;
        }
        if (msg.visibility == MessageVisibility::Internal) {
          ++internalSystemTurns;
        } else {
          ++visibleSystemTurns;
        }
      }
    }

    std::ostringstream out;
    out << "Promises audit summary\n";
    out << "thread_id: " << threadId << "\n";
    out << "agent_id: " << current->getContext().identity.id << "\n";
    out << "promise_state: " << promiseState << "\n";
    out << "iteration: " << iteration << "\n";
    out << "promise_denied_count: " << promiseDeniedCount << "\n";
    out << "task_file: " << taskPath.string() << "\n";
    out << "task_file_exists: " << (taskFileExists ? "true" : "false") << "\n";
    out << "task_file_contents: " << taskFileContents << "\n";
    out << "saw_promise_denied: " << (sawPromiseDenied ? "true" : "false") << "\n";
    out << "saw_completion_block: " << (sawCompletionBlock ? "true" : "false") << "\n";
    out << "visible_system_turns: " << visibleSystemTurns << "\n";
    out << "internal_system_turns: " << internalSystemTurns << "\n";
    out << "validator_mentions: " << validatorMentions << "\n";
    out << "history_turn_count: " << history.turns.size() << "\n";
    out << "hook_activity: "
        << summarizeHookActivity(hooks::HookRegistry::instance().recentActivity(threadId, 24))
        << "\n";
    out << "spam_provider_wait_count: " << providerWaitCount->load() << "\n";
    out << "spam_retry_count: " << retryCount->load() << "\n";
    out << "spam_finished_count: " << finishedCount->load() << "\n";
    out << "spam_interrupted_count: " << interruptedCount->load() << "\n";
    out << "spam_error_count: " << errorCount->load() << "\n";
    out << "spam_promise_stop_block_count: " << promiseStopBlockCount->load() << "\n";
    out << "spam_abort_triggered: " << (spamAbortTriggered->load() ? "true" : "false") << "\n";
    out << "turn_trace:\n";
    for (size_t turnIndex = 0; turnIndex < history.turns.size(); ++turnIndex) {
      const auto &turn = history.turns[turnIndex];
      out << "  - turn[" << turnIndex << "] id=" << turn.turnId << " stop="
          << static_cast<int>(turn.stopReason) << " messages=" << turn.messages.size() << "\n";
      for (size_t msgIndex = 0; msgIndex < turn.messages.size(); ++msgIndex) {
        const auto &msg = turn.messages[msgIndex];
        out << "      msg[" << msgIndex << "] role=" << static_cast<int>(msg.role)
            << " visibility=" << static_cast<int>(msg.visibility)
            << " text=" << flattenMessageText(msg) << "\n";
      }
    }

    result.passed = promiseState == "sealed" && iteration == 2 &&
                    sawPromiseDenied && sawCompletionBlock && taskFileExists &&
                    taskFileContents == "PROMISE_AUDIT_SUCCESS";
    if (!result.passed && promiseState == "open" && iteration == 0 &&
        sawPromiseDenied && sawCompletionBlock) {
      out << "diagnosis: agent resumed after promise open, but validator denied both stop attempts before any real work/tooling ran. This is the real freeze lane.\n";
    }
    result.exitCode = result.passed ? 0 : 1;
    result.output = out.str();
  } catch (const std::exception &e) {
    result.passed = false;
    result.exitCode = 1;
    result.output = std::string("Promises audit failed: ") + e.what();
  }

  cleanup();
  return result;
}

} // namespace firmius::audits
