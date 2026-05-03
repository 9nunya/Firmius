#include "components/ChatWindow.hpp"
#include "Context.hpp"
#include "Enums.hpp"
#include "Message.hpp"
#include "ThemeManager.hpp"
#include "components/ErrorDisplay.hpp"
#include "components/Markdown.hpp"
#include "components/ScrollableBox.hpp"
#include "components/TranscriptGrouping.hpp"
#include "components/ToolBlock.hpp"
#include "components/GlintEffect.hpp"
#include "components/DiffRenderer.hpp"
#include "components/SyntaxHighlighter.hpp"
#include "NotificationManager.hpp"
#include "utils/Clipboard.hpp"
#include "utils/Icons.hpp"
#include "utils/ToolSummaries.hpp"
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <limits>
#include <memory>
#include <sstream>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace firmius::tui {
void noteTuiChatWindowRebuild(std::chrono::nanoseconds) __attribute__((weak));
}

namespace {
inline void NoteTuiChatWindowRebuildIfAvailable(std::chrono::nanoseconds elapsed) {
  if (firmius::tui::noteTuiChatWindowRebuild) {
    firmius::tui::noteTuiChatWindowRebuild(elapsed);
  }
}
}

static std::string rolePrefix(firmius::shared::Role role) {
  using firmius::shared::Role;
  switch (role) {
  case Role::User:
    return "> ";
  case Role::Assistant:
    return "";
  case Role::System:
    return "# ";
  case Role::ToolResult:
    return "+ ";
  case Role::Error:
    return "! ";
  }
  return "? ";
}

namespace firmius::tui {
} // namespace firmius::tui

namespace {

constexpr int kChatTailPaddingLines = 3;

std::string transcriptPreview(const std::string &text) {
  return firmius::tui::ClampTranscriptTextForDisplay(text);
}

std::string normalizeRollingFieldValue(const std::string &value) {
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return normalized;
}

bool isObservationNotice(const firmius::shared::NoticeContent &notice) {
  if (!notice.rollingMetadata) {
    return false;
  }
  return normalizeRollingFieldValue(notice.rollingMetadata->eventKind) ==
         "observation";
}

int observationLifecycleRank(const std::string &lifecycle) {
  const auto normalized = normalizeRollingFieldValue(lifecycle);
  if (normalized == "start" || normalized == "buffering" ||
      normalized == "inprogress" || normalized == "in_progress") {
    return 0;
  }
  if (normalized == "complete" || normalized == "completed" ||
      normalized == "done" || normalized == "finished" ||
      normalized == "success") {
    return 1;
  }
  if (normalized == "activate" || normalized == "activated" ||
      normalized == "activation") {
    return 2;
  }
  return -1;
}

std::optional<std::string> observationRangeKey(
    const firmius::shared::RollingNoticeMetadata &meta) {
  if (!meta.sourceStartTurnId || !meta.sourceEndTurnId) {
    return std::nullopt;
  }
  return *meta.sourceStartTurnId + "\x1F" + *meta.sourceEndTurnId;
}

struct ObservationNoticeRenderState {
  int lifecycle_rank = -1;
  size_t sequence = 0;
};

struct HistoryRenderSignature {
  const firmius::shared::AgentHistory *history = nullptr;
  std::size_t turn_count = 0;
  std::size_t cheap_key = 0;
  bool show_internal_nudges = false;
  bool hide_errors = false;

  bool operator==(const HistoryRenderSignature &other) const {
    return history == other.history && turn_count == other.turn_count &&
           cheap_key == other.cheap_key &&
           show_internal_nudges == other.show_internal_nudges &&
           hide_errors == other.hide_errors;
  }

  bool operator!=(const HistoryRenderSignature &other) const {
    return !(*this == other);
  }
};

template <typename T>
void HashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

std::size_t BuildHistoryCheapRenderKey(
    const firmius::shared::AgentHistory *history) {
  if (!history) {
    return 0;
  }

  std::size_t key = history->turns.size();
  const auto sample_turn = [&](const firmius::shared::AgentTurn &turn) {
    HashCombine(key, turn.turnId);
    HashCombine(key, turn.messages.size());
    if (!turn.messages.empty()) {
      const auto &message = turn.messages.back();
      HashCombine(key, static_cast<int>(message.role));
      HashCombine(key, static_cast<int>(message.visibility));
      HashCombine(key, message.content.size());
      if (!message.content.empty()) {
        std::visit(
            [&](const auto &content) {
              using T = std::decay_t<decltype(content)>;
              HashCombine(key, typeid(T).hash_code());
              if constexpr (std::is_same_v<T, firmius::shared::TextContent>) {
                HashCombine(key, content.text.size());
              } else if constexpr (std::is_same_v<T, firmius::shared::ThinkingContent>) {
                HashCombine(key, content.thinking.size());
                HashCombine(key, content.signature.size());
              } else if constexpr (std::is_same_v<T, firmius::shared::ToolCallContent>) {
                HashCombine(key, content.id);
                HashCombine(key, content.name);
                HashCombine(key, content.args.size());
              } else if constexpr (std::is_same_v<T, firmius::shared::ToolResultContent>) {
                HashCombine(key, content.toolCallId);
                HashCombine(key, content.result.size());
                HashCombine(key, content.success);
              } else if constexpr (std::is_same_v<T, firmius::shared::ImageContent>) {
                HashCombine(key, content.url.size());
                HashCombine(key, content.mediaType.size());
              } else if constexpr (std::is_same_v<T, firmius::shared::ErrorContent>) {
                HashCombine(key, content.errorName);
                HashCombine(key, content.description.size());
              } else if constexpr (std::is_same_v<T, firmius::shared::NoticeContent>) {
                HashCombine(key, content.title);
                HashCombine(key, content.message.size());
                HashCombine(key, static_cast<int>(content.severity));
                if (content.rollingMetadata) {
                  HashCombine(key, content.rollingMetadata->eventKind);
                  HashCombine(key, content.rollingMetadata->lifecycle);
                  if (content.rollingMetadata->sourceStartTurnId) {
                    HashCombine(key, *content.rollingMetadata->sourceStartTurnId);
                  }
                  if (content.rollingMetadata->sourceEndTurnId) {
                    HashCombine(key, *content.rollingMetadata->sourceEndTurnId);
                  }
                  if (content.rollingMetadata->sourceTurnCount) {
                    HashCombine(key, *content.rollingMetadata->sourceTurnCount);
                  }
                  if (content.rollingMetadata->sourceChunkCount) {
                    HashCombine(key, *content.rollingMetadata->sourceChunkCount);
                  }
                  if (content.rollingMetadata->sourceTokens) {
                    HashCombine(key, *content.rollingMetadata->sourceTokens);
                  }
                  if (content.rollingMetadata->summaryTokens) {
                    HashCombine(key, *content.rollingMetadata->summaryTokens);
                  }
                  if (content.rollingMetadata->savedTokens) {
                    HashCombine(key, *content.rollingMetadata->savedTokens);
                  }
                }
              }
            },
            message.content.back());
      }
    }
  };

  sample_turn(history->turns.front());
  if (history->turns.size() > 1) {
    sample_turn(history->turns.back());
  }
  return key;
}

std::size_t HashTurn(const firmius::shared::AgentTurn &turn) {
  std::size_t key = 0;
  HashCombine(key, turn.turnId);
  HashCombine(key, turn.messages.size());
  for (const auto &msg : turn.messages) {
    HashCombine(key, msg.id);
    HashCombine(key, msg.timestamp);
    HashCombine(key, static_cast<int>(msg.role));
    HashCombine(key, msg.content.size());
    if (!msg.content.empty()) {
      std::visit(
          [&](const auto &content) {
            using T = std::decay_t<decltype(content)>;
            HashCombine(key, typeid(T).hash_code());
            if constexpr (requires { content.text; }) {
              HashCombine(key, content.text.size());
            } else if constexpr (requires { content.thinking; }) {
              HashCombine(key, content.thinking.size());
            } else if constexpr (requires { content.result; }) {
              HashCombine(key, content.result.size());
            } else if constexpr (requires { content.message; }) {
              HashCombine(key, content.message.size());
            }
          },
          msg.content.back());
    }
  }
  HashCombine(key, turn.metrics.timing.endMs);
  HashCombine(key, turn.metrics.context.sentTokens);
  HashCombine(key, turn.metrics.tokens.completion);
  return key;
}

HistoryRenderSignature BuildHistoryRenderSignature(
    const firmius::shared::AgentHistory *history, bool show_internal_nudges,
    bool hide_errors) {
  HistoryRenderSignature signature;
  signature.history = history;
  signature.show_internal_nudges = show_internal_nudges;
  signature.hide_errors = hide_errors;
  if (!history) {
    return signature;
  }

  signature.turn_count = history->turns.size();
  signature.cheap_key = BuildHistoryCheapRenderKey(history);
  return signature;
}

std::string formatCompactCount(uint32_t value) {
  std::ostringstream out;
  if (value >= 1000000) {
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000000.0);
    std::string text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "M";
  }
  if (value >= 1000) {
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000.0);
    std::string text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "k";
  }
  return std::to_string(value);
}

