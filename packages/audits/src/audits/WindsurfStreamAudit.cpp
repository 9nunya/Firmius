#include "audits/WindsurfStreamAudit.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/WindsurfProvider.hpp"

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <variant>

namespace firmius::audits {

using firmius::core::EnvLoader;
using firmius::provider::WindsurfProvider;
using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::AuditResult;
using firmius::shared::Message;
using firmius::provider::ProviderOptions;
using firmius::shared::Role;
using firmius::shared::StreamDone;
using firmius::shared::StreamError;
using firmius::shared::StreamEvent;
using firmius::shared::TextChunk;
using firmius::shared::TextContent;
using firmius::shared::ThinkingChunk;

std::string WindsurfStreamAudit::getId() const { return "windsurf_stream"; }

std::string WindsurfStreamAudit::getDescription() const {
  return "Smoke-test streaming for a curated set of Windsurf models "
         "(Opus 4.7, GPT-5.5, Sonnet 4.6, SWE-1.6).";
}

class ScopedEnvOverride {
public:
  ScopedEnvOverride(const char *name, const char *value)
      : name_(name), hadValue_(std::getenv(name) != nullptr),
        oldValue_(hadValue_ ? std::getenv(name) : "") {
    if (value) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }

  ~ScopedEnvOverride() {
    if (hadValue_) {
      setenv(name_.c_str(), oldValue_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

private:
  std::string name_;
  bool hadValue_ = false;
  std::string oldValue_;
};

namespace {

struct StreamResult {
  std::string modelId;
  bool ok = false;
  int httpStatus = 0;
  std::string errorMessage;
  std::size_t textBytes = 0;
  std::size_t thinkBytes = 0;
  std::size_t chunks = 0;
  std::int64_t firstChunkMs = -1;
  std::int64_t totalMs = -1;
  std::string firstSnippet;
  std::string finalSnippet;
};

StreamResult tryModel(WindsurfProvider &provider,
                      const std::string &modelId,
                      const std::string &prompt) {
  StreamResult r;
  r.modelId = modelId;

  AgentHistory hist;
  hist.threadId = "audit-stream";
  AgentTurn turn;
  Message userMsg;
  userMsg.id = "msg-0";
  userMsg.role = Role::User;
  userMsg.content.push_back(TextContent{prompt});
  turn.messages.push_back(std::move(userMsg));
  hist.turns.push_back(std::move(turn));

  ProviderOptions opts;
  opts.modelId = modelId;
  opts.temperature = 0.0f;

  auto t0 = std::chrono::steady_clock::now();
  std::atomic<bool> sawFirst{false};
  std::mutex mu;

  provider.stream(hist, opts, [&](const StreamEvent &ev) {
    auto now = std::chrono::steady_clock::now();
    if (!sawFirst.exchange(true)) {
      r.firstChunkMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - t0)
              .count();
    }
    std::lock_guard<std::mutex> g(mu);
    if (auto *t = std::get_if<TextChunk>(&ev)) {
      r.textBytes += t->delta.size();
      r.chunks++;
      if (r.firstSnippet.empty()) {
        r.firstSnippet = t->delta.substr(0, 80);
      }
      r.finalSnippet = t->delta.substr(0, 80);
    } else if (auto *th = std::get_if<ThinkingChunk>(&ev)) {
      r.thinkBytes += th->delta.size();
    } else if (auto *err = std::get_if<StreamError>(&ev)) {
      r.errorMessage = err->message;
      r.httpStatus = err->httpStatus;
    } else if (auto *done = std::get_if<StreamDone>(&ev)) {
      (void)done;
    }
  });

  auto t1 = std::chrono::steady_clock::now();
  r.totalMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  r.ok = r.errorMessage.empty() && r.textBytes > 0;
  return r;
}

void printRow(const StreamResult &r) {
  std::cout << std::left << std::setw(34) << r.modelId << " | "
            << (r.ok ? "OK " : "FAIL") << " | " << std::right << std::setw(6)
            << r.totalMs << "ms | first=" << std::setw(5) << r.firstChunkMs
            << "ms | bytes=" << std::setw(5) << r.textBytes
            << " think=" << std::setw(4) << r.thinkBytes << " chunks="
            << std::setw(3) << r.chunks;
  if (!r.ok) {
    std::cout << " | err=" << (r.httpStatus ? std::to_string(r.httpStatus) : "")
              << " " << r.errorMessage.substr(0, 120);
  } else if (!r.firstSnippet.empty()) {
    std::cout << " | snippet=\"" << r.firstSnippet << "\"";
  }
  std::cout << std::endl;
}

} // namespace

AuditResult WindsurfStreamAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();
  EnvLoader::load(".env.local");
  firmius::core::Engine::instance();

  auto base = firmius::provider::ProviderRegistry::instance().getProvider(
      WindsurfProvider::kProviderId);
  auto provider = std::dynamic_pointer_cast<WindsurfProvider>(base);
  if (!provider) {
    std::cerr << "windsurf provider not registered" << std::endl;
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  if (provider->getAccounts().empty()) {
    std::cerr << "no Windsurf account on file" << std::endl;
    result.exitCode = 2;
    result.passed = false;
    return result;
  }

  // Make sure discovery has run so the dynamic models we plan to test are
  // actually present in the cache.
  provider->fetchAndMergeModels(provider->getAccounts().front());

  bool remoteOnly = false;
  std::vector<std::string> targets;
  for (const auto &arg : args) {
    if (arg == "--remote") {
      remoteOnly = true;
      continue;
    }
    targets.push_back(arg);
  }
  if (targets.empty()) {
    targets = {
        // The exact ids the user asked about + a couple of legacy anchors.
        "claude-opus-4-7-medium",
        "claude-opus-4-6",
        "claude-sonnet-4-6",
        "gpt-5-5-medium",
        "gpt-5-5-low",
        "swe-1-6",
        "swe-1-6-fast",
        // Legacy anchor — should still work even after we replaced the
        // static catalog with discovery, because the dynamic cache holds
        // whatever the server currently exposes under that id.
        "claude-3.5-sonnet",
    };
  }

  const ScopedEnvOverride forceRemote(
      "FIRMIUS_WINDSURF_REMOTE", remoteOnly ? "1" : nullptr);

  std::string prompt =
      "Reply with EXACTLY this JSON and nothing else: "
      "{\"ok\":true,\"hello\":\"world\"}";

  std::cout << "mode=" << (remoteOnly ? "remote" : "local") << std::endl;
  std::cout << std::left << std::setw(34) << "model" << " | stat | "
            << "  total | first    | bytes (text/think/chunks) | snippet"
            << std::endl;
  std::cout << std::string(120, '-') << std::endl;

  std::vector<StreamResult> results;
  for (const auto &m : targets) {
    auto r = tryModel(*provider, m, prompt);
    printRow(r);
    results.push_back(std::move(r));
  }

  std::size_t passed = 0;
  for (const auto &r : results) if (r.ok) ++passed;
  std::cout << std::endl
            << passed << "/" << results.size() << " models streamed OK"
            << std::endl;

  result.exitCode = passed == results.size() ? 0 : 3;
  result.passed = passed == results.size();
  return result;
}

} // namespace firmius::audits
