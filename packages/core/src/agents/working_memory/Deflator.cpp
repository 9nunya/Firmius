#include "agents/working_memory/Deflator.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace firmius::core::working_memory {

namespace {

constexpr const char* kDeflatedPrefix = "[deflated:";

std::uint32_t horizonFor(const std::string& toolName,
                         const DeflationSelectorInputs& inputs) {
  auto it = inputs.horizonsByTool.find(toolName);
  if (it != inputs.horizonsByTool.end()) {
    return it->second;
  }
  return inputs.defaultHorizon;
}

std::string deterministicStub(const std::string& toolName,
                              const std::string& body,
                              const std::string& archiveId) {
  // Count lines and characters cheaply.
  std::size_t lines = 1;
  for (char c : body) {
    if (c == '\n') {
      ++lines;
    }
  }
  std::ostringstream out;
  out << kDeflatedPrefix << ' ';
  if (!toolName.empty()) {
    out << toolName << ' ';
  }
  out << "result, " << lines << " line";
  if (lines != 1) {
    out << 's';
  }
  out << ", " << body.size() << " bytes";
  out << " | archive:" << archiveId << "]";
  return out.str();
}

std::string summaryStub(const std::string& toolName,
                        const std::string& body,
                        const std::string& archiveId,
                        const std::string& summary) {
  std::ostringstream out;
  out << kDeflatedPrefix << ' ';
  if (!toolName.empty()) {
    out << toolName << ' ';
  }
  out << "result | archive:" << archiveId << "] ";
  // Bound the summary to a reasonable size in characters; the tokenizer
  // estimate the caller passed in is approximate.
  std::string s = summary;
  if (s.size() > 1024) {
    s.resize(1024);
    s += "...";
  }
  out << s;
  // Suppress unused warning while keeping the param for signature symmetry.
  (void)body;
  return out.str();
}

} // namespace

bool isDeflatedStub(const std::string& result) {
  return result.size() >= 11 &&
         result.compare(0, std::string::traits_type::length(kDeflatedPrefix),
                        kDeflatedPrefix) == 0;
}

std::string extractArchiveId(const std::string& deflatedResult) {
  if (!isDeflatedStub(deflatedResult)) {
    return "";
  }
  const std::string marker = "archive:";
  const auto start = deflatedResult.find(marker);
  if (start == std::string::npos) {
    return "";
  }
  const auto idStart = start + marker.size();
  const auto end = deflatedResult.find(']', idStart);
  if (end == std::string::npos) {
    return "";
  }
  return deflatedResult.substr(idStart, end - idStart);
}