std::string formatDurationMs(uint64_t durationMs) {
  const uint64_t totalSeconds = durationMs / 1000;
  const uint64_t minutes = totalSeconds / 60;
  const uint64_t seconds = totalSeconds % 60;
  std::ostringstream out;
  if (minutes > 0) {
    out << minutes << "m" << seconds << "s";
  } else {
    out << seconds << "s";
  }
  return out.str();
}

std::optional<std::string> buildTurnFooterSummary(
    const firmius::shared::AgentTurn &turn, size_t turn_number) {
  const auto &metrics = turn.metrics;
  const bool hasTiming = metrics.timing.endMs > metrics.timing.startMs;
  const bool hasTokens = metrics.context.sentTokens > 0 ||
                         metrics.tokens.completion > 0 ||
                         metrics.context.rawPromptTokens > 0 ||
                         metrics.context.billedPromptTokens > 0;
  if (!hasTiming && !hasTokens) {
    return std::nullopt;
  }

  std::ostringstream out;
  out << firmius::shared::ICON_CHECK << " done";
  if (turn_number > 0) {
    out << " · turn " << turn_number;
  }
  if (hasTiming) {
    out << " · " << formatDurationMs(metrics.timing.endMs -
                                     metrics.timing.startMs);
  }
  if (hasTokens) {
    out << " · \xE2\x86\x91" << formatCompactCount(metrics.context.sentTokens);
    if (metrics.context.billedPromptTokens > 0 &&
        metrics.context.billedPromptTokens != metrics.context.sentTokens) {
      out << "/" << formatCompactCount(metrics.context.billedPromptTokens);
    }
    out << " \xE2\x86\x93" << formatCompactCount(metrics.tokens.completion);
  }
  return out.str();
}

class RowComponent : public ftxui::ComponentBase {
public:
  RowComponent(ftxui::Component child, std::function<ftxui::Element()> render)
      : child_(std::move(child)), render_(std::move(render)) {
    if (child_)
      Add(child_);
  }

  ftxui::Element OnRender() override {
    // Null-Element guard: a null shared_ptr<Node> piped into any decorator
    // (selectionStyleReset, color, xflex, ...) builds a NodeDecorator with a
    // null child[0] and segfaults on SetBox during render.
    ftxui::Element rendered = render_ ? render_() : ftxui::Element{};
    if (!rendered) {
      rendered = ftxui::text("");
    }
    return ftxui::selectionStyleReset(rendered);
  }
  const ftxui::Box &box() const { return box_; }

  bool Focusable() const override { return false; }
  bool OnEvent(ftxui::Event event) override {
    if (child_)
      return child_->OnEvent(event);
    return false;
  }

private:
  ftxui::Component child_;
  std::function<ftxui::Element()> render_;
  ftxui::Box box_;
};

class CopyableRowComponent : public ftxui::ComponentBase {
public:
  CopyableRowComponent(std::function<ftxui::Element(bool)> render,
                       std::shared_ptr<const std::string> copy_text)
      : render_(std::move(render)), copy_text_(std::move(copy_text)) {}

  ftxui::Element OnRender() override {
    auto element = render_ ? render_(false) : ftxui::text("");
    if (!element) {
      element = ftxui::text("");
    }
    return element | ftxui::selectionBackgroundColor(ftxui::Color::RGB(72, 96, 152)) |
           ftxui::selectionForegroundColor(ftxui::Color::RGB(245, 247, 252)) |
           ftxui::reflect(box_);
  }

  bool Focusable() const override { return false; }

  const ftxui::Box &box() const { return box_; }
  const std::string &copyText() const {
    static const std::string empty;
    return copy_text_ ? *copy_text_ : empty;
  }

private:
  std::function<ftxui::Element(bool)> render_;
  std::shared_ptr<const std::string> copy_text_;
  ftxui::Box box_;
};

struct QuickToolCluster {
  std::vector<std::string> order;
  std::unordered_map<std::string, std::shared_ptr<firmius::shared::ToolCallView>>
      views;
  std::unordered_map<std::string, bool> failed_tools;
  std::unordered_map<std::string, bool> successful_tools;

  bool empty() const { return order.empty(); }

  void add(const std::shared_ptr<firmius::shared::ToolCallView> &view,
           bool is_success = true) {
    if (!view) {
      return;
    }
    if (!views.count(view->toolCallId)) {
      order.push_back(view->toolCallId);
    }
    views[view->toolCallId] = view;
    if (is_success) {
      successful_tools[view->toolCallId] = true;
    } else {
      failed_tools[view->toolCallId] = true;
    }
  }

  void clear() {
    order.clear();
    views.clear();
    failed_tools.clear();
    successful_tools.clear();
  }
};

ftxui::Element RenderQuickToolRow(const firmius::tui::QuickToolGroupSummary &summary,
                                  const firmius::tui::Theme &theme) {
  const bool live = summary.has_preparing || summary.has_live;
  std::string action;
  std::string icon = firmius::shared::ICON_TOOL;
  switch (summary.category) {
  case firmius::tui::QuickToolCategory::Read:
    icon = firmius::shared::ICON_FILE;
    action = live ? "reading" : "read";
    break;
  case firmius::tui::QuickToolCategory::List:
    icon = firmius::shared::ICON_FOLDER;
    action = live ? "listing" : "listed";
    break;
  case firmius::tui::QuickToolCategory::Search:
    icon = firmius::shared::ICON_SEARCH;
    action = live ? "searching" : "searched";
    break;
  case firmius::tui::QuickToolCategory::None:
    return ftxui::text("");
  }
  if (summary.has_error) {
    action = "failed";
  }
  const int live_count = summary.preparing_count + summary.live_count;

  const auto deduped_targets = firmius::tui::DedupeQuickToolTargets(summary.targets);

  std::string joined_targets;
  for (size_t i = 0; i < deduped_targets.size(); ++i) {
    if (i > 0) {
      joined_targets += ", ";
    }
    joined_targets += deduped_targets[i];
  }

  auto pill_bg = theme.agent_strip.pills.tool_bg;
  auto pill_fg = theme.agent_strip.pills.tool_fg;
  auto target_color = theme.base.fg;

  if (summary.category == firmius::tui::QuickToolCategory::Search) {
    pill_bg = theme.tool_blocks.specific.wait.bg;
    pill_fg = theme.tool_blocks.specific.wait.fg;
  } else if (summary.category == firmius::tui::QuickToolCategory::List) {
    pill_bg = theme.tool_blocks.specific.ls.bg;
    pill_fg = theme.tool_blocks.specific.ls.fg;
  } else if (summary.category == firmius::tui::QuickToolCategory::Read) {
    pill_bg = theme.tool_blocks.specific.file_read.bg;
    pill_fg = theme.tool_blocks.specific.file_read.fg;
  }

  if (summary.has_error) {
    pill_bg = theme.status_bar.error.normal.bg;
    pill_fg = theme.status_bar.error.normal.fg;
    target_color = theme.status_bar.error.normal.fg;
  } else if (summary.has_live) {
    pill_bg = theme.status_bar.streaming.normal.bg;
    pill_fg = theme.status_bar.streaming.normal.fg;
  } else if (summary.has_preparing) {
    pill_bg = theme.status_bar.executing_tool.normal.bg;
    pill_fg = theme.status_bar.executing_tool.normal.fg;
  }

  auto pill = ftxui::text(" " + icon + " " + action + " ") | ftxui::bold |
              ftxui::color(pill_fg) | ftxui::bgcolor(pill_bg);
  if (live) {
    firmius::tui::GlintConfig cfg;
    cfg.target = firmius::tui::GlintConfig::Target::Background;
    cfg.gradientColors = {pill_bg, theme.base.highlight};
    cfg.glintSize = 8;
    cfg.intervalSeconds = summary.has_live ? 1.8f : 2.8f;
    cfg.durationSeconds = summary.has_live ? 1.5f : 1.9f;
    cfg.easing = firmius::tui::GlintEasing::EaseInOut;
    pill = firmius::tui::GlintEffect(pill, cfg)->Render();
  }

  ftxui::Elements row_items = {
      pill,
      ftxui::text(" "),
      ftxui::paragraph(joined_targets.empty() ? "." : joined_targets) |
          ftxui::color(target_color) | ftxui::flex,
  };
  if (live && live_count > 1) {
    row_items.push_back(ftxui::text(" "));
    row_items.push_back(ftxui::text("\xC3\x97" + std::to_string(live_count)) |
                        ftxui::bold | ftxui::color(theme.base.highlight));
  }
  return ftxui::hbox(std::move(row_items)) | ftxui::xflex;
}

