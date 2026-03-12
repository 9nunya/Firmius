#include "agents/Agent.hpp"
#include "EnvLoader.hpp"
#include "Events.hpp"
#include "Message.hpp"
#include "Panic.hpp"
#include "agents/PurposeLoader.hpp"
#include "persistence/Journaler.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::core {

using namespace firmius::shared;

std::uint64_t Agent::nowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

Agent::Agent(AgentContext ctx, std::unique_ptr<shared::IHost> h,
             ToolRegistry &reg, std::shared_ptr<Journaler> jnl)
    : context(std::move(ctx)), host(std::move(h)), toolRegistry(reg),
      journaler(jnl) {
  if (!context.history) {
    context.history = std::make_shared<AgentHistory>();
  }
  provider = firmius::provider::ProviderRegistry::instance().getProvider(
      context.config.providerId);
  if (!provider)
    throw std::runtime_error("Unknown provider: " + context.config.providerId);

  permissionChecks =
      std::make_unique<firmius::core::AgentPermissionChecks>(context);
}

Agent::~Agent() {
  for (const auto &id : backgroundProcessIds) {
    try {
      host->killBackgroundProcess(id);
    } catch (...) {
    }
  }
  if (host)
    host->destroy();
}

void Agent::reset() {
  context.history->turns.clear();
  context.aggregateMetrics = {};
  context.state = {};
  interrupted = false;
  running = false;

  for (const auto &id : backgroundProcessIds) {
    try {
      host->killBackgroundProcess(id);
    } catch (...) {
    }
  }
  backgroundProcessIds.clear();
}

std::string Agent::resolvePath(const std::string &inputPath) const {
  return shared::FSUtil::resolvePath(inputPath, context.environment.cwd);
}

void Agent::interrupt() { interrupted = true; }

void Agent::setModel(const std::string &providerId,
                     const std::string &modelId) {
  if (running.load()) {
    throw std::runtime_error("Cannot switch model while agent is running");
  }

  auto newProvider =
      firmius::provider::ProviderRegistry::instance().getProvider(providerId);
  if (!newProvider) {
    throw std::runtime_error("Unknown provider: " + providerId);
  }

  context.config.providerId = providerId;
  context.config.modelId = modelId;
  provider = newProvider;
}

void Agent::setModel(const std::string &providerId, const std::string &modelId,
                     const std::string &variantName) {
  if (running.load()) {
    throw std::runtime_error("Cannot switch model while agent is running");
  }

  auto newProvider =
      firmius::provider::ProviderRegistry::instance().getProvider(providerId);
  if (!newProvider) {
    throw std::runtime_error("Unknown provider: " + providerId);
  }

  context.config.providerId = providerId;
  context.config.modelId = modelId;
  context.config.modelVariant = variantName;
  provider = newProvider;
}

std::string Agent::spawnProcess(const std::string &command,
                                const std::string &toolCallId,
                                const std::string &cwd,
                                const std::map<std::string, std::string> &env) {
  std::string id = StringUtil::generateUuid();
  auto proc = host->spawn(command, cwd, env);
  proc->onOutput([this, id](const std::string &output, bool isError) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    if (eventCallback) {
      eventCallback(ProcessOutputDelta{id, output, isError, false});
    }
  });
  host->registerBackgroundProcess(id, std::move(proc));
  backgroundProcessIds.insert(id);
  emitProcessSpawned(id, toolCallId, command);
  return id;
}

shared::ProcessSnapshot Agent::inspectProcess(const std::string &id) {
  return host->inspectBackgroundProcess(id);
}

void Agent::writeToProcess(const std::string &id, const std::string &data) {
  host->writeToBackgroundProcess(id, data);
}

void Agent::registerProcessId(const std::string &id) {
  backgroundProcessIds.insert(id);
}

void Agent::emitProcessSpawned(const std::string &processId,
                               const std::string &toolCallId,
                               const std::string &command) {
  std::lock_guard<std::mutex> lock(callbackMutex);
  if (eventCallback) {
    eventCallback(shared::StreamEvent(
        AgentProcessSpawned{context.identity.id, processId, toolCallId, command,
                            context.identity.parentId}));
  }
}

void Agent::addBlockingProcessId(const std::string &id) {
  std::lock_guard<std::mutex> lock(blockingProcessMutex);
  context.state.blockingProcessIds.push_back(id);
}

