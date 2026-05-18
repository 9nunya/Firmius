#include "providers/BaseAnthropicProvider.hpp"
#include "utils/InterruptibleSleep.hpp"
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string_view>

namespace firmius::provider {

namespace {

struct CurlTransferResult {
  CURLcode code = CURLE_OK;
  long responseCode = 0;
};

CurlTransferResult
performInterruptibleTransfer(CURL *curl, std::atomic<bool> *abortSignal) {
  CurlTransferResult result;
  CURLM *multi = curl_multi_init();
  if (!multi) {
    result.code = CURLE_FAILED_INIT;
    return result;
  }

  FILE *devnull = fopen("/dev/null", "w");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
  if (devnull) {
    curl_easy_setopt(curl, CURLOPT_STDERR, devnull);
  }

  CURLMcode multiCode = curl_multi_add_handle(multi, curl);
  if (multiCode != CURLM_OK) {
    result.code = CURLE_FAILED_INIT;
    curl_multi_cleanup(multi);
    if (devnull) {
      fclose(devnull);
    }
    return result;
  }

  int stillRunning = 0;
  multiCode = curl_multi_perform(multi, &stillRunning);
  if (multiCode != CURLM_OK) {
    result.code = CURLE_RECV_ERROR;
  }

  while (result.code == CURLE_OK && stillRunning > 0) {
    if (abortSignal && abortSignal->load()) {
      result.code = CURLE_ABORTED_BY_CALLBACK;
      break;
    }

    int numFds = 0;
    multiCode = curl_multi_wait(multi, nullptr, 0, 20, &numFds);
    if (multiCode != CURLM_OK) {
      result.code = CURLE_RECV_ERROR;
      break;
    }

    if (abortSignal && abortSignal->load()) {
      result.code = CURLE_ABORTED_BY_CALLBACK;
      break;
    }

    multiCode = curl_multi_perform(multi, &stillRunning);
    if (multiCode != CURLM_OK) {
      result.code = CURLE_RECV_ERROR;
      break;
    }
  }

  if (result.code == CURLE_OK) {
    int messagesLeft = 0;
    while (CURLMsg *msg = curl_multi_info_read(multi, &messagesLeft)) {
      if (msg->msg == CURLMSG_DONE) {
        result.code = msg->data.result;
      }
    }
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.responseCode);
  curl_multi_remove_handle(multi, curl);
  curl_multi_cleanup(multi);
  if (devnull) {
    fclose(devnull);
  }
  return result;
}

size_t anthropicSSEWriteCallback(char *ptr, size_t size, size_t nmemb,
                                 void *userdata) {
  if (!userdata) return 0;
  auto *ctx = static_cast<AnthropicStreamContext *>(userdata);
  if (ctx->abortSignal && ctx->abortSignal->load())
    return 0;

  ctx->buffer.append(ptr, size * nmemb);

  size_t newlinePos;
  while ((newlinePos = ctx->buffer.find('\n', ctx->readOffset)) !=
         std::string::npos) {
    std::string_view line(ctx->buffer.data() + ctx->readOffset,
                          newlinePos - ctx->readOffset);
    ctx->readOffset = newlinePos + 1;

    // Remove \r if present
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    if (line.empty())
      continue;

    ctx->provider->processSSELine(std::string(line), *(ctx->onEvent), *ctx);
  }

  // Compact buffer if it gets too large (> 1MB)
  if (ctx->readOffset > 1024 * 1024) {
    ctx->buffer.erase(0, ctx->readOffset);
    ctx->readOffset = 0;
  }

  return size * nmemb;
}

std::string roleToString(Role r) {
  switch (r) {
  case Role::System:
    return "system";
  case Role::User:
    return "user";
  case Role::Assistant:
    return "assistant";
  case Role::ToolResult:
    return "user"; // Tool results are user messages in Anthropic
  case Role::Error:
    return "system";
  }
  return "user";
}

std::string trimTrailingWhitespace(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

} // namespace

BaseAnthropicProvider::BaseAnthropicProvider(std::string id,
                                             const std::string &baseUrl,
                                             const std::string &apiKey)
    : BaseAPIKeyProvider(std::move(id)), baseUrl(baseUrl) {
  // If apiKey is provided and non-empty, add it as the initial account
  if (!apiKey.empty()) {
    APIKeyAccount acc;
    acc.apiKey = apiKey;
    acc.keyPrefix = extractKeyPrefix(apiKey);
    acc.identifier = generateIdentifier();
    addAccount(acc);
  }
}

std::unique_ptr<APIKeyWizard> BaseAnthropicProvider::beginConnectionWizard() {
  return std::make_unique<SimpleAPIKeyWizard>();
}

std::uint64_t BaseAnthropicProvider::nowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

size_t BaseAnthropicProvider::headerCallback(char *ptr, size_t size,
                                             size_t nmemb, void *userdata) {
  auto *ctx = static_cast<AnthropicHeaderCaptureContext *>(userdata);
  size_t totalSize = size * nmemb;
  std::string headerLine(ptr, totalSize);

  if (headerLine.find("HTTP/") == 0) {
    size_t codePos = headerLine.find(' ');
    if (codePos != std::string::npos) {
      ctx->httpStatus = std::stol(headerLine.substr(codePos + 1, 3));
    }
  } else {
    std::string lowerHeader = headerLine;
    std::transform(lowerHeader.begin(), lowerHeader.end(), lowerHeader.begin(),
                   ::tolower);

    if (lowerHeader.find("retry-after:") == 0) {
      size_t colonPos = headerLine.find(':');
      if (colonPos != std::string::npos) {
        try {
          int seconds = std::stoi(headerLine.substr(colonPos + 1));
          ctx->retryAfterMs = std::max(ctx->retryAfterMs, seconds * 1000);
        } catch (...) {
        }
      }
    } else if (lowerHeader.find("retry-after-ms:") == 0) {
      size_t colonPos = headerLine.find(':');
      if (colonPos != std::string::npos) {
        try {
          int ms = std::stoi(headerLine.substr(colonPos + 1));
          ctx->retryAfterMs = std::max(ctx->retryAfterMs, ms);
        } catch (...) {
        }
      }
    } else if (lowerHeader.find("anthropic-ratelimit-requests-reset:") == 0) {
      size_t colonPos = headerLine.find(':');
      if (colonPos != std::string::npos) {
        try {
          int seconds = std::stoi(headerLine.substr(colonPos + 1));
          ctx->retryAfterMs = std::max(ctx->retryAfterMs, seconds * 1000);
        } catch (...) {
        }
      }
    } else if (lowerHeader.find("anthropic-ratelimit-tokens-reset:") == 0) {
      size_t colonPos = headerLine.find(':');
      if (colonPos != std::string::npos) {
        try {
          int seconds = std::stoi(headerLine.substr(colonPos + 1));
          ctx->retryAfterMs = std::max(ctx->retryAfterMs, seconds * 1000);
        } catch (...) {
        }
      }
    }
  }

  return totalSize;
}

bool BaseAnthropicProvider::isRetriableStatus(const RetryPolicyRuntime &policy,
                                              int httpStatus) const {
  return RetryPolicyResolver::isRetriableHttpStatus(policy, httpStatus);
}

bool BaseAnthropicProvider::isNonRetriableStatus(
    const RetryPolicyRuntime &policy, int httpStatus) const {
  return RetryPolicyResolver::isNonRetriableHttpStatus(policy, httpStatus);
}

int BaseAnthropicProvider::calculateRetryDelay(const RetryPolicyRuntime &policy,
                                               int attempt,
                                               int headerDelayMs) const {
  return RetryPolicyResolver::computeDelayMs(policy, attempt, headerDelayMs);
}

bool BaseAnthropicProvider::isRetriableCurlError(
    const RetryPolicyRuntime &policy, CURLcode code) const {
  return RetryPolicyResolver::isRetriableCurlError(policy, code);
}

std::string BaseAnthropicProvider::formatErrorMessage(
    const std::string &providerId, const std::string &modelId, int httpStatus,
    const std::string &responseBody, const std::string &prefix) {
  std::string message = prefix;
  if (httpStatus > 0) {
    message += " (HTTP " + std::to_string(httpStatus) + ")";
  }
  if (!providerId.empty()) {
    message += "\nProvider: " + providerId;
  }
  if (!modelId.empty()) {
    message += "\nModel: " + modelId;
  }
  const std::string rawBody = trimTrailingWhitespace(responseBody);
  if (!rawBody.empty()) {
    message += "\nRaw provider body:\n" + rawBody;
  }
  return message;
}

void BaseAnthropicProvider::processSSELine(
    const std::string &line, std::function<void(const StreamEvent &)> &onEvent,
    AnthropicStreamContext &ctx) {

  // Anthropic SSE format: "event: <type>\ndata: <json>"
  // We're looking for lines starting with "data:" (with or without space after
  // colon)
  if (!line.starts_with("data:"))
    return;

  // Extract data after "data:" prefix (skip colon and optional space)
  std::string data = line.substr(5);
  if (!data.empty() && data[0] == ' ') {
    data = data.substr(1);
  }
  if (data.empty())
    return;

  rapidjson::Document d;
  d.Parse(data.c_str());
  if (d.HasParseError() || !d.IsObject())
    return;

  // Get the event type from the "type" field
  if (!d.HasMember("type") || !d["type"].IsString())
    return;

  std::string eventType = d["type"].GetString();

  // Handle different Anthropic event types
  if (eventType == "message_start") {
    // Capture usage from message_start (Kimi sends full usage here)
    if (d.HasMember("message") && d["message"].IsObject()) {
      const auto &message = d["message"];
      if (message.HasMember("usage") && message["usage"].IsObject()) {
        const auto &usage = message["usage"];

        if (usage.HasMember("input_tokens")) {
          if (usage["input_tokens"].IsUint()) {
            ctx.inputTokens = usage["input_tokens"].GetUint();
          } else if (usage["input_tokens"].IsUint64()) {
            ctx.inputTokens =
                static_cast<std::uint32_t>(usage["input_tokens"].GetUint64());
          }
        }
        if (usage.HasMember("output_tokens")) {
          if (usage["output_tokens"].IsUint()) {
            ctx.outputTokens = usage["output_tokens"].GetUint();
          } else if (usage["output_tokens"].IsUint64()) {
            ctx.outputTokens =
                static_cast<std::uint32_t>(usage["output_tokens"].GetUint64());
          }
        }
        if (usage.HasMember("cache_read_input_tokens")) {
          if (usage["cache_read_input_tokens"].IsUint()) {
            ctx.cacheRead = usage["cache_read_input_tokens"].GetUint();
          } else if (usage["cache_read_input_tokens"].IsUint64()) {
            ctx.cacheRead = static_cast<std::uint32_t>(
                usage["cache_read_input_tokens"].GetUint64());
          }
        }
        if (usage.HasMember("cache_creation_input_tokens")) {
          if (usage["cache_creation_input_tokens"].IsUint()) {
            ctx.cacheWrite = usage["cache_creation_input_tokens"].GetUint();
          } else if (usage["cache_creation_input_tokens"].IsUint64()) {
            ctx.cacheWrite = static_cast<std::uint32_t>(
                usage["cache_creation_input_tokens"].GetUint64());
          }
        }
        ctx.usageCaptured = true;
      }
    }
  } else if (eventType == "content_block_start") {
    // A content block is starting (text, tool_use, etc.)
    if (d.HasMember("content_block") && d["content_block"].IsObject()) {
      const auto &block = d["content_block"];
      if (block.HasMember("type") && block["type"].IsString()) {
        std::string blockType = block["type"].GetString();

        if (blockType == "tool_use") {
          // Tool call starting
          ToolCallChunk chunk;
          ctx.currentContentBlockType = blockType;
          ctx.currentToolCallId.clear();
          ctx.currentToolCallName.clear();
          ctx.currentToolCallArgs.clear();
          ctx.currentToolCallFinalized = false;
          chunk.argsDelta = "";

          // Index can be at event level or inside content_block
          if (d.HasMember("index") && d["index"].IsUint()) {
            chunk.index = d["index"].GetUint();
          } else if (block.HasMember("index") && block["index"].IsUint()) {
            chunk.index = block["index"].GetUint();
          }

          ctx.currentToolCallIndex = static_cast<int>(chunk.index);
          ctx.inToolCall = true;
          if (block.HasMember("id") && block["id"].IsString()) {
            chunk.id = block["id"].GetString();
            ctx.currentToolCallId = chunk.id;
          }
          if (block.HasMember("name") && block["name"].IsString()) {
            chunk.nameDelta = block["name"].GetString();
            ctx.currentToolCallName = chunk.nameDelta;
            onEvent(chunk);
          }
          if (block.HasMember("input") && block["input"].IsObject()) {
            ctx.currentToolCallArgs = "{}";
          }
        }
      }
    }
  } else if (eventType == "content_block_delta") {
    // Content delta (text, thinking, or tool arguments)
    if (d.HasMember("delta") && d["delta"].IsObject()) {
      const auto &delta = d["delta"];

      if (delta.HasMember("type") && delta["type"].IsString()) {
        std::string deltaType = delta["type"].GetString();

        if (deltaType == "text_delta") {
          // Regular text content
          if (delta.HasMember("text") && delta["text"].IsString()) {
            onEvent(TextChunk{delta["text"].GetString()});
          }
        } else if (deltaType == "thinking_delta") {
          // Thinking/reasoning content
          if (delta.HasMember("thinking") && delta["thinking"].IsString()) {
            std::string signature;
            if (delta.HasMember("signature") && delta["signature"].IsString()) {
              signature = delta["signature"].GetString();
            }
            onEvent(ThinkingChunk{delta["thinking"].GetString(), signature});
          }
        } else if (deltaType == "input_json_delta") {
          // Tool call arguments delta
          if (delta.HasMember("partial_json") &&
              delta["partial_json"].IsString()) {
            ToolCallChunk chunk;
            chunk.argsDelta = delta["partial_json"].GetString();
            // Index can be at event level or inside delta
            if (d.HasMember("index") && d["index"].IsUint()) {
              chunk.index = d["index"].GetUint();
            } else if (delta.HasMember("index") && delta["index"].IsUint()) {
              chunk.index = delta["index"].GetUint();
            }
            onEvent(chunk);
            ctx.currentToolCallIndex = static_cast<int>(chunk.index);
            ctx.currentToolCallArgs += chunk.argsDelta;
          }
        }
      }
    }
  } else if (eventType == "content_block_stop") {
    // Content block finished
    int stoppedIndex = ctx.currentContentBlockIndex;
    if (d.HasMember("index") && d["index"].IsUint()) {
      stoppedIndex = static_cast<int>(d["index"].GetUint());
    }
    if (ctx.inToolCall && ctx.currentContentBlockType == "tool_use" &&
        stoppedIndex == ctx.currentToolCallIndex &&
        !ctx.currentToolCallFinalized && !ctx.currentToolCallName.empty() &&
        !ctx.currentToolCallArgs.empty()) {
      ctx.currentToolCallFinalized = true;
      onEvent(ToolCall{ctx.currentToolCallId,
                       ctx.currentToolCallIndex >= 0
                           ? static_cast<std::uint32_t>(ctx.currentToolCallIndex)
                           : std::numeric_limits<std::uint32_t>::max(),
                       ctx.currentToolCallName, ctx.currentToolCallArgs});
      ctx.inToolCall = false;
    }
    // Message delta with stop reason and usage
    StopReason stopReason = StopReason::Stop;

    if (d.HasMember("delta") && d["delta"].IsObject()) {
      const auto &delta = d["delta"];
      if (delta.HasMember("stop_reason") && delta["stop_reason"].IsString()) {
        std::string reason = delta["stop_reason"].GetString();
        if (reason == "tool_use") {
          stopReason = StopReason::ToolUse;
        } else if (reason == "max_tokens") {
          stopReason = StopReason::MaxTokens;
        } else if (reason == "end_turn") {
          stopReason = StopReason::Stop;
        } else if (reason == "stop_sequence") {
          stopReason = StopReason::Stop;
        }
      }
    }

    // Emit StreamDone with the stop reason
    onEvent(StreamDone{stopReason});

    // Capture/update usage from message_delta (Kimi sends output_tokens here)
    if (d.HasMember("usage") && d["usage"].IsObject()) {
      const auto &usage = d["usage"];
      if (usage.HasMember("output_tokens")) {
        if (usage["output_tokens"].IsUint()) {
          ctx.outputTokens = usage["output_tokens"].GetUint();
        } else if (usage["output_tokens"].IsUint64()) {
          ctx.outputTokens =
              static_cast<std::uint32_t>(usage["output_tokens"].GetUint64());
        }
      }
      // Also capture input_tokens if not already captured
      if (!ctx.usageCaptured && usage.HasMember("input_tokens")) {
        if (usage["input_tokens"].IsUint()) {
          ctx.inputTokens = usage["input_tokens"].GetUint();
        } else if (usage["input_tokens"].IsUint64()) {
          ctx.inputTokens =
              static_cast<std::uint32_t>(usage["input_tokens"].GetUint64());
        }
      }
      ctx.usageCaptured = true;
    }

    // Emit metrics using captured usage
    if (ctx.usageCaptured) {
      AgentMetrics am;

      am.tokens.contextSize = ctx.inputTokens;
      am.tokens.completion = ctx.outputTokens;
      am.tokens.total = ctx.inputTokens + ctx.outputTokens;
      am.tokens.cacheRead = ctx.cacheRead;
      am.tokens.cacheWrite = ctx.cacheWrite;
      am.tokens.prompt = (ctx.inputTokens >= ctx.cacheRead)
                             ? (ctx.inputTokens - ctx.cacheRead)
                             : 0;
      am.tokens.cumulativePrompt = am.tokens.prompt;

      onEvent(am);
    }
  } else if (eventType == "message_stop") {
    // Message completed (already handled by message_delta)
  } else if (eventType == "error") {
    ctx.inToolCall = false;
    // Error event
    std::string errorMsg = "Unknown error";
    if (d.HasMember("error") && d["error"].IsObject()) {
      const auto &error = d["error"];
      if (error.HasMember("message") && error["message"].IsString()) {
        errorMsg = error["message"].GetString();
      }
    }
    onEvent(StreamError{errorMsg, 0, ""});
  }
}

void BaseAnthropicProvider::stream(
    const AgentHistory &history, const ProviderOptions &opts,
    std::function<void(const StreamEvent &)> onEvent) {
  const RetryPolicyRuntime retryPolicy = RetryPolicyResolver::resolve(getId());

  std::string url = getMessagesUrl();
  std::string body = prepareRequestBody(history, opts);
  auto headerMap = getHeaders();

  auto startMs = nowMs();
  bool firstTokenEmitted = false;
  std::uint64_t firstTokenMs = 0;
  AgentMetrics capturedMetrics;
  bool metricsReceived = false;
  bool doneReceived = false;

  auto wrappedOnEvent = [&](const StreamEvent &ev) {
    if (!firstTokenEmitted) {
      if (std::holds_alternative<TextChunk>(ev) ||
          std::holds_alternative<ThinkingChunk>(ev)) {
        firstTokenMs = nowMs();
        firstTokenEmitted = true;
      }
    }

    if (auto *met = std::get_if<AgentMetrics>(&ev)) {
      capturedMetrics = *met;
      metricsReceived = true;
      return;
    }

    if (std::holds_alternative<StreamDone>(ev)) {
      doneReceived = true;
    }

    onEvent(ev);
  };

  int attempt = 0;
  CURLcode res = CURLE_OK;
  long responseCode = 0;

  struct curl_slist *headers = nullptr;
  for (const auto &[k, v] : headerMap) {
    headers = curl_slist_append(headers, (k + ": " + v).c_str());
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    if (headers)
      curl_slist_free_all(headers);
    onEvent(StreamError{"CURL init failed", 0, ""});
    return;
  }

  while (attempt <= retryPolicy.config.maxRetries) {
    curl_easy_reset(curl);
    std::function<void(const StreamEvent &)> wrappedFn = wrappedOnEvent;
    AnthropicStreamContext ctx;
    ctx.provider = this;
    ctx.onEvent = &wrappedFn;
    ctx.abortSignal = opts.abortSignal;
    AnthropicHeaderCaptureContext currentHeaderCtx;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, anthropicSSEWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &currentHeaderCtx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     static_cast<long>(retryPolicy.config.timeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     static_cast<long>(retryPolicy.config.connectTimeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    const CurlTransferResult transfer =
        performInterruptibleTransfer(curl, opts.abortSignal);
    res = transfer.code;
    responseCode = transfer.responseCode;

    if (res != CURLE_OK) {
      if (opts.abortSignal && opts.abortSignal->load()) {
        break;
      }
      if (isRetriableCurlError(retryPolicy, res) &&
          attempt < retryPolicy.config.maxRetries) {
        int delayMs = calculateRetryDelay(retryPolicy, attempt, 0);
        onEvent(StreamRetrying{attempt + 1,
                               retryPolicy.config.maxRetries,
                               0,
                               delayMs,
                               "transport error",
                               "",
                               std::string("CURL error: ") +
                                   curl_easy_strerror(res)});
        if (!interruptibleSleep(std::chrono::milliseconds(delayMs),
                                opts.abortController, opts.abortSignal)) {
          return;
        }
        attempt++;
        continue;
      }
      onEvent(StreamError{std::string("CURL error: ") + curl_easy_strerror(res),
                          0, ""});
      break;
    }

    if (responseCode < 400) {
      break;
    }

    if (isNonRetriableStatus(retryPolicy, static_cast<int>(responseCode))) {
      std::string errMsg = formatErrorMessage(getId(), opts.modelId,
                                              static_cast<int>(responseCode),
                                              ctx.buffer, "API error");
      onEvent(StreamError{errMsg, static_cast<int>(responseCode), ""});
      break;
    }

    if (isRetriableStatus(retryPolicy, static_cast<int>(responseCode))) {
      if (attempt >= retryPolicy.config.maxRetries) {
        std::string errMsg = formatErrorMessage(
            getId(), opts.modelId, static_cast<int>(responseCode), ctx.buffer,
            "API error after " + std::to_string(attempt + 1) + " attempts");

        onEvent(StreamRetryExhausted{static_cast<int>(responseCode),
                                     attempt + 1,
                                     "Maximum retry attempts exceeded"});
        onEvent(StreamError{errMsg, static_cast<int>(responseCode), ""});
        break;
      }

      const int headerDelayMs = retryPolicy.config.respectRetryAfter
                                    ? currentHeaderCtx.retryAfterMs
                                    : 0;
      int delayMs =
          calculateRetryDelay(retryPolicy, attempt, headerDelayMs);
      std::string reason =
          responseCode == 429 ? "rate limited" : "server error";
      onEvent(StreamRetrying{attempt + 1,
                             retryPolicy.config.maxRetries,
                             static_cast<int>(responseCode),
                             delayMs,
                             reason,
                             "",
                             ""});

      if (!interruptibleSleep(std::chrono::milliseconds(delayMs),
                              opts.abortController, opts.abortSignal)) {
        return;
      }
      attempt++;
    } else {
      std::string errMsg = formatErrorMessage(getId(), opts.modelId,
                                              static_cast<int>(responseCode),
                                              ctx.buffer, "API error");
      onEvent(StreamError{errMsg, static_cast<int>(responseCode), ""});
      break;
    }
  }

  if (headers)
    curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  auto endMs = nowMs();

  if (metricsReceived) {
    capturedMetrics.timing.startMs = startMs;
    capturedMetrics.timing.firstTokenMs = firstTokenEmitted ? firstTokenMs : 0;
    capturedMetrics.timing.endMs = endMs;

    try {
      auto model = getModelInfo(opts.modelId);
      calculateCost(capturedMetrics, model);
    } catch (...) {
    }

    onEvent(capturedMetrics);
  }

  if (!doneReceived && res == CURLE_OK && responseCode < 400) {
    onEvent(StreamDone{StopReason::Stop});
  }
}

std::vector<ModelInfo> BaseAnthropicProvider::listModels() {
  // Default implementation returns empty list
  // Subclasses should override with actual model discovery
  return cachedModels;
}

std::map<std::string, std::string> BaseAnthropicProvider::getHeaders() {
  auto account = getAvailableAccount();
  std::string apiKeyValue = account ? (*account)->apiKey : "";

  std::map<std::string, std::string> headers = {
      {"x-api-key", apiKeyValue},
      {"Content-Type", "application/json"},
      {"anthropic-version", "2023-06-01"}};

  std::string betaHeader = getAnthropicBetaHeader();
  if (!betaHeader.empty()) {
    headers["anthropic-beta"] = betaHeader;
  }

  return headers;
}

std::string
BaseAnthropicProvider::prepareRequestBody(const AgentHistory &history,
                                          const ProviderOptions &opts) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();

  d.AddMember("model", rapidjson::Value(opts.modelId.c_str(), a), a);
  d.AddMember("temperature", opts.temperature, a);
  d.AddMember("stream", true, a);

  if (opts.maxTokens) {
    d.AddMember("max_tokens", opts.maxTokens.value(), a);
  } else {
    // Anthropic requires max_tokens - use a reasonable default
    d.AddMember("max_tokens", 32196, a);
  }

  if (!opts.stop.empty()) {
    rapidjson::Value stopSeqs(rapidjson::kArrayType);
    for (const auto &s : opts.stop) {
      stopSeqs.PushBack(rapidjson::Value(s.c_str(), a), a);
    }
    d.AddMember("stop_sequences", stopSeqs, a);
  }

  // Build messages array
  rapidjson::Value messages(rapidjson::kArrayType);
  std::vector<std::pair<Role, std::string>> systemMessages;

  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == Role::System) {
        // Collect system messages separately
        for (const auto &part : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&part)) {
            systemMessages.push_back({Role::System, txt->text});
          }
        }
        continue;
      }