std::vector<ftxui::Component> BuildQuickToolClusterRows(
    const QuickToolCluster &cluster,
    const firmius::tui::LiveQuickSummaryCluster *live_cluster = nullptr) {
  std::vector<ftxui::Component> rows;
  if (cluster.empty() && (!live_cluster || live_cluster->summaries.empty())) {
    return rows;
  }

  std::vector<firmius::tui::QuickToolCategory> category_order;
  std::unordered_map<int, firmius::tui::QuickToolGroupSummary> summaries;
  std::vector<std::string> individual_failed_tool_call_ids;

  for (const auto &tool_call_id : cluster.order) {
    auto it = cluster.views.find(tool_call_id);
    if (it == cluster.views.end() || !it->second) {
      continue;
    }

    const auto descriptor = firmius::tui::DescribeQuickToolCall(*it->second);
    if (!firmius::tui::IsQuickToolCategory(descriptor.category)) {
      continue;
    }

    if (cluster.failed_tools.count(tool_call_id)) {
      individual_failed_tool_call_ids.push_back(tool_call_id);
      continue;
    }

    auto key = static_cast<int>(descriptor.category);
    auto &summary = summaries[key];
    if (summary.targets.empty()) {
      summary.category = descriptor.category;
      category_order.push_back(descriptor.category);
    }
    if (!descriptor.target.empty()) {
      summary.targets.push_back(descriptor.target);
    }
    summary.has_preparing =
        summary.has_preparing ||
        it->second->phase == firmius::shared::ToolPhase::Preparing;
    summary.has_live =
        summary.has_live || it->second->phase == firmius::shared::ToolPhase::Called;
    summary.has_error = false; // Grouped rows in this logic are only for success/live
  }

  if (live_cluster) {
    for (auto category : live_cluster->category_order) {
      auto key = static_cast<int>(category);
      if (summaries.count(key) == 0) {
        category_order.push_back(category);
      }
      auto &summary = summaries[key];
      if (summary.category == firmius::tui::QuickToolCategory::None) {
        summary.category = category;
      }
      auto it_live = live_cluster->summaries.find(key);
      if (it_live != live_cluster->summaries.end()) {
        const auto &live = it_live->second;
        summary.targets.insert(summary.targets.end(), live.targets.begin(),
                               live.targets.end());
        summary.has_preparing = summary.has_preparing || live.has_preparing;
        summary.has_live = summary.has_live || live.has_live;
        summary.has_error = summary.has_error || live.has_error;
        summary.preparing_count += live.preparing_count;
        summary.live_count += live.live_count;
      }
    }
    for (const auto &[key, live] : live_cluster->summaries) {
      if (summaries.count(key) > 0) {
        continue;
      }
      auto category = static_cast<firmius::tui::QuickToolCategory>(key);
      category_order.push_back(category);
      summaries[key] = live;
    }
  }

  if (category_order.empty() && !summaries.empty()) {
    std::vector<firmius::tui::QuickToolCategory> fallback = {
        firmius::tui::QuickToolCategory::Read,
        firmius::tui::QuickToolCategory::List,
        firmius::tui::QuickToolCategory::Search};
    for (auto category : fallback) {
      if (summaries.count(static_cast<int>(category)) > 0) {
        category_order.push_back(category);
      }
    }
  }

  for (auto category : category_order) {
    auto summary = summaries[static_cast<int>(category)];
    if (summary.category == firmius::tui::QuickToolCategory::None) {
      summary.category = category;
    }
    rows.push_back(ftxui::Make<RowComponent>(
        nullptr, [summary = std::move(summary)] {
          const auto &theme =
              firmius::tui::ThemeManager::instance().getCurrentTheme();
          return firmius::tui::IndentAgentRow(
              RenderQuickToolRow(summary, theme));
        }));
  }

  for (const auto &tool_call_id : individual_failed_tool_call_ids) {
    auto it = cluster.views.find(tool_call_id);
    if (it == cluster.views.end()) continue;
    firmius::tui::QuickToolGroupSummary failure_summary;
    const auto descriptor = firmius::tui::DescribeQuickToolCall(*it->second);
    failure_summary.category = descriptor.category;
    failure_summary.targets = {descriptor.target};
    failure_summary.has_error = true;
    rows.push_back(ftxui::Make<RowComponent>(
        nullptr, [summary = std::move(failure_summary)] {
          const auto &theme =
              firmius::tui::ThemeManager::instance().getCurrentTheme();
          return firmius::tui::IndentAgentRow(
              RenderQuickToolRow(summary, theme));
        }));
  }

  return rows;
}

class LazyToolBlock : public ftxui::ComponentBase {
public:
    LazyToolBlock(std::shared_ptr<firmius::tui::ToolCallView> view,
                  firmius::tui::HistoryGetter sub_history_getter,
                  firmius::tui::StreamGetter sub_stream_getter,
                  firmius::tui::ProcessStateGetter process_state_getter,
                  firmius::tui::SubagentStateGetter subagent_state_getter,
                  firmius::tui::AgentFocusHandler agent_focus_handler)
        : view_(std::move(view)), sub_history_getter_(std::move(sub_history_getter)),
          sub_stream_getter_(std::move(sub_stream_getter)),
          process_state_getter_(std::move(process_state_getter)),
          subagent_state_getter_(std::move(subagent_state_getter)),
          agent_focus_handler_(std::move(agent_focus_handler)) {}

    ftxui::Element OnRender() override {
        ensure();
        return block_ ? block_->Render() : ftxui::text("Preparing tool...");
    }
    bool OnEvent(ftxui::Event event) override {
        ensure();
        return block_ ? block_->OnEvent(event) : false;
    }
    bool Focusable() const override { return true; }
private:
    void ensure() {
        if (!block_) {
            block_ = firmius::tui::ToolBlock(view_, sub_history_getter_, sub_stream_getter_,
                                             process_state_getter_, subagent_state_getter_,
                                             agent_focus_handler_);
            Add(block_);
        }
    }
    std::shared_ptr<firmius::tui::ToolCallView> view_;
    firmius::tui::HistoryGetter sub_history_getter_;
    firmius::tui::StreamGetter sub_stream_getter_;
    firmius::tui::ProcessStateGetter process_state_getter_;
    firmius::tui::SubagentStateGetter subagent_state_getter_;
    firmius::tui::AgentFocusHandler agent_focus_handler_;
    ftxui::Component block_;
};

