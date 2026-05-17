#pragma once

#include "TranscriptItem.hpp"

#include <string>

namespace firmius::tui2 {

/// A message sent by the user. Immutable after construction.
class UserMessageItem : public TranscriptItem {
public:
  explicit UserMessageItem(std::string text, std::string agentId = {});
  std::string_view type() const override { return "UserMessage"; }
  std::vector<std::string> render(int width) const override;
  int rowCount(int width) const override;
  const std::string& text() const { return text_; }
  const std::string& agentId() const { return agentId_; }
private:
  std::string text_;
  std::string agentId_;
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

} // namespace firmius::tui2