      if (msg.role == Role::Error) {
        continue;
      }

      if (msg.role == Role::ToolResult) {
        // Tool results in Anthropic format are user messages with tool_result content blocks
        rapidjson::Value m(rapidjson::kObjectType);
        m.AddMember("role", "user", a);
        
        rapidjson::Value content(rapidjson::kArrayType);
        
        for (const auto &part : msg.content) {
          if (auto *res = std::get_if<ToolResultContent>(&part)) {
            rapidjson::Value toolResultPart(rapidjson::kObjectType);
            toolResultPart.AddMember("type", "tool_result", a);
            toolResultPart.AddMember("tool_use_id", rapidjson::Value(res->toolCallId.c_str(), a), a);
            
            // Check if result is an error
            if (res->result.find("error") != std::string::npos || 
                res->result.find("Error") != std::string::npos) {
              toolResultPart.AddMember("is_error", true, a);
            }
            
            // Add content (text representation of result)
            toolResultPart.AddMember("content", rapidjson::Value(res->result.c_str(), a), a);
            
            content.PushBack(toolResultPart, a);
          }
        }
        
        m.AddMember("content", content, a);
        messages.PushBack(m, a);
        continue;
      }

      rapidjson::Value m(rapidjson::kObjectType);
      m.AddMember("role", rapidjson::Value(roleToString(msg.role).c_str(), a),
                  a);