class ChatWindowComponent : public ftxui::ComponentBase {
public:
  explicit ChatWindowComponent(
      std::function<const firmius::shared::AgentHistory *()> history_getter,
      std::function<std::vector<ftxui::Element>()> live_rows_provider,
      firmius::tui::ToolViewProvider tool_view_provider,
      firmius::tui::ProcessStateGetter process_state_getter,
      firmius::tui::SubagentStateGetter subagent_state_getter,
      firmius::tui::AgentFocusHandler agent_focus_handler,
      firmius::tui::HistoryGetter sub_history_getter,
      firmius::tui::StreamGetter sub_stream_getter,
      firmius::tui::LiveQuickSummaryProvider live_quick_summary_provider,
      std::function<std::size_t()> live_measurement_signature_getter,
      std::function<bool()> show_internal_nudges_getter,
      std::function<bool()> hide_errors_getter,
      std::function<bool()> show_turn_footers_getter,
      firmius::tui::EditableModeEnabledGetter editable_mode_enabled_getter,
      firmius::tui::EditableMessageSelectedGetter editable_message_selected_getter,
      firmius::tui::EditableMessageClickHandler editable_message_click_handler)
      : history_getter_(std::move(history_getter)),
        live_rows_provider_(std::move(live_rows_provider)),
        tool_view_provider_(std::move(tool_view_provider)),
        process_state_getter_(std::move(process_state_getter)),
        subagent_state_getter_(std::move(subagent_state_getter)),
        agent_focus_handler_(std::move(agent_focus_handler)),
        sub_history_getter_(std::move(sub_history_getter)),
        sub_stream_getter_(std::move(sub_stream_getter)),
        live_quick_summary_provider_(std::move(live_quick_summary_provider)),
        live_measurement_signature_getter_(
            std::move(live_measurement_signature_getter)),
        show_internal_nudges_getter_(
            std::move(show_internal_nudges_getter)),
        hide_errors_getter_(std::move(hide_errors_getter)),
        show_turn_footers_getter_(std::move(show_turn_footers_getter)),
        editable_mode_enabled_getter_(std::move(editable_mode_enabled_getter)),
        editable_message_selected_getter_(std::move(editable_message_selected_getter)),
        editable_message_click_handler_(std::move(editable_message_click_handler)) {

    history_inner_ = ftxui::Container::Vertical({});
    history_container_ = ftxui::Renderer(history_inner_, [this] { return RenderHistoryWindow(); });

    tail_spacer_ =
        ftxui::Make<RowComponent>(nullptr, [this] {
          const bool show_turn_footers =
              show_turn_footers_getter_ ? show_turn_footers_getter_() : true;
          const int padding_lines = show_turn_footers ? kChatTailPaddingLines : 0;
          ftxui::Elements padding_rows;
          padding_rows.reserve(padding_lines > 0 ? padding_lines : 1);
          for (int i = 0; i < padding_lines; ++i) {
            padding_rows.push_back(ftxui::text(" "));
          }
          if (padding_rows.empty()) {
            padding_rows.push_back(ftxui::text(""));
          }
          return ftxui::vbox(std::move(padding_rows)) | ftxui::xflex;
        });
    auto live_rows_cmp = ftxui::Renderer([this] {
      if (!live_rows_provider_) {
        return ftxui::vbox(ftxui::Elements{});
      }
      // Live rows can be expensive (markdown render / tool row composition).
      // Avoid recomputing them on unrelated redraws (e.g. animation ticks for
      // other widgets) by caching against the same signature we use for scroll
      // measurement invalidation.
      const std::size_t sig = live_measurement_signature_getter_
                                  ? live_measurement_signature_getter_()
                                  : 0u;
      if (has_cached_live_rows_ && sig == last_live_rows_signature_) {
        return ftxui::vbox(cached_live_rows_);
      }

      // Drop any null Elements before they reach VBox::SetBox / Flex::SetBox.
      // A single bad row otherwise turns the whole frame into a SIGSEGV.
      auto rows = live_rows_provider_();
      rows.erase(std::remove_if(rows.begin(), rows.end(),
                                [](const ftxui::Element &e) { return !e; }),
                 rows.end());
      cached_live_rows_ = rows;
      last_live_rows_signature_ = sig;
      has_cached_live_rows_ = true;
      return ftxui::vbox(std::move(rows));
    });

    container_ = ftxui::Container::Vertical(
        {history_container_, live_rows_cmp, tail_spacer_});
    container_ = ftxui::Renderer(container_, [this, live_rows_cmp] {
      ftxui::Elements parts;
      parts.reserve(3);
      auto push = [&](ftxui::Element e) {
        if (e) parts.push_back(std::move(e));
      };
      push(history_container_->Render());
      push(live_rows_cmp->Render());
      push(tail_spacer_->Render());
      return ftxui::vbox(std::move(parts));
    });

    scrollable_ = firmius::tui::ScrollableBox(
        container_,
        {.startAtBottom = true,
         .overlayScrollbar = true,
         .measurement_signature_getter =
             [this]() {
               std::size_t seed = last_history_signature_.turn_count;
               HashCombine(seed, last_history_signature_.cheap_key);
               if (live_measurement_signature_getter_) {
                 HashCombine(seed, live_measurement_signature_getter_());
               }
               return seed;
             }});
    Add(scrollable_);

    if (scrollable_) {
        scrollable_->options().custom_size_getter = [this](int width) {
            return GetTotalHistoryHeight(width);
        };
    }
  }

  ftxui::Element OnRender() override {
    EnsureHistoryRows();
    if (pending_bottom_restore_ && scrollable_) {
      if (scrollable_->ViewportHeight() > 1 && scrollable_->ContentWidth() > 1) {
        scrollable_->TakeFocus();
        scrollable_->RequestScrollToBottom();
        pending_bottom_restore_ = false;
      }
    }
    return scrollable_ ? scrollable_->Render() : ftxui::text("");
  }

  bool Focusable() const override { return false; }

  bool OnEvent(ftxui::Event event) override {
    if (event == ftxui::Event::Special("TranscriptChanged")) {
      const bool should_follow_bottom =
          !scrollable_ || scrollable_->IsAtBottom() || pending_bottom_restore_;
      MarkHistoryDirty(false);
      EnsureHistoryRows();
      if (scrollable_) {
        scrollable_->TakeFocus();
        if (should_follow_bottom) {
          scrollable_->RequestScrollToBottom();
          pending_bottom_restore_ = true;
        } else {
          scrollable_->InvalidateLayout();
        }
      }
      return true;
    }

    if (event == ftxui::Event::Special("ThreadChanged") ||
        event == ftxui::Event::Special("ThemeChanged")) {
      if (event == ftxui::Event::Special("ThemeChanged")) {
        firmius::tui::ClearToolPresentationDiffCache();
        firmius::tui::ClearMarkdownCache();
        firmius::tui::SyntaxHighlighter::instance().clearRenderCache();
      }
      MarkHistoryDirty(true);
      EnsureHistoryRows();
      if (scrollable_) {
        scrollable_->RequestScrollToBottom();
        scrollable_->TakeFocus();
        pending_bottom_restore_ = true;
      }
      return true;
    }

    if (event == ftxui::Event::Custom) {
      if (pending_bottom_restore_ && scrollable_) {
        EnsureHistoryRows();
        if (scrollable_->ViewportHeight() <= 1 || scrollable_->ContentWidth() <= 1) {
          return true;
        }
        scrollable_->TakeFocus();
        scrollable_->RequestScrollToBottom();
        pending_bottom_restore_ = false;
        return true;
      }
      pending_bottom_restore_ = false;
    }

    const bool copy_handler_consumed = HandleCopySelection(event);
    EnsureHistoryRows();
    // Do not invalidate transcript layout for ordinary keypresses. The input
    // bar shares the event loop with chat; forcing a full chat re-measure on
    // every typed character makes large threads feel seconds behind.
    const bool handled = scrollable_ ? scrollable_->OnEvent(event) : false;
    FinalizePendingCopy();
    if (copy_drag_candidate_ && event.is_mouse() &&
        (event.mouse().button == ftxui::Mouse::WheelUp ||
         event.mouse().button == ftxui::Mouse::WheelDown)) {
      if (auto *screen = ftxui::ScreenInteractive::Active()) {
        auto resumed = event.mouse();
        resumed.button = ftxui::Mouse::Left;
        resumed.motion = ftxui::Mouse::Moved;
        resumed.x = last_drag_x_;
        resumed.y = last_drag_y_;
        screen->PostEvent(ftxui::Event::Mouse("", resumed));
      }
    }
    return copy_handler_consumed || handled;
  }

private:
  bool HandleCopySelection(ftxui::Event event) {
    if (!event.is_mouse()) {
      return false;
    }

    EnsureHistoryRows();
    const auto mouse = event.mouse();
    if (mouse.button != ftxui::Mouse::Left) {
      return false;
    }

    const int hovered = FindCopyableRowAt(mouse.x, mouse.y);
    if (mouse.motion == ftxui::Mouse::Pressed) {
      copy_drag_candidate_ = (hovered >= 0);
      copy_drag_started_ = false;
      press_x_ = mouse.x;
      press_y_ = mouse.y;
      last_drag_x_ = mouse.x;
      last_drag_y_ = mouse.y;
      return false;
    }

    if (!copy_drag_candidate_) {
      return false;
    }

    if (mouse.motion == ftxui::Mouse::Moved) {
      const int dx = std::abs(mouse.x - press_x_);
      const int dy = std::abs(mouse.y - press_y_);
      copy_drag_started_ = copy_drag_started_ || dx > 0 || dy > 0;
      last_drag_x_ = mouse.x;
      last_drag_y_ = mouse.y;
      return false;
    }

    if (mouse.motion == ftxui::Mouse::Released) {
      last_drag_x_ = mouse.x;
      last_drag_y_ = mouse.y;
      if (copy_drag_started_) {
        pending_copy_release_ = true;
        pending_release_x_ = mouse.x;
        pending_release_y_ = mouse.y;
      }
      copy_drag_candidate_ = false;
      copy_drag_started_ = false;
      return false;
    }

    return false;
  }

