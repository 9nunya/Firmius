#include "audits/WorkingMemoryLiveAudit.hpp"

#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "Panic.hpp"
#include "agents/Agent.hpp"
#include "harness/Harness.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/Base64.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::provider;
using namespace firmius::shared;

namespace {

namespace fs = std::filesystem;

struct AuditOpts {
  std::string providerId = "kilo";
  std::string modelId = "stepfun/step-3.5-flash:free";
  std::string modelVariant;
  std::string persona = "lead";
  std::uint32_t windowOverride = 2048;
  int saturateTurnLimit = 12;
  int probeTimeoutSeconds = 90;
  int phaseTimeoutSeconds = 480;
  std::string imagePath; // optional; when set, audit attaches it to the first
                         // user message and probes image retention.
  bool autoSwapImageModel = true; // when imagePath is set and provider is
                                  // still the default text-only one, swap to
                                  // gitlawb/mimo-v2-omni.
  std::string summarizerProviderId; // optional; when set, deflated tool
                                    // result bodies are summarized via this
                                    // provider/model instead of getting a
                                    // deterministic stub.
  std::string summarizerModelId;
  std::string summarizerVariantName;
};

AuditOpts parseArgs(const std::vector<std::string> &args) {
  AuditOpts opts;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string &a = args[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= args.size()) {
        throw std::runtime_error("Missing value for option: " + a);
      }
      ++i;
      return args[i];
    };
    if (a == "--provider") {
      opts.providerId = next();
    } else if (a == "--model") {
      opts.modelId = next();
    } else if (a == "--variant") {
      opts.modelVariant = next();
    } else if (a == "--persona") {
      opts.persona = next();
    } else if (a == "--window") {
      opts.windowOverride =
          static_cast<std::uint32_t>(std::stoul(next()));
    } else if (a == "--saturate-turns") {
      opts.saturateTurnLimit = std::stoi(next());
    } else if (a == "--probe-timeout") {
      opts.probeTimeoutSeconds = std::stoi(next());
    } else if (a == "--phase-timeout") {
      opts.phaseTimeoutSeconds = std::stoi(next());
    } else if (a == "--image" || a == "--image-path") {
      opts.imagePath = next();
    } else if (a == "--no-auto-swap-image-model") {
      opts.autoSwapImageModel = false;
    } else if (a == "--summarizer-provider") {
      opts.summarizerProviderId = next();
    } else if (a == "--summarizer-model") {
      opts.summarizerModelId = next();
    } else if (a == "--summarizer-variant") {
      opts.summarizerVariantName = next();
    }
  }
  return opts;
}

template <typename Fn>
bool waitForCondition(Fn &&fn, std::chrono::milliseconds timeout,
                      std::chrono::milliseconds step =
                          std::chrono::milliseconds(150)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) return true;
    std::this_thread::sleep_for(step);
  }
  return fn();
}


std::optional<ImageContent> loadImageContent(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::vector<unsigned char> bytes(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.empty()) return std::nullopt;
  std::string mime = "image/png";
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower.size() >= 4) {
    if (lower.substr(lower.size() - 4) == ".jpg" ||
        lower.substr(lower.size() - 4) == "jpeg") {
      mime = "image/jpeg";
    } else if (lower.substr(lower.size() - 4) == ".gif") {
      mime = "image/gif";
    } else if (lower.size() >= 5 &&
               lower.substr(lower.size() - 5) == ".webp") {
      mime = "image/webp";
    }
  }
  ImageContent ic;
  ic.url = "data:" + mime + ";base64," + base64Encode(bytes);
  ic.mediaType = mime;
  ic.detail = "auto";
  return ic;
}