      rapidjson::Value content(rapidjson::kArrayType);

      for (const auto &part : msg.content) {
        if (auto *txt = std::get_if<TextContent>(&part)) {
          rapidjson::Value textPart(rapidjson::kObjectType);
          textPart.AddMember("type", "text", a);
          textPart.AddMember("text", rapidjson::Value(txt->text.c_str(), a), a);
          content.PushBack(textPart, a);
        } else if (auto *thk = std::get_if<ThinkingContent>(&part)) {
          // Include thinking as a separate content block if needed
          rapidjson::Value textPart(rapidjson::kObjectType);
          textPart.AddMember("type", "text", a);
          std::string thinkingText = "[Thinking: " + thk->thinking + "]";
          textPart.AddMember("text", rapidjson::Value(thinkingText.c_str(), a),
                             a);
          content.PushBack(textPart, a);
        } else if (auto *tcc = std::get_if<ToolCallContent>(&part)) {
          rapidjson::Value toolPart(rapidjson::kObjectType);
          toolPart.AddMember("type", "tool_use", a);
          toolPart.AddMember("id", rapidjson::Value(tcc->id.c_str(), a), a);
          toolPart.AddMember("name", rapidjson::Value(tcc->name.c_str(), a), a);

          rapidjson::Document argsDoc;
          argsDoc.Parse(tcc->args.c_str());
          if (!argsDoc.HasParseError()) {
            rapidjson::Value args;
            args.CopyFrom(argsDoc, a);
            toolPart.AddMember("input", args, a);
          } else {
            toolPart.AddMember("input",
                               rapidjson::Value(rapidjson::kObjectType), a);
          }

          content.PushBack(toolPart, a);
        }
      }

