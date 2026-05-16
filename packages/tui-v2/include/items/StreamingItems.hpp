#pragma once

#include "TranscriptItem.hpp"

#include <string>
#include <vector>

namespace firmius::tui2 {

/// Accumulates streaming assistant text. Updated in-place as deltas arrive.
class AgentTextItem : public TranscriptItem {
public:
  AgentTextItem() = default;
  std::string_view type() const override { return "AgentText"; }
  std::vector<std::string> render(int width) const override;
  int rowCount(int width) const override;

  /// Append a streaming delta. Marks dirty.
  void appendDelta(const std::string& delta);

  /// Finalize this text block (no more deltas). Marks dirty.
  void finalize();

  bool isFinalized() const override { return finalized_; }
  const std::string& accumulated() const { return accumulated_; }

  void setAgentId(const std::string& id) { agentId_ = id; }
  const std::string& agentId() const { return agentId_; }

private:
  std::string accumulated_;
  std::string agentId_;
  bool finalized_ = false;
};

/// Accumulates streaming thinking text. Updated in-place as deltas arrive.
class AgentThinkingItem : public TranscriptItem {
public:
  AgentThinkingItem() = default;
  std::string_view type() const override { return "AgentThinking"; }
  std::vector<std::string> render(int width) const override;
  int rowCount(int width) const override;

  /// Append a streaming delta. Marks dirty.
  void appendDelta(const std::string& delta);

  /// Finalize this thinking block. Marks dirty.
  void finalize();

  bool isFinalized() const override { return finalized_; }
  const std::string& accumulated() const { return accumulated_; }

  void setAgentId(const std::string& id) { agentId_ = id; }
  const std::string& agentId() const { return agentId_; }

private:
  std::string accumulated_;
  std::string agentId_;
  bool finalized_ = false;
};

} // namespace firmius::tui2
