#include "audits/CacheAudit.hpp"
#include "ConfigLoader.hpp"
#include "EnvLoader.hpp"
#include "Message.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"
#include "providers/ProviderRegistry.hpp"

#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

namespace {

constexpr auto kAgentTimeout = std::chrono::seconds(360);

struct ModelTarget {
  std::string providerId;
  std::string modelId;
  std::string variantName;
};

struct CacheProbeResult {
  std::string label;
  bool success = false;
  std::string error;
  uint32_t call1_prompt = 0;
  uint32_t call1_cacheRead = 0;
  uint32_t call1_cacheWrite = 0;
  uint32_t call2_prompt = 0;
  uint32_t call2_cacheRead = 0;
  uint32_t call2_cacheWrite = 0;
  bool cacheHitOnSecondCall = false;
  double hitRatio = 0.0;
};

// ~2000 tokens of stable system prompt content.
std::string buildLargeSystemPrompt() {
  std::ostringstream s;
  s << "You are a helpful coding assistant. You have deep expertise in "
       "systems programming, distributed systems, and compiler design. "
       "You follow best practices for C++20, Rust, and Go. You always "
       "provide complete, working code examples with proper error handling.\n\n";
  // Pad to ~2000 tokens with deterministic filler.
  for (int i = 0; i < 80; ++i) {
    s << "Rule " << (i + 1) << ": When writing code, always consider "
      << "edge cases, thread safety, and memory management. Use RAII "
      << "patterns and avoid raw pointers. Prefer std::unique_ptr and "
      << "std::shared_ptr for ownership semantics.\n";
  }
  return s.str();
}

struct ProbeState {
  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;
  bool hadError = false;
  std::string errorMessage;
  uint32_t lastPrompt = 0;
  uint32_t lastCacheRead = 0;
  uint32_t lastCacheWrite = 0;
  bool metricsReceived = false;
};

}  // namespace

std::string CacheAudit::getId() const { return "cache"; }

std::string CacheAudit::getDescription() const {
  return "Verify prompt-caching hits across configured providers by sending "
         "identical prompts twice and checking cacheRead > 0 on the second call";
}

