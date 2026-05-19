#ifndef FIRMIUS_TUI_QUICKTOOLCLUSTERITEM_HPP
#define FIRMIUS_TUI_QUICKTOOLCLUSTERITEM_HPP

#include "TranscriptItem.hpp"

#include <string>
#include <vector>

namespace firmius::tui {

/// Groups fast "read-only" tools (Read/Grep/Glob/List) into one compact block.
class QuickToolClusterItem : public TranscriptItem {
public:
  QuickToolClusterItem() = default;
  std::string_view type() const override { return "QuickToolCluster"; }
  std::vector<std::string> render(int width) const override;
  int rowCount(int width) const override;

  void setAgentId(const std::string& id) { agentId_ = id; }
  const std::string& agentId() const { return agentId_; }

  void addOrUpdateCall(const std::string& toolCallId,
                       const std::string& toolName,
                       const std::string& args,
                       bool inFlight);
  void setResult(const std::string& toolCallId, bool success, const std::string& result);
  void finalize();
  bool isFinalized() const override { return finalized_; }

private:
  struct Entry {
    std::string toolCallId;
    std::string toolName;
    std::string args;
    std::string label;
    bool inFlight = true;
    bool hasResult = false;
    bool success = false;
  };

  std::string agentId_;
  std::vector<Entry> entries_;
  bool finalized_ = false;
};

} // namespace firmius::tui

#endif