void Agent::removeBlockingProcessId(const std::string &id) {
  std::lock_guard<std::mutex> lock(blockingProcessMutex);
  auto &vec = context.state.blockingProcessIds;
  vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
}

std::vector<std::string> Agent::getBlockingProcessIds() {
  std::lock_guard<std::mutex> lock(blockingProcessMutex);
  return context.state.blockingProcessIds; // return copy
}

bool Agent::hasReadFile(const std::string &path) const {
  return std::find(context.state.readFiles.begin(),
                   context.state.readFiles.end(),
                   path) != context.state.readFiles.end();
}

void Agent::markFileAsRead(const std::string &path) {
  if (!hasReadFile(path)) {
    context.state.readFiles.push_back(path);
  }
}

void Agent::run(const std::string &task,
                std::function<void(const shared::StreamEvent &)> onEvent) {
  {
    std::lock_guard<std::mutex> lock(callbackMutex);
    eventCallback = onEvent;
  }

  // Guard against concurrent runs with mutex
  std::lock_guard<std::mutex> lock(runMutex);
  if (running.load()) {
    throw std::runtime_error("Agent is already running");
  }
  running = true;
  booting = false;
  interrupted = false;
  context.state.currentStatus = AgentStatus::Idle;
  context.state.fatalError = std::nullopt;

  // 1. Bootstrap System Message
  if (context.history->turns.empty()) {
    auto toolDefs =
        toolRegistry.getAvailableToolDefinitions(context.permissions);
    std::string personaName = context.config.personaName.empty()
                                  ? "coder"
                                  : context.config.personaName;
    Persona persona = PurposeLoader::load(personaName);
    std::string toolBlock = PurposeLoader::buildToolsBlock(toolDefs);

    std::string protocolAddon =
        "\n\n# PROTOCOL STRICTNESS\n"
        "- If you are calling a tool, your message MUST contain ONLY the tool "
        "call JSON.\n"
        "- Do NOT include any other text or thinking when calling a tool.\n"
        "- If you emit any non-tool content, it is treated as your final "
        "response; do not include tool calls in that turn.\n";

    std::string systemPrompt =
        PurposeLoader::composeSystemPrompt(persona, context, toolBlock) +
        protocolAddon;

    AgentTurn turn;
    turn.turnId = "bootstrap-system";
    Message msg;
    msg.role = Role::System;
    msg.content.push_back(TextContent{systemPrompt});
    auto now = std::chrono::system_clock::now();
    msg.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
    turn.messages.push_back(msg);
    context.history->turns.push_back(turn);
    if (context.config.persistHistory && journaler)
      journaler->appendTurn(turn);
  }

  // 2. Add Task as User Message
  // Always append the new task as a User message turn (for re-tasking support)
  AgentTurn taskTurn;
  taskTurn.turnId =
      "user-task-" + std::to_string(context.history->turns.size());
  Message taskMsg;
  taskMsg.role = Role::User;
  taskMsg.content.push_back(TextContent{task});
  auto now = std::chrono::system_clock::now();
  taskMsg.timestamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count());
  taskTurn.messages.push_back(taskMsg);
  context.history->turns.push_back(taskTurn);
  if (context.config.persistHistory && journaler)
    journaler->appendTurn(taskTurn);

  // 3. Autonomous Loop
  bool taskFinished = false;
  int maxTurns = context.config.maxTurns > 0 ? context.config.maxTurns : 200;
  int turnCount = 0;

  int consecutiveProviderFailures = 0;
  const int maxProviderRetries = 3;

  while (!taskFinished && turnCount < maxTurns && !interrupted.load()) {
    // --- CHECK FOR CONTEXT COMPACTION ---
    try {
      auto model = provider->getModelInfo(context.config.modelId);
      bool forceCompact = (std::getenv("FORCE_COMPACTION") != nullptr);
      if (forceCompact || (model.contextWindow > 0 &&
                           context.aggregateMetrics.tokens.contextSize >
                               model.contextWindow * 0.8)) {
        compactContext(onEvent);
      }
      if (interrupted.load())
        break;
    } catch (...) {
      // Compaction is best-effort
    }

    turnCount++;

    try {
      // --- State: ProviderWaiting ---
      context.state.currentStatus = AgentStatus::ProviderWaiting;
      onEvent(ProviderWaiting{});

      firmius::provider::ProviderOptions opts;
      opts.modelId = context.config.modelId;
      try {
        auto modelInfo = provider->getModelInfo(context.config.modelId);
        for (const auto &v : modelInfo.variants) {
          if (v.variantName == context.config.modelVariant) {
            opts.modelVariantJson = v.extraMetadataJson;
            break;
          }
        }
      } catch (...) {
      }
      opts.temperature = context.config.temperature;
      if (context.config.maxTokens.has_value()) {
        opts.maxTokens = context.config.maxTokens;
      }
      opts.stop = context.config.stop;
      opts.tools =
          toolRegistry.getAvailableToolDefinitions(context.permissions);
      opts.abortSignal = &interrupted;

      std::vector<ToolCallChunk> accumulatedToolChunks;
      std::string fullResponse;
      std::string fullThinking;
      std::string lastThinkingSignature;
      AgentMetrics turnMetrics;
      StopReason turnStopReason = StopReason::Stop;
      std::string streamError;
      bool sawContent = false;
      bool sawThinking = false;
      bool sawTool = false;
      bool mixedContentAndTools = false;

      // Token repetition detection for hallucination loops
      bool tokenLoopDetected = false;
      char lastChar = '\0';
      int consecutiveRepeatCount = 0;
      const int MAX_CONSECUTIVE_REPEAT = 15;

      provider->stream(*context.history, opts, [&](const StreamEvent &ev) {
        if (context.state.currentStatus == AgentStatus::ProviderWaiting) {
          if (std::holds_alternative<TextChunk>(ev) ||
              std::holds_alternative<ThinkingChunk>(ev) ||
              std::holds_alternative<ToolCallChunk>(ev)) {
            context.state.currentStatus = AgentStatus::Streaming;
          }
        }

        if (auto *txt = std::get_if<TextChunk>(&ev)) {
          onEvent(ev);
          if (sawTool) {
            mixedContentAndTools = true;
          }
          // Treat pure-whitespace content as non-visible so tools can still
          // run.
          for (unsigned char c : txt->delta) {
            if (!std::isspace(c)) {
              sawContent = true;
              break;
            }
          }
          // Check for token loop (same character repeated)
          for (char c : txt->delta) {
            if (c == lastChar && !tokenLoopDetected) {
              consecutiveRepeatCount++;
              if (consecutiveRepeatCount >= MAX_CONSECUTIVE_REPEAT) {
                tokenLoopDetected = true;
              }
            } else {
              consecutiveRepeatCount = 1;
              lastChar = c;
            }
          }

          if (!tokenLoopDetected) {
            fullResponse += txt->delta;
          }
        } else if (auto *thk = std::get_if<ThinkingChunk>(&ev)) {
          onEvent(ev);
          sawThinking = true;
          fullThinking += thk->delta;
          if (!thk->signature.empty()) {
            lastThinkingSignature = thk->signature;
          }
        } else if (auto *tcc = std::get_if<ToolCallChunk>(&ev)) {
          if (sawContent) {
            mixedContentAndTools = true;
          }
          sawTool = true;
          // Emit immediately so TUI can show "Preparing" state
          onEvent(ev);
          bool found = false;
          for (auto &existing : accumulatedToolChunks) {
            if (existing.index == tcc->index) {
              existing.nameDelta += tcc->nameDelta;
              existing.argsDelta += tcc->argsDelta;
              if (!tcc->id.empty())
                existing.id = tcc->id;
              found = true;
              break;
            }
          }
          if (!found) {
            auto newChunk = *tcc;
            if (newChunk.id.empty())
              newChunk.id = "call_" + std::to_string(turnCount) + "_" +
                            std::to_string(tcc->index);
            accumulatedToolChunks.push_back(newChunk);
          }
        } else if (auto *met = std::get_if<AgentMetrics>(&ev)) {
          onEvent(ev);
          turnMetrics = *met;
        } else if (auto *done = std::get_if<StreamDone>(&ev)) {
          onEvent(ev);
          turnStopReason = done->reason;
        } else if (auto *err = std::get_if<StreamError>(&ev)) {
          onEvent(ev);
          streamError = err->message;
        } else {
          onEvent(ev);
        }
      });

      // ToolCallChunk events are now emitted immediately above
      // No need to re-emit buffered events

      // If mixed content/tool output occurred, prefer tool calls and drop
      // visible text.
      if (mixedContentAndTools && !accumulatedToolChunks.empty()) {
        fullResponse.clear();
        sawContent = false;
        turnStopReason = StopReason::ToolUse;
      } else if (sawContent) {
        accumulatedToolChunks.clear();
        if (turnStopReason == StopReason::ToolUse) {
          turnStopReason = StopReason::Stop;
        }
      }

      // If token loop was detected, inject a nudge to refocus the agent
      if (tokenLoopDetected && fullResponse.empty() &&
          accumulatedToolChunks.empty()) {
        AgentTurn loopNudgeTurn;
        loopNudgeTurn.turnId = "loop-nudge-" + std::to_string(turnCount);
        Message loopNudgeMsg;
        loopNudgeMsg.role = Role::User;
        loopNudgeMsg.content.push_back(TextContent{
            "Your output was cut off due to token repetition (hallucination "
            "loop). "
            "Please step back, refocus, and try a different approach. "
            "Do NOT repeat the same failed actions."});
        auto nudgeNow = std::chrono::system_clock::now();
        loopNudgeMsg.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                nudgeNow.time_since_epoch())
                .count());
        loopNudgeTurn.messages.push_back(loopNudgeMsg);
        context.history->turns.push_back(loopNudgeTurn);
        if (context.config.persistHistory && journaler)
          journaler->appendTurn(loopNudgeTurn);

        // Continue to next turn instead of erroring
        continue;
      }

      // If there was a stream error and no content came back, retry
      if (!streamError.empty() && fullResponse.empty() &&
          accumulatedToolChunks.empty()) {
        consecutiveProviderFailures++;
        if (consecutiveProviderFailures > maxProviderRetries) {
          throw std::runtime_error("Provider stream error: " + streamError);
        }
        // Emit retry event and wait briefly before retrying
        int retryDelaySec = 1 << (consecutiveProviderFailures - 1); // 1, 2, 4
        onEvent(StreamRetrying{consecutiveProviderFailures, maxProviderRetries,
                               429, retryDelaySec * 1000,
                               "Provider error, retrying", ""});
        std::this_thread::sleep_for(std::chrono::seconds(retryDelaySec));
        continue;
      }

      // Reset consecutive failure counter on success
      consecutiveProviderFailures = 0;

      // --- Build assistant turn ---
      AgentTurn assistantTurn;
      assistantTurn.turnId =
          "assistant-" + std::to_string(context.history->turns.size());
      assistantTurn.stopReason = turnStopReason;

      // Store per-turn metrics
      assistantTurn.metrics = turnMetrics;

      // Accumulate into session total
      context.aggregateMetrics += turnMetrics;

      Message assistantMsg;
      assistantMsg.role = Role::Assistant;
      if (!fullThinking.empty())
        assistantMsg.content.push_back(
            ThinkingContent{fullThinking, lastThinkingSignature});
      if (!fullResponse.empty())
        assistantMsg.content.push_back(TextContent{fullResponse});

      for (const auto &chunk : accumulatedToolChunks) {
        assistantMsg.content.push_back(
            ToolCallContent{chunk.id, chunk.nameDelta, chunk.argsDelta});
      }

      auto now_end = std::chrono::system_clock::now();
      assistantMsg.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now_end.time_since_epoch())
              .count());

      assistantTurn.messages.push_back(assistantMsg);
      context.history->turns.push_back(assistantTurn);
      if (context.config.persistHistory && journaler)
        journaler->appendTurn(assistantTurn);

      // Broadcast turn completion
      onEvent(AgentTurnCompleted{context.identity.id, assistantTurn,
                                 context.aggregateMetrics,
                                 context.identity.parentId});

      // --- Check for termination ---
      if (accumulatedToolChunks.empty()) {
        if (fullResponse.empty() && fullThinking.empty()) {
          throw std::runtime_error(
              "Provider returned empty response (timeout or model failure)");
        }
        taskFinished = true;
      } else {
        // --- State: ExecutingTool ---
        context.state.currentStatus = AgentStatus::ExecutingTool;

        // Track pending tool calls
        for (const auto &chunk : accumulatedToolChunks) {
          context.state.pendingToolCalls.push_back(chunk.id);
        }

        auto toolStartMs = nowMs();
        executeTools(accumulatedToolChunks, onEvent);
        auto toolEndMs = nowMs();

        // Update the turn metrics with tool execution time
        // (The turn is already pushed to history, so update the last assistant
        // turn in-place)
        auto &lastTurn =
            context.history
                ->turns[context.history->turns.size() -
                        2]; // assistant turn is 2nd-to-last (tool result turn
                            // was just pushed by executeTools)
        lastTurn.metrics.timing.toolExecutionMs = toolEndMs - toolStartMs;

        // Also update the aggregate (only the tool timing delta)
        context.aggregateMetrics.timing.toolExecutionMs +=
            (toolEndMs - toolStartMs);

        // Clear pending tool calls
        context.state.pendingToolCalls.clear();
      }

    } catch (const std::exception &e) {
      // --- State: Error ---
      context.state.currentStatus = AgentStatus::Error;
      context.state.fatalError = e.what();

      // Persist error as a system turn in history for journal survival
      AgentTurn errorTurn;
      errorTurn.turnId =
          "error-" + std::to_string(context.history->turns.size());
      Message errorMsg;
      errorMsg.role = Role::Error;
      errorMsg.content.push_back(
          ErrorContent{"Agent Runtime Error",
                       "The agent encountered a fatal runtime exception.",
                       std::string(e.what())});
      errorMsg.timestamp = nowMs();
      errorTurn.messages.push_back(errorMsg);
      context.history->turns.push_back(errorTurn);
      if (context.config.persistHistory && journaler)
        journaler->appendTurn(errorTurn);

      // Emit error as a StreamError event
      onEvent(StreamError{e.what(), 0, ""});
      break;
    }
  }

  // --- Final state ---
  if (interrupted.load()) {
    context.state.currentStatus = AgentStatus::Cancelled;
  } else if (context.state.currentStatus != AgentStatus::Error) {
    context.state.currentStatus = AgentStatus::Idle;
  }

  running = false;
}

