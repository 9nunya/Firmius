#include "agents/working_memory/PinPolicy.hpp"

#include <algorithm>
#include <unordered_map>

namespace firmius::core::working_memory {

namespace {

std::uint32_t tokensForTurn(const shared::AgentTurn& turn,
                            const shared::ITokenizer& tok) {
  std::uint32_t total = 0;
  for (const auto& msg : turn.messages) {
    for (const auto& part : msg.content) {
      if (const auto* txt = std::get_if<shared::TextContent>(&part)) {
        total += tok.count(txt->text);
      } else if (const auto* th = std::get_if<shared::ThinkingContent>(&part)) {
        total += tok.count(th->thinking);
      } else if (const auto* tc = std::get_if<shared::ToolCallContent>(&part)) {
        total += tok.count(tc->name);
        total += tok.count(tc->args);
      } else if (const auto* tr = std::get_if<shared::ToolResultContent>(&part)) {
        total += tok.count(tr->result);
      } else if (const auto* nt = std::get_if<shared::NoticeContent>(&part)) {
        total += tok.count(nt->title);
        total += tok.count(nt->message);
        total += tok.count(nt->details);
      } else if (const auto* er = std::get_if<shared::ErrorContent>(&part)) {
        total += tok.count(er->errorName);
        total += tok.count(er->description);
        total += tok.count(er->details);
      } else if (std::holds_alternative<shared::ImageContent>(part)) {
        // Image cost varies by provider; conservative fixed estimate.
        total += 1024;
      }
    }
  }
  return total;
}

bool turnIsUserRole(const shared::AgentTurn& turn) {
  for (const auto& msg : turn.messages) {
    if (msg.role == shared::Role::User) {
      return true;
    }
  }
  return false;
}

bool turnHoldsImage(const shared::AgentTurn& turn) {
  for (const auto& msg : turn.messages) {
    for (const auto& part : msg.content) {
      if (std::holds_alternative<shared::ImageContent>(part)) {
        return true;
      }
    }
  }
  return false;
}

bool turnReferencesEditedFile(
    const shared::AgentTurn& turn,
    const std::unordered_set<std::string>& editedFiles) {
  if (editedFiles.empty()) {
    return false;
  }
  for (const auto& msg : turn.messages) {
    for (const auto& part : msg.content) {
      const std::string* searchable = nullptr;
      if (const auto* tr = std::get_if<shared::ToolResultContent>(&part)) {
        searchable = &tr->result;
      } else if (const auto* tc = std::get_if<shared::ToolCallContent>(&part)) {
        searchable = &tc->args;
      }
      if (!searchable || searchable->empty()) {
        continue;
      }
      for (const auto& path : editedFiles) {
        if (path.empty()) {
          continue;
        }
        if (searchable->find(path) != std::string::npos) {
          return true;
        }
      }
    }
  }
  return false;
}

void upgradeIfEvictable(PinDecision& decision, PinKind newKind,
                       std::string reason, PinClassification& cls) {
  if (decision.kind == PinKind::HardPin) {
    return; // Hard pins can't be downgraded.
  }
  if (decision.kind == PinKind::SoftPin && newKind != PinKind::HardPin) {
    return; // Soft can only become hard.
  }
  // Adjust counters
  if (decision.kind == PinKind::Evictable) {
    cls.evictableTokens -= decision.estimatedTokens;
    cls.evictableCount -= 1;
  } else if (decision.kind == PinKind::SoftPin) {
    cls.softPinTokens -= decision.estimatedTokens;
    cls.softPinCount -= 1;
  }
  decision.kind = newKind;
  decision.reason = std::move(reason);
  if (newKind == PinKind::HardPin) {
    cls.hardPinTokens += decision.estimatedTokens;
    cls.hardPinCount += 1;
  } else if (newKind == PinKind::SoftPin) {
    cls.softPinTokens += decision.estimatedTokens;
    cls.softPinCount += 1;
  }
}

} // namespace

PinClassification classifyPins(const shared::AgentHistory& history,
                               const shared::AgentContext& context,
                               const PinPolicyInputs& inputs,
                               const shared::ITokenizer& tokenizer) {
  PinClassification cls;
  cls.decisions.reserve(history.turns.size());

  // Pre-compute token cost per turn.
  std::vector<std::uint32_t> tokens(history.turns.size(), 0);
  std::uint32_t totalTokens = 0;
  for (std::size_t i = 0; i < history.turns.size(); ++i) {
    tokens[i] = tokensForTurn(history.turns[i], tokenizer);
    totalTokens += tokens[i];
  }
  cls.totalTokens = totalTokens;

  // Recency tail: walk backwards, marking the trailing turns whose
  // cumulative token cost stays under the budget. Always keep at least 1
  // turn in the tail when history is non-empty.
  std::vector<bool> isInTail(history.turns.size(), false);
  if (!history.turns.empty()) {
    std::uint32_t tailAccum = 0;
    for (std::size_t i = history.turns.size(); i-- > 0;) {
      isInTail[i] = true;
      tailAccum += tokens[i];
      if (tailAccum >= inputs.recencyTailTokens && i + 1 < history.turns.size()) {
        break;
      }
    }
  }

  // Initial classification pass.
  for (std::size_t i = 0; i < history.turns.size(); ++i) {
    const auto& turn = history.turns[i];
    PinDecision decision;
    decision.turnId = turn.turnId;
    decision.turnIndex = i;
    decision.estimatedTokens = tokens[i];

    bool hard = false;
    std::string reason;

    // The bootstrap-system turn carries the persona prompt; pin it.
    if (turn.turnId == "bootstrap-system" ||
        turn.turnId.rfind("system-note-", 0) == 0) {
      hard = true;
      reason = "bootstrap_or_system_note";
    } else if (turnIsUserRole(turn)) {
      hard = true;
      reason = "user_message";
    } else if (turnHoldsImage(turn)) {
      hard = true;
      reason = "image_part";
    } else if (isInTail[i]) {
      hard = true;
      reason = "recency_tail";
    } else if (inputs.agentPinnedTurnIds.count(turn.turnId) > 0) {
      hard = true;
      reason = "agent_pinned";
    } else if (inputs.activeStateReferences.count(turn.turnId) > 0) {
      hard = true;
      reason = "active_state_reference";
    } else if (turnReferencesEditedFile(turn, inputs.editedFiles)) {
      hard = true;
      reason = "edited_file_reference";
    }

    if (hard) {
      decision.kind = PinKind::HardPin;
      decision.reason = std::move(reason);
      cls.hardPinTokens += decision.estimatedTokens;
      cls.hardPinCount += 1;
    } else {
      decision.kind = PinKind::Evictable;
      decision.reason = "evictable";
      cls.evictableTokens += decision.estimatedTokens;
      cls.evictableCount += 1;
    }
    cls.decisions.push_back(std::move(decision));
  }
  // Suppress unused-parameter warning while keeping the signature stable
  // for future contextual policies (active mode, persona-specific rules).
  (void)context;

  // Build tool-call <-> tool-result pairing maps. Same logic the v1 filter
  // had, but we only care about pinning, not skipping.
  std::unordered_map<std::string, std::size_t> callIdToTurn;
  std::unordered_map<std::string, std::size_t> resultIdToTurn;
  for (std::size_t i = 0; i < history.turns.size(); ++i) {
    for (const auto& msg : history.turns[i].messages) {
      for (const auto& part : msg.content) {
        if (const auto* tc = std::get_if<shared::ToolCallContent>(&part)) {
          if (!tc->id.empty()) {
            callIdToTurn[tc->id] = i;
          }
        } else if (const auto* tr =
                       std::get_if<shared::ToolResultContent>(&part)) {
          if (!tr->toolCallId.empty()) {
            resultIdToTurn[tr->toolCallId] = i;
          }
        }
      }
    }
  }

  // Cascading upgrade pass: if a turn is HardPin and references an evictable
  // partner via tool_call/tool_result pairing, upgrade the partner to SoftPin.
  // SoftPin still travels in the request but is allowed to be deflated.
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t i = 0; i < history.turns.size(); ++i) {
      auto& decision = cls.decisions[i];
      if (decision.kind == PinKind::Evictable) {
        continue;
      }
      for (const auto& msg : history.turns[i].messages) {
        for (const auto& part : msg.content) {
          if (const auto* tc = std::get_if<shared::ToolCallContent>(&part)) {
            auto it = resultIdToTurn.find(tc->id);
            if (it != resultIdToTurn.end()) {
              auto& partner = cls.decisions[it->second];
              if (partner.kind == PinKind::Evictable) {
                upgradeIfEvictable(partner, PinKind::SoftPin,
                                   "tool_result_pair_with_pinned_call", cls);
                changed = true;
              }
            }
          } else if (const auto* tr =
                         std::get_if<shared::ToolResultContent>(&part)) {
            auto it = callIdToTurn.find(tr->toolCallId);
            if (it != callIdToTurn.end()) {
              auto& partner = cls.decisions[it->second];
              if (partner.kind == PinKind::Evictable) {
                upgradeIfEvictable(partner, PinKind::SoftPin,
                                   "tool_call_pair_with_pinned_result", cls);
                changed = true;
              }
            }
          }
        }
      }
    }
  }

  return cls;
}

} // namespace firmius::core::working_memory