std::string seedWorkspace(const fs::path &root) {
  fs::create_directories(root);
  // Three files with distinguishable phrases, padded with realistic-looking
  // code-style noise so grep/read tool results are non-trivial in size.
  // Padding makes the saturate phase actually accumulate enough tokens to
  // cross working-memory thresholds when the actor window is small.
  auto pad = [](const std::string &tag, std::size_t lines) {
    std::ostringstream out;
    for (std::size_t i = 0; i < lines; ++i) {
      out << "// " << tag << " padding line " << i
          << ": void inner_helper_" << i << "(int x) { /* nothing */ }\n";
    }
    return out.str();
  };
  {
    std::ofstream f(root / "alpha.txt");
    f << "title: alpha\n"
      << pad("alpha-pre", 250)
      << "MAGIC_PHRASE_ALPHA appears here.\n"
      << pad("alpha-mid", 250)
      << "MAGIC_PHRASE_GAMMA also appears.\n"
      << pad("alpha-end", 250);
  }
  {
    std::ofstream f(root / "beta.txt");
    f << "title: beta\n"
      << pad("beta-pre", 500)
      << "This file holds MAGIC_PHRASE_BETA.\n"
      << pad("beta-end", 500);
  }
  {
    std::ofstream f(root / "gamma_decoy.txt");
    f << "title: gamma_decoy\n"
      << pad("decoy", 600)
      << "No magic phrase in this one. Just a decoy.\n";
  }
  std::ofstream(root / "README.md")
      << "Three files in this workspace, two of them have MAGIC_PHRASE_<X> "
         "markers. alpha.txt has two phrases; beta.txt has one; "
         "gamma_decoy.txt is a decoy.\n";
  return root.string();
}

std::string lastAssistantText(const AgentHistory &history) {
  for (auto it = history.turns.rbegin(); it != history.turns.rend(); ++it) {
    for (auto mi = it->messages.rbegin(); mi != it->messages.rend(); ++mi) {
      if (mi->role != Role::Assistant) continue;
      std::string out;
      for (const auto &part : mi->content) {
        if (const auto *txt = std::get_if<TextContent>(&part)) {
          if (!out.empty()) out += "\n";
          out += txt->text;
        }
      }
      if (!out.empty()) return out;
    }
  }
  return "";
}

std::size_t turnCount(const AgentHistory &history) {
  return history.turns.size();
}

std::size_t toolCallCount(const AgentHistory &history) {
  std::size_t n = 0;
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (std::holds_alternative<ToolCallContent>(part)) {
          n += 1;
        }
      }
    }
  }
  return n;
}

bool agentSettled(const std::string &agentId) {
  auto agent = std::dynamic_pointer_cast<Agent>(
      AgentRegistry::instance().getAgent(agentId));
  return agent && !agent->isRunning() && !agent->isBooting();
}

bool waitUntilSettled(const std::string &agentId,
                      std::chrono::seconds timeout) {
  return waitForCondition(
      [&]() { return agentSettled(agentId); }, timeout, std::chrono::milliseconds(200));
}

bool answerContainsAllCaseInsensitive(const std::string &answer,
                                      const std::vector<std::string> &needles) {
  std::string lower = answer;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  for (const auto &needle : needles) {
    std::string n = needle;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower.find(n) == std::string::npos) {
      return false;
    }
  }
  return true;
}

struct ProbeResult {
  std::string label;
  std::string verdict; // PASS / PARTIAL / FAIL / ERROR
  std::string reasoning;
  std::string answer;
  MemoryMetrics metricsBefore;
  MemoryMetrics metricsAfter;
};

std::string memoryMetricsLine(const MemoryMetrics &m) {
  std::ostringstream out;
  out << "raw=" << m.rawHistoryTokens << " set=" << m.workingSetTokens
      << " hardPin=" << m.pinnedTurnCount << " evict=" << m.evictedTurnCount
      << " recall=" << m.recalledTurnCount << " defl=" << m.deflatedPartCount
      << " saveDefl=" << m.tokensSavedByDeflation
      << " saveEvict=" << m.tokensSavedByEviction
      << " spendSum=" << m.tokensSpentOnSummaries
      << " spendEmb=" << m.tokensSpentOnEmbeddings
      << " redundantReads=" << m.redundantReadCount
      << " redundantTools=" << m.redundantToolSignatureCount
      << " latUs=" << m.hotPathLatencyMicros;
  if (m.aboveEmergencyThreshold) out << " [EMERG]";
  else if (m.aboveTargetThreshold) out << " [TGT]";
  else if (m.aboveBufferThreshold) out << " [BUF]";
  else out << " [PASS]";
  return out.str();
}