shared::AuditResult CacheAudit::run(const std::vector<std::string> &args) {
  shared::AuditResult result;
  result.auditId = getId();
  result.passed = false;
  (void)args;

  Panic::init();
  EnvLoader::load(".env.local");

  const auto originalConfig = ConfigLoader::instance().getConfig();
  auto cleanup = [&]() {
    Harness::instance().shutdown();
    ConfigLoader::instance().updateConfig(originalConfig);
  };

  // Collect model targets from the configured router categories.
  std::vector<ModelTarget> targets;
  {
    const auto &config = ConfigLoader::instance().getConfig();
    std::set<std::string> seen;
    for (const auto &[catName, cat] : config.modelRouterCategories) {
      for (const auto &m : cat.models) {
        std::string key = m.providerId + "/" + m.modelId;
        if (seen.count(key)) continue;
        seen.insert(key);
        // Skip local models (no caching concept).
        if (m.providerId == "lmstudio") continue;
        targets.push_back({m.providerId, m.modelId, m.variantName});
      }
    }
  }

  // Also add the specific models the user asked about if not already present.
  auto ensureTarget = [&](const std::string &prov, const std::string &model,
                          const std::string &variant = "") {
    for (const auto &t : targets) {
      if (t.providerId == prov && t.modelId == model) return;
    }
    targets.push_back({prov, model, variant});
  };
  ensureTarget("antigravity", "gemini-3-flash");
  ensureTarget("antigravity", "gemini-3.1-pro");
  ensureTarget("codex", "gpt-5.4");
  ensureTarget("codex", "gpt-5.5");

  std::cout << "[CacheAudit] Probing " << targets.size() << " model targets\n";

  std::vector<CacheProbeResult> probeResults;

  auto &harness = Harness::instance();
  harness.init();

  const std::string systemPrompt = buildLargeSystemPrompt();
  const std::string userMessage = "What is 2+2? Reply with just the number.";

  for (const auto &target : targets) {
    CacheProbeResult probe;
    probe.label = target.providerId + "/" + target.modelId;
    if (!target.variantName.empty()) {
      probe.label += " (" + target.variantName + ")";
    }
    std::cout << "\n[CacheAudit] Testing: " << probe.label << "\n";

    // Check provider exists.
    try {
      provider::ProviderRegistry::instance().getProvider(target.providerId);
    } catch (...) {
      probe.error = "Provider not registered: " + target.providerId;
      probeResults.push_back(probe);
      continue;
    }

    // Configure harness for this model.
    auto cfg = ConfigLoader::instance().getConfig();
    cfg.defaultProviderId = target.providerId;
    cfg.defaultModelId = target.modelId;
    cfg.defaultModelVariant = target.variantName;
    cfg.defaultLeadPersona = "coder";
    cfg.dangerouslySkipPermissions = true;
    cfg.mcpServers.clear();
    ConfigLoader::instance().updateConfig(cfg);

    // Run two calls with the same system prompt. Each call gets up to
    // 2 retries on transient failure (no-metrics, network blip) since
    // the Code Assist endpoint specifically can be flaky on the first
    // hit of a cold model.
    //
    // Use a SINGLE thread for both calls so prompt_cache_key (derived
    // from threadId) stays stable. Different threads would produce
    // different cache keys and hide actual cache hits.
    constexpr int kRetriesPerCall = 2;
    const std::string workingDir = "/tmp";
    const std::string sharedThreadId =
        harness.newThread({}, workingDir, "coder");
    if (sharedThreadId.empty()) {
      probe.error = "Failed to create thread for probe";
      probeResults.push_back(probe);
      continue;
    }
    if (!target.variantName.empty()) {
      harness.switchModel(target.providerId, target.modelId,
                          target.variantName);
    } else {
      harness.switchModel(target.providerId, target.modelId);
    }

    for (int callIdx = 0; callIdx < 2; ++callIdx) {
      bool callSucceeded = false;
      std::string lastAttemptError;

      for (int attempt = 0; attempt <= kRetriesPerCall && !callSucceeded;
           ++attempt) {
        if (attempt > 0) {
          std::cout << "  Call " << (callIdx + 1) << " retry "
                    << attempt << " (last: " << lastAttemptError << ")\n";
          std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        ProbeState state;
        int subId = harness.subscribe([&](const AppEvent &ev) {
          std::visit(
              [&](auto &&e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, AgentMetricsStreamed>) {
                  std::lock_guard<std::mutex> lk(state.mtx);
                  if (e.metrics.tokens.prompt > 0 ||
                      e.metrics.tokens.cacheRead > 0) {
                    state.lastPrompt = e.metrics.tokens.prompt;
                    state.lastCacheRead = e.metrics.tokens.cacheRead;
                    state.lastCacheWrite = e.metrics.tokens.cacheWrite;
                    state.metricsReceived = true;
                  }
                } else if constexpr (std::is_same_v<T, AgentFinished>) {
                  std::lock_guard<std::mutex> lk(state.mtx);
                  state.done = true;
                  state.cv.notify_one();
                } else if constexpr (std::is_same_v<T, AgentError>) {
                  std::lock_guard<std::mutex> lk(state.mtx);
                  state.done = true;
                  state.hadError = true;
                  state.errorMessage = e.message;
                  state.cv.notify_one();
                } else if constexpr (std::is_same_v<T,
                                                    PermissionEscalationRequest>) {
                  harness.resolvePermissionEscalation(
                      e.requestId, PermissionResponse::AllowAlways);
                }
              },
              ev);
        });

        harness.send(userMessage);

        {
          std::unique_lock<std::mutex> lk(state.mtx);
          state.cv.wait_for(lk, kAgentTimeout, [&] { return state.done; });
        }

        harness.unsubscribe(subId);

        if (state.hadError) {
          lastAttemptError = "error: " + state.errorMessage;
          continue;
        }
        if (!state.metricsReceived) {
          lastAttemptError = "no metrics received (timeout or no usage block)";
          continue;
        }

        // Success.
        if (callIdx == 0) {
          probe.call1_prompt = state.lastPrompt;
          probe.call1_cacheRead = state.lastCacheRead;
          probe.call1_cacheWrite = state.lastCacheWrite;
        } else {
          probe.call2_prompt = state.lastPrompt;
          probe.call2_cacheRead = state.lastCacheRead;
          probe.call2_cacheWrite = state.lastCacheWrite;
        }
        std::cout << "  Call " << (callIdx + 1) << ": prompt="
                  << state.lastPrompt << " cacheRead=" << state.lastCacheRead
                  << " cacheWrite=" << state.lastCacheWrite << "\n";
        callSucceeded = true;
      }

      if (!callSucceeded) {
        probe.error = "Call " + std::to_string(callIdx + 1) +
                      " failed after " + std::to_string(kRetriesPerCall + 1) +
                      " attempts: " + lastAttemptError;
        break;
      }

      // Pause between calls to let the cache settle. Some providers
      // (Antigravity Gemini implicit caching) take 1-2 seconds before
      // the second call observes the warm cache. 500ms was too tight
      // for the Code Assist endpoint specifically.
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    if (probe.error.empty()) {
      probe.success = true;
      probe.cacheHitOnSecondCall = probe.call2_cacheRead > 0;
      if (probe.call2_prompt + probe.call2_cacheRead > 0) {
        probe.hitRatio = static_cast<double>(probe.call2_cacheRead) /
                         static_cast<double>(probe.call2_prompt +
                                            probe.call2_cacheRead);
      }
    }
    probeResults.push_back(probe);
  }

  // Report.
  std::ostringstream report;
  report << "\n=== CACHE AUDIT REPORT ===\n\n";
  report << std::left << std::setw(45) << "Model"
         << std::right << std::setw(8) << "C1_prmpt"
         << std::setw(8) << "C1_cR"
         << std::setw(8) << "C1_cW"
         << std::setw(8) << "C2_prmpt"
         << std::setw(8) << "C2_cR"
         << std::setw(8) << "C2_cW"
         << std::setw(8) << "Hit%"
         << "  Status\n";
  report << std::string(110, '-') << "\n";

  int passed = 0;
  int failed = 0;
  for (const auto &p : probeResults) {
    report << std::left << std::setw(45) << p.label;
    if (!p.success) {
      report << "  ERROR: " << p.error;
      ++failed;
    } else {
      report << std::right
             << std::setw(8) << p.call1_prompt
             << std::setw(8) << p.call1_cacheRead
             << std::setw(8) << p.call1_cacheWrite
             << std::setw(8) << p.call2_prompt
             << std::setw(8) << p.call2_cacheRead
             << std::setw(8) << p.call2_cacheWrite
             << std::setw(7) << std::fixed << std::setprecision(0)
             << (p.hitRatio * 100) << "%";
      if (p.cacheHitOnSecondCall) {
        report << "  ✅ HIT";
        ++passed;
      } else {
        report << "  ❌ MISS";
        ++failed;
      }
    }
    report << "\n";
  }
  report << std::string(110, '-') << "\n";
  report << "Passed: " << passed << "  Failed/Error: " << failed
         << "  Total: " << probeResults.size() << "\n";
  report << "===========================\n";

  result.output = report.str();
  result.exitCode = (failed == 0) ? 0 : 1;
  result.passed = (failed == 0);

  cleanup();
  return result;
}

}  // namespace firmius::audits