  int FindCopyableRowAt(int x, int y) const {
    for (size_t i = 0; i < copyable_rows_.size(); ++i) {
      if (!copyable_rows_[i]) {
        continue;
      }
      const auto &box = copyable_rows_[i]->box();
      if (y >= box.y_min && y <= box.y_max &&
          x >= box.x_min && x <= box.x_max) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  bool IsPointInCopyableRow(int x, int y) const {
    return FindCopyableRowAt(x, y) >= 0;
  }

  void FinalizePendingCopy() {
    if (!pending_copy_release_) {
      return;
    }
    pending_copy_release_ = false;
    auto *screen = ftxui::ScreenInteractive::Active();
    if (!screen) {
      return;
    }

    std::string copied = screen->GetSelection();
    if (copied.empty()) {
      copied = ExtractCopyableTextFromScreen(
          *screen, press_x_, press_y_, pending_release_x_, pending_release_y_);
    }

    if (!copied.empty() && Clipboard::setText(copied)) {
      firmius::tui::NotificationManager::instance().notifySuccess(
          "Copied!", "Transcript selection copied.",
          std::chrono::milliseconds(1200));
      ClearFrameworkSelection(*screen, pending_release_x_, pending_release_y_);
      return;
    }

    firmius::tui::NotificationManager::instance().notifyWarning(
        "Copy Failed", "Selection could not be copied.",
        std::chrono::milliseconds(1500));
  }

  std::string ExtractCopyableTextFromScreen(ftxui::ScreenInteractive &screen,
                                            int start_x, int start_y,
                                            int end_x, int end_y) const {
    const int x0 = std::clamp(start_x, 0, std::max(0, screen.dimx() - 1));
    const int y0 = std::clamp(start_y, 0, std::max(0, screen.dimy() - 1));
    const int x1 = std::clamp(end_x, 0, std::max(0, screen.dimx() - 1));
    const int y1 = std::clamp(end_y, 0, std::max(0, screen.dimy() - 1));

    const bool forward = (y0 < y1) || (y0 == y1 && x0 <= x1);
    const int top = forward ? y0 : y1;
    const int bottom = forward ? y1 : y0;

    std::vector<std::string> lines;
    for (int y = top; y <= bottom; ++y) {
      int left = 0;
      int right = screen.dimx() - 1;
      if (forward) {
        if (y == y0) left = x0;
        if (y == y1) right = x1;
      } else {
        if (y == y1) left = x1;
        if (y == y0) right = x0;
      }

      std::string line;
      for (int x = left; x <= right; ++x) {
        if (!IsPointInCopyableRow(x, y)) {
          line.push_back(' ');
          continue;
        }
        const std::string &cell = screen.at(x, y);
        line += cell.empty() ? " " : cell;
      }

      while (!line.empty() && line.back() == ' ') {
        line.pop_back();
      }
      lines.push_back(std::move(line));
    }

    while (!lines.empty() && lines.front().empty()) {
      lines.erase(lines.begin());
    }
    while (!lines.empty() && lines.back().empty()) {
      lines.pop_back();
    }

    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (i > 0) {
        out << '\n';
      }
      out << lines[i];
    }
    return out.str();
  }

  void ClearFrameworkSelection(ftxui::ScreenInteractive &screen, int x, int y) {
    ftxui::Mouse pressed;
    pressed.button = ftxui::Mouse::Left;
    pressed.motion = ftxui::Mouse::Pressed;
    pressed.x = x;
    pressed.y = y;

    ftxui::Mouse released = pressed;
    released.motion = ftxui::Mouse::Released;

    screen.PostEvent(ftxui::Event::Mouse("", pressed));
    screen.PostEvent(ftxui::Event::Mouse("", released));
  }

  void AddRow(ftxui::Component row) {
    rows_.push_back(std::move(row));
  }

  void AddCopyableRow(std::function<ftxui::Element(bool)> render,
                      std::shared_ptr<const std::string> copy_text) {
    auto row = ftxui::Make<CopyableRowComponent>(std::move(render),
                                                 std::move(copy_text));
    copyable_rows_.push_back(row);
    rows_.push_back(row);
  }

  static constexpr int kDefaultEstimatedRowHeight = 4;
  static constexpr int kVirtualizationOverscanLines = 12;

  int ReadMeasuredRowHeight(size_t index) const {
    if (index >= rows_.size()) {
      return kDefaultEstimatedRowHeight;
    }
    if (auto measured = std::dynamic_pointer_cast<RowComponent>(rows_[index])) {
      const auto &box = measured->box();
      if (box.y_max >= box.y_min) {
        return std::max(1, box.y_max - box.y_min + 1);
      }
    }
    if (auto copyable =
            std::dynamic_pointer_cast<CopyableRowComponent>(rows_[index])) {
      const auto &box = copyable->box();
      if (box.y_max >= box.y_min) {
        return std::max(1, box.y_max - box.y_min + 1);
      }
    }
    return row_height_cache_[index];
  }

  void RefreshCachedVisibleHeights() {
    if (row_height_cache_.empty() || last_visible_end_ <= last_visible_start_) {
      return;
    }
    const size_t end = std::min(last_visible_end_, row_height_cache_.size());
    for (size_t i = last_visible_start_; i < end; ++i) {
      row_height_cache_[i] = ReadMeasuredRowHeight(i);
    }
  }


  int GetTotalHistoryHeight(int width) {
    (void)width;
    if (row_height_cache_.empty() && !rows_.empty()) {
        row_height_cache_.assign(rows_.size(), kDefaultEstimatedRowHeight);
    }
    int total = 0;
    for (int h : row_height_cache_) {
      total += h;
    }
    return total;
  }

  ftxui::Element RenderHistoryWindow() {
    if (rows_.empty()) {
      return ftxui::text("");
    }
    if (row_height_cache_.size() != rows_.size()) {
      row_height_cache_.assign(rows_.size(), kDefaultEstimatedRowHeight);
    }

    RefreshCachedVisibleHeights();

    size_t start = 0;
    size_t end = rows_.size();
    int top_padding = 0;
    int bottom_padding = 0;
    if (scrollable_ && scrollable_->ViewportHeight() > 0) {
      const int viewport_height = scrollable_->ViewportHeight();
      const int scroll_offset = scrollable_->ScrollOffset();

      const bool has_measured_window =
          last_visible_end_ > last_visible_start_ &&
          last_visible_end_ <= row_height_cache_.size();

      const bool should_virtualize =
          has_measured_window &&
          rows_.size() > static_cast<size_t>(viewport_height * 3);

      if (should_virtualize) {
        const int history_height = GetTotalHistoryHeight(scrollable_->ContentWidth());
        const int anchor_offset = scrollable_->IsAtBottom()
                                      ? std::max(0, history_height - viewport_height)
                                      : scroll_offset;
        const int target_top =
            std::max(0, anchor_offset - kVirtualizationOverscanLines);
        const int target_bottom = anchor_offset + viewport_height +
                                  kVirtualizationOverscanLines;
        int cumulative = 0;
        start = rows_.size();
        end = rows_.size();
        for (size_t i = 0; i < row_height_cache_.size(); ++i) {
          const int next = cumulative + row_height_cache_[i];
          if (start == rows_.size() && next > target_top) {
            start = i;
            top_padding = cumulative;
          }
          if (next >= target_bottom) {
            end = i + 1;
            cumulative = next;
            break;
          }
          cumulative = next;
        }
        if (start == rows_.size()) {
          start = 0;
          top_padding = 0;
        }
        if (end == rows_.size()) {
          cumulative = 0;
          for (size_t i = 0; i < row_height_cache_.size(); ++i) {
            cumulative += row_height_cache_[i];
          }
        }
        if (end < start) {
          start = 0;
          end = rows_.size();
          top_padding = 0;
          bottom_padding = 0;
        } else {
          for (size_t i = end; i < row_height_cache_.size(); ++i) {
            bottom_padding += row_height_cache_[i];
          }
        }
      } else {
        start = 0;
        end = rows_.size();
        top_padding = 0;
        bottom_padding = 0;
      }
    }
    last_visible_start_ = start;
    last_visible_end_ = end;

    // Sync history_inner_ children with the visible window to keep event processing O(1)
    if (start != last_attached_start_ || end != last_attached_end_ || history_inner_->ChildCount() == 0) {
        history_inner_->DetachAllChildren();
        for (size_t i = start; i < end; ++i) {
            if (i < rows_.size()) {
                history_inner_->Add(rows_[i]);
            }
        }
        last_attached_start_ = start;
        last_attached_end_ = end;
    }

    ftxui::Elements elements;
    if (top_padding > 0) elements.push_back(ftxui::text("") | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, top_padding));
    
    for (size_t i = 0; i < history_inner_->ChildCount(); ++i) {
        auto e = history_inner_->ChildAt(i)->Render();
        if (e) {
          elements.push_back(std::move(e));
        }
    }

    if (bottom_padding > 0) elements.push_back(ftxui::text("") | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, bottom_padding));
    return ftxui::vbox(std::move(elements));
  }

  void RebuildIfNeeded() {
    const auto signature = currentHistorySignature();
    auto* history = signature.history;
    const bool showInternalNudges = signature.show_internal_nudges;
    const bool hideErrors = signature.hide_errors;

    int current_width = scrollable_ ? scrollable_->ContentWidth() : -1;
    if (current_width != last_rebuild_width_) {
      MarkHistoryDirty(true);
      last_rebuild_width_ = current_width;
    }

    if (!history_dirty_ && signature == last_history_signature_) {
      return;
    }
    history_dirty_ = false;
    last_history_signature_ = signature;

    if (scrollable_) {
      scrollable_->InvalidateLayout();
    }

    const auto rebuild_begin = std::chrono::steady_clock::now();

    // Pass 1: Observation suppression & turn hashing
    std::unordered_map<std::string, ObservationNoticeRenderState> latest_obs;
    size_t obs_seq = 0;
    std::vector<std::size_t> new_hashes;
    if (history) {
      new_hashes.reserve(history->turns.size());
      for (size_t i = 0; i < history->turns.size(); ++i) {
        const auto& t = history->turns[i];
        for (const auto& msg : t.messages) {
          if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges, t.turnId)) continue;
          for (const auto& part : msg.content) {
            auto* notice = std::get_if<firmius::shared::NoticeContent>(&part);
            if (notice && isObservationNotice(*notice)) {
              const auto& meta = *notice->rollingMetadata;
              const auto range_key = observationRangeKey(meta);
              const int lifecycle_rank = observationLifecycleRank(meta.lifecycle);
              if (range_key && lifecycle_rank >= 0) {
                const size_t seq = obs_seq++;
                auto& entry = latest_obs[*range_key];
                if (lifecycle_rank > entry.lifecycle_rank || (lifecycle_rank == entry.lifecycle_rank && seq >= entry.sequence)) {
                  entry.lifecycle_rank = lifecycle_rank;
                  entry.sequence = seq;
                }
              }
            }
          }
        }
      }

      obs_seq = 0;
      for (const auto& t : history->turns) {
        std::size_t h = HashTurn(t);
        for (const auto& msg : t.messages) {
          if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges, t.turnId)) continue;
          for (const auto& part : msg.content) {
            auto* notice = std::get_if<firmius::shared::NoticeContent>(&part);
            if (notice && isObservationNotice(*notice)) {
              const auto& meta = *notice->rollingMetadata;
              const auto range_key = observationRangeKey(meta);
              const int lifecycle_rank = observationLifecycleRank(meta.lifecycle);
              bool suppressed = false;
              if (range_key && lifecycle_rank >= 0) {
                const size_t seq = obs_seq++;
                auto it = latest_obs.find(*range_key);
                if (it != latest_obs.end()) {
                  suppressed = it->second.lifecycle_rank > lifecycle_rank || (it->second.lifecycle_rank == lifecycle_rank && it->second.sequence > seq);
                }
              }
              HashCombine(h, suppressed);
            }
          }
        }
        new_hashes.push_back(h);
      }
    }

    // Pass 2: Truncate cache at first mismatch
    size_t first_mismatch = 0;
    while (first_mismatch < new_hashes.size() && first_mismatch < turn_hashes_.size() && new_hashes[first_mismatch] == turn_hashes_[first_mismatch]) {
      first_mismatch++;
    }

    if (first_mismatch < turn_hashes_.size() || new_hashes.size() < turn_hashes_.size() || block_cache_.empty()) {
      size_t block_idx = 0;
      while (block_idx < block_cache_.size() && block_cache_[block_idx].turn_end < first_mismatch) {
        block_idx++;
      }
      if (block_idx < block_cache_.size()) {
        const auto& b = block_cache_[block_idx];
        rows_.erase(rows_.begin() + b.rows_start, rows_.end());
        copyable_rows_.erase(copyable_rows_.begin() + b.copyable_start, copyable_rows_.end());
        if (b.rows_start < row_height_cache_.size()) row_height_cache_.erase(row_height_cache_.begin() + b.rows_start, row_height_cache_.end());
        block_cache_.erase(block_cache_.begin() + block_idx, block_cache_.end());
        turn_hashes_.resize(b.turn_start);
      } else if (first_mismatch == 0 || block_cache_.empty()) {
        rows_.clear(); copyable_rows_.clear(); row_height_cache_.clear(); block_cache_.clear(); turn_hashes_.clear();
      }
    }

    // Pass 3: Process remaining turns incrementally
    if (history && turn_hashes_.size() < history->turns.size()) {
      size_t turn_index = turn_hashes_.size();
      std::unordered_map<std::string, bool> seen_tool_call;
      for (size_t i = 0; i < turn_index; ++i) {
        for (const auto& msg : history->turns[i].messages) {
          for (const auto& part : msg.content) {
            if (auto* tc = std::get_if<firmius::shared::ToolCallContent>(&part)) seen_tool_call[tc->id] = true;
          }
        }
      }

      QuickToolCluster quick_cluster;
      std::vector<std::string> pending_turn_footers;
      size_t block_turn_start = turn_index;
      size_t obs_pass_seq = 0; 

      // Re-calculate obs_pass_seq for the start_turn
      for (size_t i = 0; i < turn_index; ++i) {
          const auto& t = history->turns[i];
          for (const auto& msg : t.messages) {
              if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges, t.turnId)) continue;
              for (const auto& part : msg.content) {
                  auto* notice = std::get_if<firmius::shared::NoticeContent>(&part);
                  if (notice && isObservationNotice(*notice)) {
                      const auto& meta = *notice->rollingMetadata;
                      if (observationRangeKey(meta) && observationLifecycleRank(meta.lifecycle) >= 0) obs_pass_seq++;
                  }
              }
          }
      }

      auto flush_block = [&](size_t current_turn, bool final_merge = false) {
        size_t rows_before = rows_.size();
        size_t copyable_before = copyable_rows_.size();

        std::vector<firmius::tui::LiveQuickSummaryCluster> live_clusters;
        if (final_merge && live_quick_summary_provider_) live_clusters = live_quick_summary_provider_();
        const firmius::tui::LiveQuickSummaryCluster *merge_cluster = nullptr;
        size_t live_start = 0;
        if (!live_clusters.empty() && live_clusters.front().merge_with_history) {
          merge_cluster = &live_clusters.front();
          live_start = 1;
        }

        auto grouped_rows = BuildQuickToolClusterRows(quick_cluster, merge_cluster);
        for (auto &row : grouped_rows) rows_.push_back(row);
        quick_cluster.clear();

        const bool show_turn_footers = show_turn_footers_getter_ ? show_turn_footers_getter_() : true;
        std::vector<std::string> footers_to_render = (grouped_rows.empty() || pending_turn_footers.empty()) ? pending_turn_footers : std::vector<std::string>{pending_turn_footers.back()};
        for (const auto &footer_text : (show_turn_footers ? footers_to_render : std::vector<std::string>{})) {
          const auto &theme = firmius::tui::ThemeManager::instance().getCurrentTheme();
          rows_.push_back(ftxui::Make<RowComponent>(nullptr, [footer_text, theme] {
            return firmius::tui::IndentAgentRow(ftxui::text(footer_text) | ftxui::color(theme.chat.timestamp));
          }));
          rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
        }
        pending_turn_footers.clear();

        if (final_merge) {
          QuickToolCluster empty;
          for (size_t i = live_start; i < live_clusters.size(); ++i) {
            auto lrows = BuildQuickToolClusterRows(empty, &live_clusters[i]);
            for (auto &row : lrows) rows_.push_back(row);
          }
        }

        if (rows_.size() > rows_before) {
          CachedBlock b;
          b.turn_start = block_turn_start;
          b.turn_end = current_turn;
          b.rows_start = rows_before;
          b.rows_count = rows_.size() - rows_before;
          b.copyable_start = copyable_before;
          b.copyable_count = copyable_rows_.size() - copyable_before;
          block_cache_.push_back(b);
          block_turn_start = current_turn + 1;
        }
      };

      for (; turn_index < history->turns.size(); ++turn_index) {
        const auto &t = history->turns[turn_index];
        turn_hashes_.push_back(new_hashes[turn_index]);
        if (t.messages.empty()) continue;
        bool visible = false;
        for (const auto& m : t.messages) if (!firmius::tui::ShouldHideMessageInTranscript(m, showInternalNudges, t.turnId)) { visible = true; break; }
        if (!visible) continue;

        const bool isCompactionStart = (t.turnId.rfind("compaction-start-", 0) == 0);
        const bool isCompactionSummary = (t.turnId.rfind("compaction-summary-", 0) == 0);
        const bool isCompactionEnd = (t.turnId.rfind("compaction-end-", 0) == 0);

        if (isCompactionStart || isCompactionSummary || isCompactionEnd) {
          flush_block(turn_index - 1);
          size_t rb = rows_.size(); size_t cb = copyable_rows_.size();
          
          auto full_width_separator = [](const std::string &label) {
            return ftxui::hbox({ ftxui::filler() | ftxui::xflex, ftxui::text(" " + label + " ") | ftxui::dim, ftxui::filler() | ftxui::xflex }) | ftxui::xflex;
          };

          if (isCompactionStart || (isCompactionSummary && (turn_index == 0 || history->turns[turn_index-1].turnId.rfind("compaction-start-", 0) != 0))) {
            rows_.push_back(ftxui::Make<RowComponent>(nullptr, [full_width_separator] { return full_width_separator("Compaction"); }));
          }

          for (const auto &msg : t.messages) {
            if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges, t.turnId)) continue;
            for (const auto &part : msg.content) {
              if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
                auto text = std::make_shared<const std::string>(txt->text);
                AddCopyableRow([text](bool selected) { (void)selected; return firmius::tui::IndentAgentRow(firmius::tui::RenderMarkdown(transcriptPreview(*text))); }, text);
              } else if (auto *thk = std::get_if<firmius::shared::ThinkingContent>(&part)) {
                auto th = std::make_shared<const std::string>(thk->thinking);
                AddCopyableRow([th](bool selected) { (void)selected; return firmius::tui::IndentAgentRow(firmius::tui::RenderMarkdown(transcriptPreview(*th), true)); }, th);
              }
            }
          }
          if (isCompactionEnd || (isCompactionSummary && (turn_index + 1 >= history->turns.size() || history->turns[turn_index+1].turnId.rfind("compaction-end-", 0) != 0))) {
            rows_.push_back(ftxui::Make<RowComponent>(nullptr, [full_width_separator] { return full_width_separator("Compaction Complete"); }));
          }

          CachedBlock b; b.turn_start = turn_index; b.turn_end = turn_index; b.rows_start = rb; b.rows_count = rows_.size() - rb; b.copyable_start = cb; b.copyable_count = copyable_rows_.size() - cb;
          block_cache_.push_back(b); block_turn_start = turn_index + 1;
          continue;
        }

        for (const auto &msg : t.messages) {
          if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges, t.turnId)) continue;
          const auto &theme = firmius::tui::ThemeManager::instance().getCurrentTheme();
          const std::string prefix = rolePrefix(msg.role);
          bool isUser = (msg.role == firmius::shared::Role::User);
          ftxui::Color prefixColor = isUser ? theme.chat.user_prefix : theme.chat.agent_prefix;

          auto makeTag = [&theme](const std::string &label) {
            return ftxui::text(" " + label + " ") | ftxui::bold | ftxui::color(theme.base.bg) | ftxui::bgcolor(theme.base.highlight);
          };
          auto renderUserMessage = [prefixColor, prefix, theme, makeTag](const std::string &text, int image_count, bool selected) {
            ftxui::Elements body;
            body.push_back(ftxui::hbox({ ftxui::text(prefix) | ftxui::bold | ftxui::color(prefixColor), firmius::tui::RenderMarkdown(transcriptPreview(text)) | ftxui::xflex }) | ftxui::xflex);
            if (image_count > 0) {
              ftxui::Elements tags;
              for (int i = 0; i < image_count; ++i) tags.push_back(makeTag("IMAGE " + std::to_string(i + 1)));
              body.push_back(ftxui::hbox(std::move(tags)) | ftxui::xflex);
            }
            return ftxui::vbox({ ftxui::text(""), ftxui::vbox(std::move(body)) | ftxui::xflex, ftxui::text("") }) | ftxui::bgcolor(selected ? ftxui::Color::RGB(72, 96, 152) : theme.input.bg) | ftxui::xflex;
          };
          auto decorateMsg = [prefixColor, prefix, isUser, theme](const ftxui::Element &content) {
            auto e = ftxui::hbox({ ftxui::text(prefix) | ftxui::bold | ftxui::color(prefixColor), content | ftxui::xflex });
            return isUser ? e | ftxui::xflex : firmius::tui::IndentAgentRow(e);
          };

          if (isUser) {
            flush_block(turn_index - 1);
            size_t rb = rows_.size(); size_t cb = copyable_rows_.size();
            std::string user_text; int image_count = 0;
            for (const auto &part : msg.content) {
              if (const auto *txt = std::get_if<firmius::shared::TextContent>(&part)) { if (!user_text.empty()) user_text += "\n"; user_text += txt->text; }
              else if (std::holds_alternative<firmius::shared::ImageContent>(part)) ++image_count;
            }
            uint64_t msg_timestamp = msg.timestamp;
            auto user_text_ptr = std::make_shared<const std::string>(std::move(user_text));
            AddCopyableRow([this, renderUserMessage, user_text_ptr, image_count, msg_timestamp](bool selected) {
              bool is_edit = editable_message_selected_getter_ ? editable_message_selected_getter_(msg_timestamp) : false;
              return renderUserMessage(*user_text_ptr, image_count, selected || is_edit);
            }, user_text_ptr);
            rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
            CachedBlock b; b.turn_start = turn_index; b.turn_end = turn_index; b.rows_start = rb; b.rows_count = rows_.size() - rb; b.copyable_start = cb; b.copyable_count = copyable_rows_.size() - cb;
            block_cache_.push_back(b); block_turn_start = turn_index + 1;
            continue;
          }

          for (const auto &part : msg.content) {
            if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
              flush_block(turn_index - 1);
              size_t rb = rows_.size(); size_t cb = copyable_rows_.size();
              auto text = std::make_shared<const std::string>(txt->text);
              AddCopyableRow([decorateMsg, text](bool selected) { (void)selected; return decorateMsg(firmius::tui::RenderMarkdown(transcriptPreview(*text))); }, text);
              rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
              CachedBlock b; b.turn_start = turn_index; b.turn_end = turn_index; b.rows_start = rb; b.rows_count = rows_.size() - rb; b.copyable_start = cb; b.copyable_count = copyable_rows_.size() - cb;
              block_cache_.push_back(b); block_turn_start = turn_index + 1;
            } else if (auto *thk = std::get_if<firmius::shared::ThinkingContent>(&part)) {
              flush_block(turn_index - 1);
              size_t rb = rows_.size(); size_t cb = copyable_rows_.size();
              auto th = std::make_shared<const std::string>(thk->thinking);
              AddCopyableRow([decorateMsg, th](bool selected) { (void)selected; return decorateMsg(firmius::tui::RenderMarkdown(transcriptPreview(*th), true)); }, th);
              rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
              CachedBlock b; b.turn_start = turn_index; b.turn_end = turn_index; b.rows_start = rb; b.rows_count = rows_.size() - rb; b.copyable_start = cb; b.copyable_count = copyable_rows_.size() - cb;
              block_cache_.push_back(b); block_turn_start = turn_index + 1;
            } else if (auto *tc = std::get_if<firmius::shared::ToolCallContent>(&part)) {
              auto &view = tool_views_[tc->id];
              if (!view && tool_view_provider_) view = tool_view_provider_(tc->id);
              if (!view) { view = std::make_shared<firmius::tui::ToolCallView>(); view->phase = firmius::tui::ToolPhase::Finished; view->success = true; }
              view->toolCallId = tc->id; view->name = tc->name; view->args = tc->args;
              if (!firmius::tui::ShouldRenderToolCallView(*view)) continue;
              if (view->phase != firmius::tui::ToolPhase::Finished && view->phase != firmius::tui::ToolPhase::Error && view->phase != firmius::tui::ToolPhase::BackgroundRunning) view->phase = firmius::tui::ToolPhase::Called;
              seen_tool_call[tc->id] = true;

              if (firmius::tui::IsQuickInspectionTool(view->name)) { quick_cluster.add(view, (view->phase != firmius::tui::ToolPhase::Error)); continue; }
              flush_block(turn_index - 1);
              size_t rb = rows_.size();
              auto block = ftxui::Make<LazyToolBlock>(view, sub_history_getter_, sub_stream_getter_, process_state_getter_, subagent_state_getter_, agent_focus_handler_);
              rows_.push_back(ftxui::Make<RowComponent>(block, [block] { return block->Render() | ftxui::xflex; }));
              rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
              CachedBlock b; b.turn_start = turn_index; b.turn_end = turn_index; b.rows_start = rb; b.rows_count = rows_.size() - rb; b.copyable_start = copyable_rows_.size(); b.copyable_count = 0;
              block_cache_.push_back(b); block_turn_start = turn_index + 1;
            } else if (auto *tr = std::get_if<firmius::shared::ToolResultContent>(&part)) {
              auto &view = tool_views_[tr->toolCallId];
              if (!view && tool_view_provider_) view = tool_view_provider_(tr->toolCallId);
              if (!view) { view = std::make_shared<firmius::tui::ToolCallView>(); view->phase = firmius::tui::ToolPhase::Finished; }
              view->toolCallId = tr->toolCallId; view->result = tr->result; view->success = tr->success;
              if (!tr->success) view->phase = firmius::tui::ToolPhase::Error; else if (view->phase != firmius::tui::ToolPhase::BackgroundRunning) view->phase = firmius::tui::ToolPhase::Finished;

              if (!firmius::tui::ShouldRenderToolCallView(*view)) continue;
              if (firmius::tui::IsQuickInspectionTool(view->name)) { if (!seen_tool_call[tr->toolCallId]) quick_cluster.add(view, (view->phase != firmius::tui::ToolPhase::Error)); continue; }
              if (!seen_tool_call[tr->toolCallId]) {
                flush_block(turn_index - 1);
                size_t rb = rows_.size();
                auto block = ftxui::Make<LazyToolBlock>(view, sub_history_getter_, sub_stream_getter_, process_state_getter_, subagent_state_getter_, agent_focus_handler_);
                rows_.push_back(ftxui::Make<RowComponent>(block, [block] { return block->Render() | ftxui::xflex; }));
                rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
                seen_tool_call[tr->toolCallId] = true;
                CachedBlock b; b.turn_start = turn_index; b.turn_end = turn_index; b.rows_start = rb; b.rows_count = rows_.size() - rb; b.copyable_start = copyable_rows_.size(); b.copyable_count = 0;
                block_cache_.push_back(b); block_turn_start = turn_index + 1;
              }
            } else if (auto *err = std::get_if<firmius::shared::ErrorContent>(&part)) {
              if (!hideErrors) {
                flush_block(turn_index - 1);
                size_t rb = rows_.size(); auto error = *err;
                rows_.push_back(ftxui::Make<RowComponent>(nullptr, [error, theme] { return firmius::tui::IndentAgentRow(RenderErrorDisplay(theme, error)); }));
                rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
                CachedBlock b; b.turn_start = turn_index; b.turn_end = turn_index; b.rows_start = rb; b.rows_count = rows_.size() - rb; b.copyable_start = copyable_rows_.size(); b.copyable_count = 0;
                block_cache_.push_back(b); block_turn_start = turn_index + 1;
              }
            } else if (auto *notice = std::get_if<firmius::shared::NoticeContent>(&part)) {
              bool suppressed = false;
              if (isObservationNotice(*notice)) {
                const auto& meta = *notice->rollingMetadata;
                const auto range_key = observationRangeKey(meta);
                const int lifecycle_rank = observationLifecycleRank(meta.lifecycle);
                if (range_key && lifecycle_rank >= 0) {
                  const size_t seq = obs_pass_seq++;
                  auto it = latest_obs.find(*range_key);
                  if (it != latest_obs.end()) suppressed = it->second.lifecycle_rank > lifecycle_rank || (it->second.lifecycle_rank == lifecycle_rank && it->second.sequence > seq);
                }
              }
              if (suppressed) continue;
              flush_block(turn_index - 1);
              size_t rb = rows_.size(); auto notice_copy = *notice;
              rows_.push_back(ftxui::Make<RowComponent>(nullptr, [notice_copy, theme] { return firmius::tui::IndentAgentRow(RenderNoticeDisplay(theme, notice_copy)); }));
              rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
              CachedBlock b; b.turn_start = turn_index; b.turn_end = turn_index; b.rows_start = rb; b.rows_count = rows_.size() - rb; b.copyable_start = copyable_rows_.size(); b.copyable_count = 0;
              block_cache_.push_back(b); block_turn_start = turn_index + 1;
            } else if (std::holds_alternative<firmius::shared::ImageContent>(part)) {
              flush_block(turn_index - 1);
              size_t rb = rows_.size();
              rows_.push_back(ftxui::Make<RowComponent>(nullptr, [decorateMsg] { return decorateMsg(ftxui::text("[Image]")); }));
              rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
              CachedBlock b; b.turn_start = turn_index; b.turn_end = turn_index; b.rows_start = rb; b.rows_count = rows_.size() - rb; b.copyable_start = copyable_rows_.size(); b.copyable_count = 0;
              block_cache_.push_back(b); block_turn_start = turn_index + 1;
            }
          }
        }
        if (auto footer = buildTurnFooterSummary(t, turn_index + 1)) pending_turn_footers.push_back(*footer);
      }
      flush_block(history->turns.size() - 1, true);
    }

    if (rows_.empty()) rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
    if (row_height_cache_.size() < rows_.size()) row_height_cache_.resize(rows_.size(), kDefaultEstimatedRowHeight);

    NoteTuiChatWindowRebuildIfAvailable(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - rebuild_begin));
  }

  HistoryRenderSignature currentHistorySignature() const {
    auto *history = history_getter_ ? history_getter_() : nullptr;
    const bool showInternalNudges =
        show_internal_nudges_getter_ ? show_internal_nudges_getter_() : false;
    const bool hideErrors =
        hide_errors_getter_ ? hide_errors_getter_() : false;
    return BuildHistoryRenderSignature(history, showInternalNudges, hideErrors);
  }

  void MarkHistoryDirty(bool reset_full = false) {
    history_dirty_ = true;
    if (reset_full) {
      last_history_signature_ = {};
      block_cache_.clear();
      turn_hashes_.clear();
      rows_.clear();
      copyable_rows_.clear();
      row_height_cache_.clear();
      last_attached_start_ = 0;
      last_attached_end_ = 0;
    }
  }

  void EnsureHistoryRows() {
    RebuildIfNeeded();
  }

  std::function<const firmius::shared::AgentHistory *()> history_getter_;
  std::function<std::vector<ftxui::Element>()> live_rows_provider_;
  firmius::tui::ToolViewProvider tool_view_provider_;
  firmius::tui::ProcessStateGetter process_state_getter_;
  firmius::tui::SubagentStateGetter subagent_state_getter_;
  firmius::tui::AgentFocusHandler agent_focus_handler_;
  firmius::tui::HistoryGetter sub_history_getter_;
  firmius::tui::StreamGetter sub_stream_getter_;
  firmius::tui::LiveQuickSummaryProvider live_quick_summary_provider_;
  std::function<std::size_t()> live_measurement_signature_getter_;
  std::function<bool()> show_internal_nudges_getter_;
  std::function<bool()> hide_errors_getter_;
  std::function<bool()> show_turn_footers_getter_;
  firmius::tui::EditableModeEnabledGetter editable_mode_enabled_getter_;
  firmius::tui::EditableMessageSelectedGetter editable_message_selected_getter_;
  firmius::tui::EditableMessageClickHandler editable_message_click_handler_;
  HistoryRenderSignature last_history_signature_{};
  bool history_dirty_ = true;

  struct CachedBlock {
    size_t turn_start = 0;
    size_t turn_end = 0; // inclusive
    size_t rows_start = 0;
    size_t rows_count = 0;
    size_t copyable_start = 0;
    size_t copyable_count = 0;
  };
  std::vector<CachedBlock> block_cache_;
  std::vector<std::size_t> turn_hashes_;
  std::vector<ftxui::Component> rows_;
  std::vector<std::shared_ptr<CopyableRowComponent>> copyable_rows_;
  ftxui::Component history_inner_;
  ftxui::Component history_container_;
  std::vector<int> row_height_cache_;
  size_t last_attached_start_ = 0;
  size_t last_attached_end_ = 0;
  size_t last_visible_start_ = 0;
  size_t last_visible_end_ = 0;
  int last_rebuild_width_ = -1;
  ftxui::Component container_;
  ftxui::Component tail_spacer_;
  std::shared_ptr<firmius::tui::ScrollableBoxComponent> scrollable_;
  std::unordered_map<std::string, std::shared_ptr<firmius::tui::ToolCallView>>
      tool_views_;
  bool copy_drag_candidate_ = false;
  bool pending_bottom_restore_ = false;
  bool copy_drag_started_ = false;
  bool pending_copy_release_ = false;
  int press_x_ = -1;
  int press_y_ = -1;
  int last_drag_x_ = -1;
  int last_drag_y_ = -1;
  int pending_release_x_ = -1;

  // Cached live rows for cheap redraws where the live stream state hasn't
  // changed.
  bool has_cached_live_rows_ = false;
  std::size_t last_live_rows_signature_ = 0;
  ftxui::Elements cached_live_rows_;
  int pending_release_y_ = -1;
};

} // namespace