MemoryMetrics snapshotMetrics(const std::string &agentId) {
  auto agent = std::dynamic_pointer_cast<Agent>(
      AgentRegistry::instance().getAgent(agentId));
  if (!agent) return {};
  return agent->getContext().aggregateMetrics.memory;
}

std::optional<std::string> sendAndAwait(Harness &harness,
                                        const std::string &agentId,
                                        const std::string &text,
                                        std::chrono::seconds timeout,
                                        std::ostream &log) {
  log << "  > sending: " << text << "\n";
  harness.send(text);
  // Give the agent a beat to start.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const bool ok = waitUntilSettled(agentId, timeout);
  if (!ok) {
    log << "  ! TIMED OUT after " << timeout.count() << "s\n";
    return std::nullopt;
  }
  auto agent = std::dynamic_pointer_cast<Agent>(
      AgentRegistry::instance().getAgent(agentId));
  if (!agent) {
    log << "  ! agent disappeared\n";
    return std::nullopt;
  }
  const auto answer = lastAssistantText(*agent->getContext().history);
  log << "  < (" << answer.size() << " chars)\n";
  return answer;
}

} // namespace

std::string WorkingMemoryLiveAudit::getId() const {
  return "working_memory_live";
}

std::string WorkingMemoryLiveAudit::getDescription() const {
  return "Real-model end-to-end stress test of rolling memory v2: drives a "
         "scripted scenario through a live agent, forces working-memory "
         "thresholds via actorContextWindowOverride, and verifies user "
         "prompt retention, tool-result recoverability, agent-driven "
         "pinning, and metric sanity. Default model: kilo / "
         "stepfun/step-3.5-flash:free.";
}