void Agent::executeTools(const std::vector<ToolCallChunk> &chunks,
                         std::function<void(const StreamEvent &)> onEvent) {
  // Check for insanity loop BEFORE executing tools
  for (const auto &chunk : chunks) {
    // Create signature: "toolName:args"
    std::string signature = chunk.nameDelta + ":" + chunk.argsDelta;

    // Check if this exact call has been repeated consecutively
    int repeatCount = 0;
    for (auto it = context.state.recentToolCallSignatures.rbegin();
         it != context.state.recentToolCallSignatures.rend(); ++it) {
      if (*it == signature) {
        repeatCount++;
      } else {
        break; // Only count consecutive repeats from the end
      }
    }

    if (repeatCount >= context.config.maxIdenticalToolCalls) {
      // Inject intervention nudge
      AgentTurn interventionTurn;
      interventionTurn.turnId =
          "insanity-nudge-" + std::to_string(context.history->turns.size());
      Message interventionMsg;
      interventionMsg.role = Role::User;
      interventionMsg.content.push_back(
          TextContent{"You are calling the same tool with identical arguments "
                      "repeatedly (" +
                      std::to_string(repeatCount + 1) +
                      " times). This indicates an insanity loop. " +
                      "Please stop and try a different approach. Do NOT repeat "
                      "this tool call."});
      auto nudgeNow = std::chrono::system_clock::now();
      interventionMsg.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              nudgeNow.time_since_epoch())
              .count());
      interventionTurn.messages.push_back(interventionMsg);
      context.history->turns.push_back(interventionTurn);
      if (context.config.persistHistory && journaler)
        journaler->appendTurn(interventionTurn);

      // Clear the recent signatures to allow recovery
      context.state.recentToolCallSignatures.clear();

      // Broadcast and return early (don't execute the repeated tool)
      onEvent(AgentTurnCompleted{context.identity.id, interventionTurn,
                                 context.aggregateMetrics,
                                 context.identity.parentId});
      return;
    }
  }

  AgentTurn toolResultTurn;
  toolResultTurn.turnId =
      "tools-" + std::to_string(context.history->turns.size());

  // Execute ALL tools in parallel using futures
  struct ToolExecution {
    std::string toolCallId;
    std::string name;
    std::string args;
    std::future<std::tuple<std::string, bool, std::string, std::string>> future;
  };

  std::vector<ToolExecution> executions;

  // First pass: start all tool executions in parallel
  for (const auto &chunk : chunks) {
    if (interrupted.load())
      break;

    rapidjson::Document input;
    input.Parse(chunk.argsDelta.c_str());

    if (input.HasParseError()) {
      // Invalid JSON - create error result immediately
      Message msg;
      msg.role = Role::ToolResult;
      msg.content.push_back(ToolResultContent{
          chunk.id, "Invalid JSON arguments: " + chunk.argsDelta, false, "",
          ""});
      auto now = std::chrono::system_clock::now();
      msg.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now.time_since_epoch())
              .count());
      toolResultTurn.messages.push_back(msg);
      continue;
    }

    // Capture chunk data for async execution
    std::string toolName = chunk.nameDelta;
    std::string toolArgs = chunk.argsDelta;
    std::string toolId = chunk.id;

    // Launch tool execution in async thread
    auto future = std::async(
        std::launch::async,
        [this, toolName, toolArgs,
         toolId]() -> std::tuple<std::string, bool, std::string, std::string> {
          rapidjson::Document input;
          input.Parse(toolArgs.c_str());

          ToolContext toolCtx{*host, *this, toolId};
          auto result = toolRegistry.execute(toolName, input, toolCtx);

          std::string resultStr;
          if (result.success) {
            resultStr = result.data;
          } else {
            resultStr = "Error: " + result.error;
          }

          return {resultStr, result.success, result.processId,
                  result.subagentId};
        });

    executions.push_back({toolId, toolName, toolArgs, std::move(future)});
  }

  // Second pass: collect all results (already running in parallel)
  for (auto &exec : executions) {
    if (interrupted.load())
      break;

    auto [resultStr, success, resultProcessId, resultSubagentId] =
        exec.future.get();

    Message msg;
    msg.role = Role::ToolResult;
    msg.content.push_back(ToolResultContent{exec.toolCallId, resultStr, success,
                                            resultProcessId, resultSubagentId});
    auto now = std::chrono::system_clock::now();
    msg.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
    toolResultTurn.messages.push_back(msg);

    // Track edited files for file_edit and file_write tools
    if (success) {
      if (exec.name == "file_edit" || exec.name == "file_write") {
        rapidjson::Document input;
        input.Parse(exec.args.c_str());
        if (!input.HasParseError() && input.HasMember("path")) {
          std::string filePath = input["path"].GetString();
          if (std::find(context.state.editedFiles.begin(),
                        context.state.editedFiles.end(),
                        filePath) == context.state.editedFiles.end()) {
            context.state.editedFiles.push_back(filePath);
          }
          std::string actionDesc = "Edited file: " + filePath;
          context.state.completedActions.push_back(actionDesc);
        }
      }
    }

    // Track this tool call signature after successful execution
    std::string signature = exec.name + ":" + exec.args;
    context.state.recentToolCallSignatures.push_back(signature);
  }

  // Keep only last 20 signatures to prevent unbounded growth
  if (context.state.recentToolCallSignatures.size() > 20) {
    context.state.recentToolCallSignatures.erase(
        context.state.recentToolCallSignatures.begin(),
        context.state.recentToolCallSignatures.begin() +
            (context.state.recentToolCallSignatures.size() - 20));
  }

  context.history->turns.push_back(toolResultTurn);
  if (context.config.persistHistory && journaler)
    journaler->appendTurn(toolResultTurn);

  // Broadcast turn completion
  onEvent(AgentTurnCompleted{context.identity.id, toolResultTurn,
                             context.aggregateMetrics,
                             context.identity.parentId});
}