ftxui::Component firmius::tui::ChatWindow(
    std::function<const shared::AgentHistory *()> history_getter,
    std::function<std::vector<ftxui::Element>()> live_rows_provider,
    ToolViewProvider tool_view_provider,
    ProcessStateGetter process_state_getter,
    SubagentStateGetter subagent_state_getter,
    AgentFocusHandler agent_focus_handler,
    firmius::tui::HistoryGetter sub_history_getter,
    firmius::tui::StreamGetter sub_stream_getter,
    firmius::tui::LiveQuickSummaryProvider live_quick_summary_provider,
    std::function<std::size_t()> live_measurement_signature_getter,
    std::function<bool()> show_internal_nudges_getter,
    std::function<bool()> hide_errors_getter,
    std::function<bool()> show_turn_footers_getter,
    EditableModeEnabledGetter editable_mode_enabled_getter,
    EditableMessageSelectedGetter editable_message_selected_getter,
    EditableMessageClickHandler editable_message_click_handler) {
  return ftxui::Make<ChatWindowComponent>(
      std::move(history_getter), std::move(live_rows_provider),
      std::move(tool_view_provider), std::move(process_state_getter),
      std::move(subagent_state_getter), std::move(agent_focus_handler),
      std::move(sub_history_getter),
      std::move(sub_stream_getter), std::move(live_quick_summary_provider),
      std::move(live_measurement_signature_getter),
      std::move(show_internal_nudges_getter), std::move(hide_errors_getter),
      std::move(show_turn_footers_getter),
      std::move(editable_mode_enabled_getter),
      std::move(editable_message_selected_getter),
      std::move(editable_message_click_handler));
}