      m.AddMember("content", content, a);
      messages.PushBack(m, a);
    }
  }

  // Add system message if present.
  // Token-caching pass: emit `system` as an array of content blocks (not
  // a bare string) so we can attach `cache_control: {"type":"ephemeral"}`
  // to the last block. This caches the system prompt across turns at no
  // cost on hits (90% discount, 1.25x base on the first write). Per
  // Anthropic docs, breakpoint goes on the LAST cacheable block.
  if (!systemMessages.empty()) {
    std::string systemContent;
    for (const auto &[role, text] : systemMessages) {
      if (!systemContent.empty())
        systemContent += "\n\n";
      systemContent += text;
    }
    rapidjson::Value systemArr(rapidjson::kArrayType);
    rapidjson::Value sysBlock(rapidjson::kObjectType);
    sysBlock.AddMember("type", "text", a);
    sysBlock.AddMember("text", rapidjson::Value(systemContent.c_str(), a), a);
    rapidjson::Value cacheCtrl(rapidjson::kObjectType);
    cacheCtrl.AddMember("type", "ephemeral", a);
    sysBlock.AddMember("cache_control", cacheCtrl, a);
    systemArr.PushBack(sysBlock, a);
    d.AddMember("system", systemArr, a);
  }

  // Add tools if present.
  // Token-caching pass: tools are the most stable part of an agent
  // session — they almost never change mid-conversation. Attach
  // cache_control to the LAST tool definition so the entire tool block
  // is cached across all turns. Anthropic's cache key matches by exact
  // prefix through the marked block, so order stability matters: the
  // tool list is iterated in opts.tools order which is whatever the
  // ProviderOptions gave us. (Tool-list serialization stability is
  // tracked separately under Tier 2 #8.)
  if (!opts.tools.empty()) {
    rapidjson::Value tools(rapidjson::kArrayType);
    for (size_t i = 0; i < opts.tools.size(); ++i) {
      const auto &t = opts.tools[i];
      rapidjson::Value tool(rapidjson::kObjectType);
      tool.AddMember("name", rapidjson::Value(t.name.c_str(), a), a);
      tool.AddMember("description", rapidjson::Value(t.description.c_str(), a),
                     a);

      rapidjson::Document schemaDoc;
      schemaDoc.Parse(t.inputSchema.c_str());
      if (!schemaDoc.HasParseError()) {
        rapidjson::Value inputSchema;
        inputSchema.CopyFrom(schemaDoc, a);
        tool.AddMember("input_schema", inputSchema, a);
      }

      // Attach cache_control to the last tool only — Anthropic limits us
      // to 4 cache breakpoints total per request and the per-block
      // marker caches everything from the start of the request through
      // that block.
      if (i + 1 == opts.tools.size()) {
        rapidjson::Value cacheCtrl(rapidjson::kObjectType);
        cacheCtrl.AddMember("type", "ephemeral", a);
        tool.AddMember("cache_control", cacheCtrl, a);
      }

      tools.PushBack(tool, a);
    }
    d.AddMember("tools", tools, a);
  }

  d.AddMember("messages", messages, a);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  return buffer.GetString();
}