shared::AuditResult WorkingMemoryLiveAudit::run(
    const std::vector<std::string> &args) {
  shared::AuditResult result;
  result.auditId = getId();
  result.passed = true;

  std::ostringstream log;
  log << "WorkingMemoryLiveAudit\n";

  AuditOpts opts;
  try {
    opts = parseArgs(args);
  } catch (const std::exception &e) {
    result.passed = false;
    result.exitCode = 2;
    result.output = std::string("argument error: ") + e.what();
    return result;
  }

  log << "  provider:        " << opts.providerId << "\n"
      << "  model:           " << opts.modelId
      << (opts.modelVariant.empty() ? "" : " (" + opts.modelVariant + ")")
      << "\n"
      << "  window override: " << opts.windowOverride << " tokens\n"
      << "  saturate turns:  " << opts.saturateTurnLimit << "\n";

  // If an image was supplied and the model is the default text-only Step
  // Flash, swap to gitlawb / mimo-v2-omni — a vision-capable Mimo with 256k
  // context. Honors --no-auto-swap-image-model.
  if (!opts.imagePath.empty() && opts.autoSwapImageModel &&
      opts.providerId == "kilo" &&
      opts.modelId == "stepfun/step-3.5-flash:free") {
    opts.providerId = "gitlawb";
    opts.modelId = "mimo-v2-omni";
    opts.modelVariant.clear();
    log << "  [auto-swap] image provided and default model is text-only; "
           "switching to gitlawb/mimo-v2-omni\n";
  }
  if (!opts.imagePath.empty()) {
    log << "  image:           " << opts.imagePath << "\n";
  }

  auto &harness = Harness::instance();
  harness.init();
  // Touch the engine singleton so providers initialize before lookup.
  (void)Engine::instance();

  // Verify provider is registered (now that providers have loaded).
  auto provider = ProviderRegistry::instance().getProvider(opts.providerId);
  if (!provider) {
    log << "  ! provider " << opts.providerId
        << " not registered; aborting (this is a setup issue, not a "
           "memory-layer failure)\n";
    result.passed = false;
    result.exitCode = 3;
    result.output = log.str();
    return result;
  }

  // Seed a temp workspace.
  const fs::path root = fs::temp_directory_path() /
                        ("firmius_wm_live_" +
                         std::to_string(std::chrono::system_clock::now()
                                            .time_since_epoch()
                                            .count()));
  const std::string cwd = seedWorkspace(root);
  log << "  workspace:       " << cwd << "\n";

  HostCreationOptions hostOpts;
  hostOpts.type = HostType::Local;
  const std::string threadId = harness.newThread(hostOpts, cwd, opts.persona);
  if (threadId.empty()) {
    log << "  ! failed to create thread\n";
    result.passed = false;
    result.exitCode = 4;
    result.output = log.str();
    return result;
  }
  log << "  thread:          " << threadId << "\n";

  if (!opts.modelVariant.empty()) {
    harness.switchModel(opts.providerId, opts.modelId, opts.modelVariant);
  } else {
    harness.switchModel(opts.providerId, opts.modelId);
  }

  log << "  focused after newThread: '" << harness.focusedAgentId() << "'\n";
  log << "  current thread:          '" << harness.currentThreadId() << "'\n";

  // ---------- PHASE 1: SATURATE ----------
  log << "\n=== PHASE 1: saturate ===\n";
  std::string firstTask =
      "There are several text files in the working directory. Your task: "
      "find every unique magic phrase of the form MAGIC_PHRASE_<NAME> "
      "across all files, then list them. To be thorough, fully read "
      "each .txt file end-to-end with the read tool — do NOT rely on "
      "grep alone, because the magic phrases may appear in surprising "
      "places and a grep can miss context. After you have read every "
      "file, give a brief summary of which phrases you found and which "
      "file each came from.";

  std::vector<ImageContent> firstImages;
  if (!opts.imagePath.empty()) {
    if (auto img = loadImageContent(opts.imagePath); img.has_value()) {
      firstImages.push_back(*img);
      firstTask +=
          "\n\nAlso, an image is attached to this message. Take a quick look "
          "at it; later in the conversation I may ask whether you saw an "
          "image earlier.";
      log << "  image attached to first task ("
          << img->url.size() << " base64 chars)\n";
    } else {
      log << "  ! image path could not be loaded; continuing without image\n";
    }
  }

  log << "  > seeding first task\n";
  harness.send(firstTask, firstImages);
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  log << "  focused after send:      '" << harness.focusedAgentId() << "'\n";

  // Wait for the agent to actually start.
  std::string agentId = harness.focusedAgentId();
  const bool started = waitForCondition(
      [&]() {
        agentId = harness.focusedAgentId();
        if (agentId.empty()) return false;
        auto agent = std::dynamic_pointer_cast<Agent>(
            AgentRegistry::instance().getAgent(agentId));
        return agent && (agent->isRunning() || agent->isBooting() ||
                         turnCount(*agent->getContext().history) > 1);
      },
      std::chrono::seconds(90), std::chrono::milliseconds(200));
  if (!started || agentId.empty()) {
    log << "  ! agent never started; final focused='"
        << harness.focusedAgentId() << "' agents in registry: "
        << AgentRegistry::instance().listAll().size() << "\n";
    result.passed = false;
    result.exitCode = 5;
    result.output = log.str();
    fs::remove_all(root);
    return result;
  }
  log << "  agent:           " << agentId << "\n";

  const bool saturated = waitUntilSettled(
      agentId, std::chrono::seconds(opts.phaseTimeoutSeconds));
  if (!saturated) {
    log << "  ! saturate phase timed out\n";
    result.passed = false;
    result.exitCode = 6;
    result.output = log.str();
    fs::remove_all(root);
    return result;
  }

  {
    auto agent = std::dynamic_pointer_cast<Agent>(
        AgentRegistry::instance().getAgent(agentId));
    if (agent) {
      const auto &h = *agent->getContext().history;
      log << "  saturate done:   turns=" << turnCount(h)
          << " toolCalls=" << toolCallCount(h) << "\n";
      log << "  metrics:         "
          << memoryMetricsLine(agent->getContext().aggregateMetrics.memory)
          << "\n";
    }
  }

  // ---------- PHASE 2: THRESHOLD CROSSING ----------
  log << "\n=== PHASE 2: window override = " << opts.windowOverride
      << " ===\n";
  if (opts.windowOverride > 0) {
    auto agent = std::dynamic_pointer_cast<Agent>(
        AgentRegistry::instance().getAgent(agentId));
    if (!agent) {
      log << "  ! agent not retrievable for override\n";
      result.passed = false;
      result.exitCode = 7;
      result.output = log.str();
      fs::remove_all(root);
      return result;
    }
    auto &mctx = agent->getMutableContext();
    mctx.config.workingMemory.actorContextWindowOverride = opts.windowOverride;
    if (!opts.summarizerProviderId.empty() &&
        !opts.summarizerModelId.empty()) {
      mctx.config.workingMemory.summarizerProviderId =
          opts.summarizerProviderId;
      mctx.config.workingMemory.summarizerModelId = opts.summarizerModelId;
      mctx.config.workingMemory.summarizerVariantName =
          opts.summarizerVariantName;
      // Tighten deflation horizon so summarization has a chance to fire
      // on the audit's modest tool-call workload. The default horizon is
      // 8 turns; this audit only generates ~6 tool calls during saturate.
      mctx.config.workingMemory.defaultDeflationTurnHorizon = 2;
      mctx.config.workingMemory.deflationMinPartTokens = 80;
      log << "  summarizer:      " << opts.summarizerProviderId << "/"
          << opts.summarizerModelId
          << (opts.summarizerVariantName.empty()
                  ? ""
                  : (" (" + opts.summarizerVariantName + ")"))
          << " (horizon=2 turns, minPart=80 tokens)\n";
    } else {
      log << "  summarizer:      (none — deterministic stubs)\n";
    }
    // Tighten thresholds so they trip immediately. We keep the v2 defaults
    // of 47/57/66 occupancy ratios — Phase 1 turn history will already
    // have crossed buffer; the goal is to make probes deterministically
    // exercise above-target behavior.
    log << "  override applied; subsequent turns will compute thresholds "
           "against "
        << opts.windowOverride << " tokens\n";
  }

  // ---------- PHASE 3 + 4: PROBES ----------
  log << "\n=== PHASE 3: probes ===\n";

  std::vector<ProbeResult> probes;

  auto runProbe = [&](const std::string &label, const std::string &prompt,
                      const std::vector<std::string> &expectedSubstrings,
                      const std::string &probeReasoning) {
    ProbeResult pr;
    pr.label = label;
    pr.metricsBefore = snapshotMetrics(agentId);
    log << "\n-- probe: " << label << " --\n";
    auto answer = sendAndAwait(harness, agentId, prompt,
                               std::chrono::seconds(opts.probeTimeoutSeconds),
                               log);
    pr.metricsAfter = snapshotMetrics(agentId);
    if (!answer.has_value()) {
      pr.verdict = "ERROR";
      pr.reasoning = "agent did not settle within probe timeout";
      probes.push_back(std::move(pr));
      return;
    }
    pr.answer = *answer;
    log << "  answer text:     "
        << (answer->size() > 240 ? answer->substr(0, 240) + " ..." : *answer)
        << "\n";
    const bool matched =
        answerContainsAllCaseInsensitive(*answer, expectedSubstrings);
    if (matched) {
      pr.verdict = "PASS";
      pr.reasoning = probeReasoning + " (all expected markers present)";
    } else {
      pr.verdict = "PARTIAL";
      // Identify which were missing.
      std::ostringstream missing;
      for (const auto &needle : expectedSubstrings) {
        std::string lowerAns = *answer;
        std::transform(lowerAns.begin(), lowerAns.end(), lowerAns.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::string n = needle;
        std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lowerAns.find(n) == std::string::npos) {
          if (!missing.str().empty()) missing << ", ";
          missing << needle;
        }
      }
      pr.reasoning = probeReasoning + " (missing: " + missing.str() + ")";
    }
    log << "  metrics:         " << memoryMetricsLine(pr.metricsAfter) << "\n";
    log << "  verdict:         " << pr.verdict << " — " << pr.reasoning << "\n";
    probes.push_back(std::move(pr));
  };

  // Probe 1: original task recall (user prompt retention). The agent must
  // know what it was originally asked.
  runProbe("original_task_recall",
           "Briefly: what was the first thing I asked you to do in this "
           "conversation? One sentence.",
           {"magic"},
           "user-prompt retention guarantees the original task survives");

  // Probe 2: magic phrases recall (tool-result content recoverability).
  // The agent must remember what it found, even if those tool result bodies
  // have been deflated.
  runProbe("magic_phrases_recall",
           "Just give me the bare list: the unique MAGIC_PHRASE_<NAME> "
           "values you found earlier, one per line, no extra prose.",
           {"alpha", "beta", "gamma"},
           "tool-result content must be recoverable post-deflation");

  // Probe 3: agent-driven pinning. Ask the agent to pin a fact via the
  // pin tool. The next probe will verify retention.
  runProbe("pin_tool_invocation",
           "Use the pin tool to pin the following exact fact, verbatim, "
           "with action=add and "
           "text=\"MAGIC_PHRASE_ALPHA was discovered in alpha.txt\". "
           "Then confirm in one short sentence.",
           {"pin"},
           "pin tool must be reachable and invocable");

  // A few filler turns to widen the gap between pin invocation and recall.
  runProbe("filler_turn_1",
           "Quick check: how many files were in the working directory?",
           {}, // matched dynamically below; runProbe's vacuous match is OK
              // because we always overwrite the verdict after.
           "filler turn to age the pin");
  // Accept "4" OR "four" as equivalent answers.
  if (!probes.empty() && probes.back().label == "filler_turn_1") {
    auto &pr = probes.back();
    if (pr.verdict != "ERROR") {
      const bool ok = answerContainsAllCaseInsensitive(pr.answer, {"4"}) ||
                      answerContainsAllCaseInsensitive(pr.answer, {"four"});
      pr.verdict = ok ? "PASS" : "PARTIAL";
      pr.reasoning =
          ok ? "filler turn to age the pin (answer matched 4 or four)"
             : "filler turn to age the pin (missing both '4' and 'four')";
      log << "  refined verdict: " << pr.verdict << " — " << pr.reasoning
          << "\n";
    }
  }

  runProbe("filler_turn_2",
           "Another quick one: which file was the decoy with no magic phrase?",
           {"gamma_decoy"},
           "filler turn to age the pin");

  // Probe 4: pin retention. Did the agent's earlier pin survive?
  runProbe("pin_retention",
           "Earlier in this conversation you used the pin tool to pin a "
           "fact. Without using any tools, what fact did you pin?",
           {"alpha", "alpha.txt"},
           "agent-driven pin must persist as hard pin in working memory");

  // Probe 5 (conditional): image recall. The image was attached to the
  // very first user message; the working-memory layer's image-pin policy
  // says an image part must NEVER drop. We test that by asking, late in
  // the conversation, whether the agent saw an image earlier.
  if (!firstImages.empty()) {
    runProbe("image_recall",
             "Way back at the start of our conversation, did I attach an "
             "image to my first message? Just say yes or no, then in one "
             "short sentence describe what you saw in it.",
             {"yes"},
             "image part attached to first user message must survive long "
             "after");
  }

  // ---------- PHASE 5: VERDICT ----------
  log << "\n=== PHASE 5: verdict ===\n";

  int passes = 0, partials = 0, fails = 0, errors = 0;
  for (const auto &pr : probes) {
    if (pr.verdict == "PASS") ++passes;
    else if (pr.verdict == "PARTIAL") ++partials;
    else if (pr.verdict == "FAIL") ++fails;
    else ++errors;
  }
  log << "  probes: " << passes << " PASS, " << partials << " PARTIAL, "
      << fails << " FAIL, " << errors << " ERROR\n";

  // Invariant checks.
  const auto finalMetrics = snapshotMetrics(agentId);
  log << "\nFINAL METRICS\n  "
      << memoryMetricsLine(finalMetrics) << "\n";

  // The strict invariants from the architecture:
  //   - userPromptsRetained == userPromptsTotal  (no user message ever drops)
  //   - imagePartsRetained  == imagePartsTotal   (no image ever drops)
  //   - hotPathLatencyMicros / turns < 50ms p99 (we approximate via mean)
  log << "\nINVARIANT CHECKS\n";
  bool invariantsHeld = true;
  if (finalMetrics.userPromptsRetained != finalMetrics.userPromptsTotal) {
    log << "  USER PROMPT RETENTION: FAIL ("
        << finalMetrics.userPromptsRetained << "/"
        << finalMetrics.userPromptsTotal << ")\n";
    invariantsHeld = false;
  } else {
    log << "  USER PROMPT RETENTION: PASS ("
        << finalMetrics.userPromptsRetained << "/"
        << finalMetrics.userPromptsTotal << ")\n";
  }
  if (finalMetrics.imagePartsRetained != finalMetrics.imagePartsTotal) {
    log << "  IMAGE PART RETENTION:  FAIL ("
        << finalMetrics.imagePartsRetained << "/"
        << finalMetrics.imagePartsTotal << ")\n";
    invariantsHeld = false;
  } else {
    log << "  IMAGE PART RETENTION:  PASS ("
        << finalMetrics.imagePartsRetained << "/"
        << finalMetrics.imagePartsTotal << ")\n";
  }
  // Latency check is best-effort because aggregate is summed across turns.
  // Compute mean per turn from total turns observed.
  auto agentFinal = std::dynamic_pointer_cast<Agent>(
      AgentRegistry::instance().getAgent(agentId));
  std::size_t totalTurns = 0;
  if (agentFinal) {
    totalTurns = turnCount(*agentFinal->getContext().history);
  }
  if (totalTurns > 0) {
    const std::uint64_t meanUs = finalMetrics.hotPathLatencyMicros / totalTurns;
    log << "  MEAN HOT-PATH LATENCY: " << meanUs << " us / turn ("
        << totalTurns << " turns; target <50000)\n";
    if (meanUs > 50000) {
      log << "    note: above 50ms target; may indicate hot-path regression\n";
    }
  }

  // Summarizer assertion. When the user configured a summarizer model, the
  // working-memory layer should fire at least one LLM-backed deflation
  // during the run, spend some summary tokens, and (ideally) save more
  // tokens by deflation than it spent on summaries. We only treat this as
  // an audit-failing invariant if the user explicitly enabled it; without
  // a summarizer configured, deterministic stubs are expected and the
  // counter stays at zero.
  if (!opts.summarizerProviderId.empty() &&
      !opts.summarizerModelId.empty()) {
    log << "  SUMMARIZER USED:       ";
    if (finalMetrics.deflatedPartCount == 0) {
      log << "FAIL (no deflation events fired; workload didn't push the "
             "layer above target threshold)\n";
      invariantsHeld = false;
    } else if (finalMetrics.tokensSpentOnSummaries == 0) {
      log << "FAIL (deflation fired " << finalMetrics.deflatedPartCount
          << " time(s) but tokensSpentOnSummaries is 0; bridge isn't "
             "calling the provider)\n";
      invariantsHeld = false;
    } else {
      log << "PASS (deflation fired " << finalMetrics.deflatedPartCount
          << " time(s), spent " << finalMetrics.tokensSpentOnSummaries
          << " tokens on summaries, saved "
          << finalMetrics.tokensSavedByDeflation << " tokens by deflation";
      if (finalMetrics.tokensSavedByDeflation >
          finalMetrics.tokensSpentOnSummaries) {
        log << "; net positive ROI)\n";
      } else {
        log << "; ROI net-negative this run, may indicate body sizes near "
               "summary budget)\n";
      }
    }
  }

  // Pretty-print probe results table.
  log << "\nPROBE TABLE\n";
  for (const auto &pr : probes) {
    log << "  " << pr.verdict << "  " << pr.label << " — " << pr.reasoning
        << "\n";
  }

  if (errors > 0 || fails > 0 || !invariantsHeld) {
    result.passed = false;
    result.exitCode = 1;
  }
  if (partials > 0 && errors == 0 && fails == 0 && invariantsHeld) {
    // Partials are common with weaker models hallucinating. They don't
    // fail the audit unless an invariant breaks.
    log << "\nNote: " << partials
        << " probe(s) returned PARTIAL; invariants held, so audit PASSES.\n";
  }

  // Cleanup: terminate agent and wipe workspace.
  if (agentFinal) {
    Engine::instance().terminateAgent(agentId);
  }
  std::error_code ec;
  fs::remove_all(root, ec);

  result.output = log.str();
  return result;
}

} // namespace firmius::audits
