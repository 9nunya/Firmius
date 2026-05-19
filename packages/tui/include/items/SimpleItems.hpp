#ifndef FIRMIUS_TUI_SIMPLEITEMS_HPP
#define FIRMIUS_TUI_SIMPLEITEMS_HPP

#include "TranscriptItem.hpp"

#include <string>

namespace firmius::tui {

/// A message sent by the user. Immutable after construction.
class UserMessageItem : public TranscriptItem {
public:
  explicit UserMessageItem(std::string text, std::string agentId = {},
                           std::string messageId = {}, bool queued = false);
  std::string_view type() const override { return "UserMessage"; }
  std::vector<std::string> render(int width) const override;
  int rowCount(int width) const override;
  const std::string& text() const { return text_; }
  const std::string& agentId() const { return agentId_; }
  const std::string& messageId() const { return messageId_; }
  bool queued() const { return queued_; }
  void setQueued(bool queued);
private:
  std::string text_;
  std::string agentId_;
  std::string messageId_;
  bool queued_ = false;
};

/// An error message. Immutable after construction.
class ErrorMessageItem : public TranscriptItem {
public:
  explicit ErrorMessageItem(std::string text);
  std::string_view type() const override { return "ErrorMessage"; }
  std::vector<std::string> render(int width) const override;
  int rowCount(int width) const override;
  const std::string& text() const { return text_; }
private:
  std::string text_;
};

/// A system notice. Immutable after construction.
class SystemNoticeItem : public TranscriptItem {
public:
  explicit SystemNoticeItem(std::string text);
  std::string_view type() const override { return "SystemNotice"; }
  std::vector<std::string> render(int width) const override;
  int rowCount(int width) const override;
  const std::string& text() const { return text_; }
private:
  std::string text_;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_SIMPLEITEMS_HPP