ModelInfo BaseAnthropicProvider::getModelInfo(const std::string &modelId) {
  if (!modelsCached) {
    cachedModels = listModels();
    modelsCached = true;
  }
  for (auto &m : cachedModels) {
    if (m.id == modelId) {
      return m;
    }
  }
  // Return a default model info if not found
  ModelInfo info;
  info.id = modelId;
  info.provider = getId();
  info.contextWindow = 100000;
  return info;
}

void BaseAnthropicProvider::calculateCost(AgentMetrics &metrics,
                                          const ModelInfo &model) const {
  double totalCost = 0.0;

  if (model.pricePer1MInput > 0) {
    double inputCost =
        (static_cast<double>(metrics.tokens.prompt) / 1'000'000.0) *
        model.pricePer1MInput;
    totalCost += inputCost;
  }
  if (model.pricePer1MOutput > 0) {
    double outputCost =
        (static_cast<double>(metrics.tokens.completion) / 1'000'000.0) *
        model.pricePer1MOutput;
    totalCost += outputCost;
  }
  if (model.pricePer1MCacheRead > 0 && metrics.tokens.cacheRead > 0) {
    double cacheReadCost =
        (static_cast<double>(metrics.tokens.cacheRead) / 1'000'000.0) *
        model.pricePer1MCacheRead;
    totalCost += cacheReadCost;
  }
  if (model.pricePer1MCacheWrite > 0 && metrics.tokens.cacheWrite > 0) {
    double cacheWriteCost =
        (static_cast<double>(metrics.tokens.cacheWrite) / 1'000'000.0) *
        model.pricePer1MCacheWrite;
    totalCost += cacheWriteCost;
  }

  metrics.estimatedCostUsd = totalCost;
}

void BaseAnthropicProvider::generateSummary(
    const std::string &modelId, const AgentHistory & /* history */,
    const std::string &compactionPrompt,
    std::function<void(const StreamEvent &)> onEvent,
    std::atomic<bool> *abortSignal) {

  // Create a simple history with just the compaction prompt
  AgentHistory summaryHistory;
  summaryHistory.threadId = "summary";

  AgentTurn turn;
  Message msg;
  msg.role = Role::User;
  msg.content.push_back(TextContent{compactionPrompt});
  turn.messages.push_back(msg);
  summaryHistory.turns.push_back(turn);

  ProviderOptions opts;
  opts.modelId = modelId;
  opts.temperature = 0.3f;
  opts.maxTokens = 1000;
  opts.abortSignal = abortSignal;

  stream(summaryHistory, opts, onEvent);
}

} // namespace firmius::provider