std::vector<DeflationCandidate>
selectDeflationCandidates(const shared::AgentHistory& history,
                          const shared::ITokenizer& tokenizer,
                          const DeflationSelectorInputs& inputs) {
  std::vector<DeflationCandidate> candidates;
  if (history.turns.empty()) {
    return candidates;
  }

  // Build tool_call lookup: toolCallId -> (turnIdx, name, args).
  struct CallMeta {
    std::string name;
    std::string args;
  };
  std::unordered_map<std::string, CallMeta> callMeta;
  for (const auto& turn : history.turns) {
    for (const auto& msg : turn.messages) {
      for (const auto& part : msg.content) {
        if (const auto* tc = std::get_if<shared::ToolCallContent>(&part)) {
          if (!tc->id.empty()) {
            callMeta[tc->id] = {tc->name, tc->args};
          }
        }
      }
    }
  }

  const std::size_t newestIdx = history.turns.size() - 1;
  for (std::size_t i = 0; i < history.turns.size(); ++i) {
    if (inputs.hardPinMask && i < inputs.hardPinMask->size() &&
        (*inputs.hardPinMask)[i]) {
      continue;
    }
    const std::uint32_t age = static_cast<std::uint32_t>(newestIdx - i);
    const auto& turn = history.turns[i];
    for (std::size_t mi = 0; mi < turn.messages.size(); ++mi) {
      const auto& msg = turn.messages[mi];
      for (std::size_t pi = 0; pi < msg.content.size(); ++pi) {
        const auto& part = msg.content[pi];
        const auto* tr = std::get_if<shared::ToolResultContent>(&part);
        if (!tr) {
          continue;
        }
        if (isDeflatedStub(tr->result)) {
          continue;
        }
        const std::uint32_t bodyTokens = tokenizer.count(tr->result);
        if (bodyTokens < inputs.minPartTokens) {
          continue;
        }

        std::string toolName;
        std::string toolArgs;
        auto it = callMeta.find(tr->toolCallId);
        if (it != callMeta.end()) {
          toolName = it->second.name;
          toolArgs = it->second.args;
        }
        const std::uint32_t horizon = horizonFor(toolName, inputs);
        if (age < horizon) {
          continue;
        }

        DeflationCandidate cand;
        cand.turnIndex = i;
        cand.messageIndex = mi;
        cand.partIndex = pi;
        cand.toolCallId = tr->toolCallId;
        cand.toolName = std::move(toolName);
        cand.toolArgs = std::move(toolArgs);
        cand.originalTokens = bodyTokens;
        cand.turnAge = age;
        cand.resultSuccess = tr->success;
        candidates.push_back(std::move(cand));
      }
    }
  }

  // Order by largest savings first within an age bucket; older turns
  // first overall.
  std::sort(candidates.begin(), candidates.end(),
            [](const DeflationCandidate& a, const DeflationCandidate& b) {
              if (a.turnAge != b.turnAge) {
                return a.turnAge > b.turnAge;
              }
              return a.originalTokens > b.originalTokens;
            });

  return candidates;
}

DeflationResult deflateCandidates(shared::AgentHistory& history,
                                  DeflationArchive& archive,
                                  const std::vector<DeflationCandidate>& candidates,
                                  const shared::ITokenizer& tokenizer,
                                  SummarizerFn summarizer,
                                  std::atomic<bool>* abort) {
  DeflationResult result;
  for (const auto& cand : candidates) {
    if (abort && abort->load()) {
      break;
    }
    if (cand.turnIndex >= history.turns.size()) {
      continue;
    }
    auto& turn = history.turns[cand.turnIndex];
    if (cand.messageIndex >= turn.messages.size()) {
      continue;
    }
    auto& msg = turn.messages[cand.messageIndex];
    if (cand.partIndex >= msg.content.size()) {
      continue;
    }
    auto* tr = std::get_if<shared::ToolResultContent>(&msg.content[cand.partIndex]);
    if (!tr) {
      continue;
    }
    if (isDeflatedStub(tr->result)) {
      continue;
    }

    const std::string archiveId = archive.mintId(history.threadId);
    archive.put(archiveId, tr->result);

    std::string newBody;
    std::uint32_t summaryTokens = 0;
    if (summarizer) {
      const std::uint32_t budget =
          std::max<std::uint32_t>(60, cand.originalTokens / 8);
      std::string summary;
      try {
        summary = summarizer(cand.toolName, cand.toolArgs, tr->result, budget,
                             abort);
      } catch (...) {
        summary.clear();
      }
      if (!summary.empty()) {
        newBody = summaryStub(cand.toolName, tr->result, archiveId, summary);
        summaryTokens = tokenizer.count(newBody);
      }
    }
    if (newBody.empty()) {
      newBody = deterministicStub(cand.toolName, tr->result, archiveId);
      summaryTokens = tokenizer.count(newBody);
    }

    const std::uint32_t saved =
        cand.originalTokens > summaryTokens ? cand.originalTokens - summaryTokens : 0u;
    result.deflatedPartCount += 1;
    result.tokensSaved += saved;
    result.tokensSpentOnSummaries += summaryTokens;
    result.archiveIds.push_back(archiveId);

    tr->result = std::move(newBody);
  }
  return result;
}

} // namespace firmius::core::working_memory