void Agent::compactContext(
    std::function<void(const shared::StreamEvent &)> onEvent) {
  context.state.currentStatus = AgentStatus::Compacting;
  onEvent(AgentCompacting{context.identity.id, context.identity.parentId});

  // Build factual state preamble to preserve actual work state
  std::string factualState = "\n## FACTUAL STATE (GROUND TRUTH)\n\n";

  if (!context.state.readFiles.empty()) {
    factualState += "**Files Read:** ";
    for (size_t i = 0; i < context.state.readFiles.size(); ++i) {
      factualState += context.state.readFiles[i];
      if (i < context.state.readFiles.size() - 1)
        factualState += ", ";
    }
    factualState += "\n\n";
  }

  if (!context.state.editedFiles.empty()) {
    factualState += "**Files Edited:** ";
    for (size_t i = 0; i < context.state.editedFiles.size(); ++i) {
      factualState += context.state.editedFiles[i];
      if (i < context.state.editedFiles.size() - 1)
        factualState += ", ";
    }
    factualState += "\n\n";
  }

  if (!context.state.completedActions.empty()) {
    factualState += "**Completed Actions:**\n";
    for (const auto &action : context.state.completedActions) {
      factualState += "- " + action + "\n";
    }
    factualState += "\n";
  }

  if (!context.state.ownedProcesses.empty()) {
    factualState += "**Active Background Processes:** ";
    for (size_t i = 0; i < context.state.ownedProcesses.size(); ++i) {
      factualState += context.state.ownedProcesses[i];
      if (i < context.state.ownedProcesses.size() - 1)
        factualState += ", ";
    }
    factualState += "\n\n";
  }

  if (context.state.fatalError.has_value()) {
    factualState +=
        "**Fatal Error:** " + context.state.fatalError.value() + "\n\n";
  }

  std::string compactionPrompt = PurposeLoader::loadCompactionPrompt();

  // Prepend factual state to compaction prompt
  std::string fullCompactionPrompt = factualState + compactionPrompt;
  std::string fullSummary;
  std::string fullThinking;

  if (interrupted.load())
    return;

  provider->generateSummary(
      context.config.modelId, *context.history, fullCompactionPrompt,
      [&](const StreamEvent &ev) {
        if (interrupted.load())
          return;
        if (auto *act = std::get_if<AgentCompactionText>(&ev)) {
          fullSummary += act->delta;
          onEvent(AgentCompactionText{context.identity.id, act->delta,
                                      context.identity.parentId});
        } else if (auto *thk = std::get_if<AgentCompactionThinking>(&ev)) {
          fullThinking += thk->delta;
          onEvent(AgentCompactionThinking{context.identity.id, thk->delta,
                                          context.identity.parentId});
        } else {
          onEvent(ev);
        }
      },
      &interrupted);

  // Validate summary before clearing history
  if (fullSummary.empty()) {
    onEvent(StreamError{"Context compaction failed: Empty summary generated", 0,
                        ""});
    context.state.currentStatus = AgentStatus::Idle;
    return;
  }

  uint32_t oldTokens = context.aggregateMetrics.tokens.contextSize;

  // Preserve Pinned Turns (0 and 1)
  std::vector<AgentTurn> pinned;
  if (context.history->turns.size() >= 2) {
    pinned.push_back(context.history->turns[0]);
    pinned.push_back(context.history->turns[1]);
  } else {
    pinned = context.history->turns;
  }

  context.history->turns.clear();
  for (auto &t : pinned)
    context.history->turns.push_back(t);

  // Create Synthetic Memory Turn
  AgentTurn summaryTurn;
  summaryTurn.turnId = "compaction-summary-" + std::to_string(nowMs());

  Message summaryMsg;
  summaryMsg.role = Role::User;
  if (!fullThinking.empty())
    summaryMsg.content.push_back(ThinkingContent{fullThinking, ""});
  summaryMsg.content.push_back(
      TextContent{"COMPACTION SUMMARY: " + fullSummary});
  summaryMsg.timestamp = nowMs();

  summaryTurn.messages.push_back(summaryMsg);
  context.history->turns.push_back(summaryTurn);

  if (context.config.persistHistory && journaler) {
    journaler->appendTurn(summaryTurn);
  }

  // Reset context size to conservative estimate (system + task + summary ~1000
  // tokens) This prevents immediate re-compaction on next turn
  context.aggregateMetrics.tokens.contextSize = 1000;

  uint32_t tokensSaved = (oldTokens > 1000) ? oldTokens - 1000 : 0;
  onEvent(ContextCompacted{context.identity.id, tokensSaved,
                           context.identity.parentId});
}

} // namespace firmius::core
