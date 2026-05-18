#ifndef FIRMIUS_CORE_DEFLATOR_HPP
#define FIRMIUS_CORE_DEFLATOR_HPP

#include "Context.hpp"
#include "ITokenizer.hpp"
#include "agents/working_memory/DeflationArchive.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace firmius::core::working_memory {

/**
 * @brief One tool-result body identified as eligible for deflation.
 */
struct DeflationCandidate {
  std::size_t turnIndex = 0;       ///< Index in history.turns.
  std::size_t messageIndex = 0;    ///< Index in turn.messages.
  std::size_t partIndex = 0;       ///< Index in message.content.
  std::string toolCallId;          ///< The toolCallId tying to its tool_call.
  std::string toolName;            ///< Tool name from the matching tool_call.
  std::string toolArgs;            ///< Tool args from the matching tool_call.
  std::uint32_t originalTokens = 0; ///< Tokens in the original body.
  std::uint32_t turnAge = 0;       ///< Distance from the newest turn (0 = newest).
  bool resultSuccess = true;
};

/**
 * @brief Inputs for candidate selection.
 */
struct DeflationSelectorInputs {
  /// Tool-result bodies smaller than this in tokens are not deflated.
  std::uint32_t minPartTokens = 200;
  /// Default per-tool turn-age horizon before eligibility.
  std::uint32_t defaultHorizon = 8;
  /// Per-tool overrides for the horizon.
  std::map<std::string, std::uint32_t> horizonsByTool;
  /// Pin decisions; only Evictable and SoftPin turns are considered for
  /// deflation. HardPin turns are never deflated.
  const std::vector<bool>* hardPinMask = nullptr;
};

/**
 * @brief Result of a deflation pass.
 */
struct DeflationResult {
  std::uint32_t deflatedPartCount = 0;
  std::uint32_t tokensSaved = 0;
  std::uint32_t tokensSpentOnSummaries = 0;
  std::vector<std::string> archiveIds; ///< Archive IDs created this pass.
};

/**
 * @brief Synchronous summarizer interface. Returns a short summary string
 * (not exceeding maxTokens worth of characters) of the given body. If the
 * function returns empty, the deflator falls back to a deterministic stub.
 */
using SummarizerFn = std::function<std::string(
    const std::string& toolName, const std::string& toolArgs,
    const std::string& body, std::uint32_t budgetTokens,
    std::atomic<bool>* abort)>;

/**
 * @brief Walk the history and produce candidates eligible for deflation.
 *
 * Eligibility: a tool result is a candidate if all of:
 *   - Its turn is NOT hard-pinned.
 *   - The tool result body's token count >= inputs.minPartTokens.
 *   - The turn's age (newestIdx - turnIdx) >= horizonForTool(toolName).
 *   - The body is not already deflated (does not start with "[deflated:").
 *
 * Candidates are ordered oldest-first (largest turnAge first), so the caller
 * can deflate aggressively from the back of the history toward the front.
 */
std::vector<DeflationCandidate>
selectDeflationCandidates(const shared::AgentHistory& history,
                          const shared::ITokenizer& tokenizer,
                          const DeflationSelectorInputs& inputs);

/**
 * @brief Deflate the candidate parts in-place inside `history`.
 *
 * Each part's `ToolResultContent.result` is replaced with a stub of the form
 *   "[deflated: <tool> <reason> | archive:<archiveId>] <summary>"
 * The original body is written to `archive` under `archiveId`.
 *
 * If `summarizer` is provided and the body is large enough, it is called to
 * produce a summary; otherwise a deterministic stub is used (length, line
 * counts, kind hint). The summarizer is called on the calling thread —
 * the caller is responsible for offloading to a background worker if
 * desired.
 */
DeflationResult deflateCandidates(shared::AgentHistory& history,
                                  DeflationArchive& archive,
                                  const std::vector<DeflationCandidate>& candidates,
                                  const shared::ITokenizer& tokenizer,
                                  SummarizerFn summarizer = nullptr,
                                  std::atomic<bool>* abort = nullptr);

/**
 * @brief Returns true if the given ToolResultContent.result string is the
 * stub produced by deflateCandidates() rather than original content.
 */
bool isDeflatedStub(const std::string& result);

/**
 * @brief Extract the archiveId from a deflated stub. Returns empty string
 * if the input is not a deflated stub.
 */
std::string extractArchiveId(const std::string& deflatedResult);

} // namespace firmius::core::working_memory

#endif
