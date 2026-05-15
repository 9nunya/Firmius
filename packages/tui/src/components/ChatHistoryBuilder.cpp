#include "components/ChatHistoryBuilder.hpp"

#include "ThemeManager.hpp"
#include "components/DiffRenderer.hpp"
#include "components/ErrorDisplay.hpp"
#include "components/GlintEffect.hpp"
#include "components/Markdown.hpp"
#include "components/SyntaxHighlighter.hpp"
#include "components/ToolBlock.hpp"
#include "utils/Icons.hpp"
#include "utils/ToolSummaries.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <variant>
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

std::string rolePrefix(firmius::shared::Role role) {
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

class LazyToolBlock : public ftxui::ComponentBase {
public:
  LazyToolBlock(std::shared_ptr<firmius::tui::ToolCallView> view,
                firmius::tui::HistoryGetter sub_history_getter,
                firmius::tui::StreamGetter sub_stream_getter,
                firmius::tui::ProcessStateGetter process_state_getter,
                firmius::tui::SubagentStateGetter subagent_state_getter,
                firmius::tui::AgentFocusHandler agent_focus_handler)
      : view_(std::move(view)),
        sub_history_getter_(std::move(sub_history_getter)),
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
      block_ = firmius::tui::ToolBlock(view_, sub_history_getter_,
                                       sub_stream_getter_,
                                       process_state_getter_,
                                       subagent_state_getter_,
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
    summary.has_error = false;
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
    rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
        nullptr, [summary = std::move(summary)] {
          const auto &theme =
              firmius::tui::ThemeManager::instance().getCurrentTheme();
          return firmius::tui::IndentAgentRow(
              RenderQuickToolRow(summary, theme));
        }));
  }

  for (const auto &tool_call_id : individual_failed_tool_call_ids) {
    auto it = cluster.views.find(tool_call_id);
    if (it == cluster.views.end()) {
      continue;
    }
    firmius::tui::QuickToolGroupSummary failure_summary;
    const auto descriptor = firmius::tui::DescribeQuickToolCall(*it->second);
    failure_summary.category = descriptor.category;
    failure_summary.targets = {descriptor.target};
    failure_summary.has_error = true;
    rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
        nullptr, [summary = std::move(failure_summary)] {
          const auto &theme =
              firmius::tui::ThemeManager::instance().getCurrentTheme();
          return firmius::tui::IndentAgentRow(
              RenderQuickToolRow(summary, theme));
        }));
  }

  return rows;
}

void AddCopyableRow(
    std::vector<ftxui::Component> &rows,
    std::vector<std::shared_ptr<firmius::tui::CopyableRowComponent>> &copyable_rows,
    std::function<ftxui::Element(bool)> render,
    std::shared_ptr<const std::string> copy_text) {
  auto row = ftxui::Make<firmius::tui::CopyableRowComponent>(std::move(render),
                                                             std::move(copy_text));
  copyable_rows.push_back(row);
  rows.push_back(row);
}

} // namespace

namespace firmius::tui {

RowComponent::RowComponent(ftxui::Component child,
                           std::function<ftxui::Element()> render)
    : child_(std::move(child)), render_(std::move(render)) {
  if (child_) {
    Add(child_);
  }
}

ftxui::Element RowComponent::OnRender() {
  ftxui::Element rendered = render_ ? render_() : ftxui::Element{};
  if (!rendered) {
    rendered = ftxui::text("");
  }
  return ftxui::selectionStyleReset(rendered);
}

bool RowComponent::Focusable() const { return false; }

bool RowComponent::OnEvent(ftxui::Event event) {
  if (child_) {
    return child_->OnEvent(event);
  }
  return false;
}

const ftxui::Box &RowComponent::box() const { return box_; }

CopyableRowComponent::CopyableRowComponent(
    std::function<ftxui::Element(bool)> render,
    std::shared_ptr<const std::string> copy_text)
    : render_(std::move(render)), copy_text_(std::move(copy_text)) {}

ftxui::Element CopyableRowComponent::OnRender() {
  auto element = render_ ? render_(false) : ftxui::text("");
  if (!element) {
    element = ftxui::text("");
  }
  return element |
         ftxui::selectionBackgroundColor(ftxui::Color::RGB(72, 96, 152)) |
         ftxui::selectionForegroundColor(ftxui::Color::RGB(245, 247, 252)) |
         ftxui::reflect(box_);
}

bool CopyableRowComponent::Focusable() const { return false; }

const ftxui::Box &CopyableRowComponent::box() const { return box_; }

const std::string &CopyableRowComponent::copyText() const {
  static const std::string empty;
  return copy_text_ ? *copy_text_ : empty;
}

bool HistoryRenderSignature::operator==(const HistoryRenderSignature &other) const {
  return history == other.history && turn_count == other.turn_count &&
         cheap_key == other.cheap_key &&
         show_internal_nudges == other.show_internal_nudges &&
         hide_errors == other.hide_errors;
}

bool HistoryRenderSignature::operator!=(const HistoryRenderSignature &other) const {
  return !(*this == other);
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

void RebuildChatHistoryIfNeeded(ChatHistoryState &state, bool &history_dirty,
                                HistoryRenderSignature &last_signature,
                                int current_width, int &last_rebuild_width,
                                const HistoryRenderSignature &signature,
                                const ChatHistoryBuildDependencies &deps) {
  auto *history = signature.history;
  const bool showInternalNudges = signature.show_internal_nudges;
  const bool hideErrors = signature.hide_errors;

  if (current_width != last_rebuild_width) {
    history_dirty = true;
    last_signature = {};
    state.block_cache.clear();
    state.turn_hashes.clear();
    state.rows.clear();
    state.copyable_rows.clear();
    last_rebuild_width = current_width;
  }

  if (!history_dirty && signature == last_signature) {
    return;
  }
  history_dirty = false;
  last_signature = signature;

  const auto rebuild_begin = std::chrono::steady_clock::now();

  std::unordered_map<std::string, ObservationNoticeRenderState> latest_obs;
  size_t obs_seq = 0;
  std::vector<std::size_t> new_hashes;
  if (history) {
    new_hashes.reserve(history->turns.size());
    for (size_t i = 0; i < history->turns.size(); ++i) {
      const auto &t = history->turns[i];
      for (const auto &msg : t.messages) {
        if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges,
                                                        t.turnId)) {
          continue;
        }
        for (const auto &part : msg.content) {
          auto *notice = std::get_if<firmius::shared::NoticeContent>(&part);
          if (notice && isObservationNotice(*notice)) {
            const auto &meta = *notice->rollingMetadata;
            const auto range_key = observationRangeKey(meta);
            const int lifecycle_rank = observationLifecycleRank(meta.lifecycle);
            if (range_key && lifecycle_rank >= 0) {
              const size_t seq = obs_seq++;
              auto &entry = latest_obs[*range_key];
              if (lifecycle_rank > entry.lifecycle_rank ||
                  (lifecycle_rank == entry.lifecycle_rank &&
                   seq >= entry.sequence)) {
                entry.lifecycle_rank = lifecycle_rank;
                entry.sequence = seq;
              }
            }
          }
        }
      }
    }

    obs_seq = 0;
    for (const auto &t : history->turns) {
      std::size_t h = HashTurn(t);
      for (const auto &msg : t.messages) {
        if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges,
                                                        t.turnId)) {
          continue;
        }
        for (const auto &part : msg.content) {
          auto *notice = std::get_if<firmius::shared::NoticeContent>(&part);
          if (notice && isObservationNotice(*notice)) {
            const auto &meta = *notice->rollingMetadata;
            const auto range_key = observationRangeKey(meta);
            const int lifecycle_rank = observationLifecycleRank(meta.lifecycle);
            bool suppressed = false;
            if (range_key && lifecycle_rank >= 0) {
              const size_t seq = obs_seq++;
              auto it = latest_obs.find(*range_key);
              if (it != latest_obs.end()) {
                suppressed = it->second.lifecycle_rank > lifecycle_rank ||
                             (it->second.lifecycle_rank == lifecycle_rank &&
                              it->second.sequence > seq);
              }
            }
            HashCombine(h, suppressed);
          }
        }
      }
      new_hashes.push_back(h);
    }
  }

  size_t first_mismatch = 0;
  while (first_mismatch < new_hashes.size() &&
         first_mismatch < state.turn_hashes.size() &&
         new_hashes[first_mismatch] == state.turn_hashes[first_mismatch]) {
    first_mismatch++;
  }

  if (first_mismatch < state.turn_hashes.size() ||
      new_hashes.size() < state.turn_hashes.size() ||
      state.block_cache.empty()) {
    size_t block_idx = 0;
    while (block_idx < state.block_cache.size() &&
           state.block_cache[block_idx].turn_end < first_mismatch) {
      block_idx++;
    }
    if (block_idx < state.block_cache.size()) {
      const auto &b = state.block_cache[block_idx];
      state.rows.erase(state.rows.begin() + b.rows_start, state.rows.end());
      state.copyable_rows.erase(state.copyable_rows.begin() + b.copyable_start,
                                state.copyable_rows.end());
      state.block_cache.erase(state.block_cache.begin() + block_idx,
                              state.block_cache.end());
      state.turn_hashes.resize(b.turn_start);
    } else if (first_mismatch == 0 || state.block_cache.empty()) {
      state.rows.clear();
      state.copyable_rows.clear();
      state.block_cache.clear();
      state.turn_hashes.clear();
    }
  }

  if (history && state.turn_hashes.size() < history->turns.size()) {
    size_t turn_index = state.turn_hashes.size();
    std::unordered_map<std::string, bool> seen_tool_call;
    for (size_t i = 0; i < turn_index; ++i) {
      for (const auto &msg : history->turns[i].messages) {
        for (const auto &part : msg.content) {
          if (auto *tc = std::get_if<firmius::shared::ToolCallContent>(&part)) {
            seen_tool_call[tc->id] = true;
          }
        }
      }
    }

    QuickToolCluster quick_cluster;
    std::vector<std::string> pending_turn_footers;
    size_t block_turn_start = turn_index;
    size_t obs_pass_seq = 0;

    for (size_t i = 0; i < turn_index; ++i) {
      const auto &t = history->turns[i];
      for (const auto &msg : t.messages) {
        if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges,
                                                        t.turnId)) {
          continue;
        }
        for (const auto &part : msg.content) {
          auto *notice = std::get_if<firmius::shared::NoticeContent>(&part);
          if (notice && isObservationNotice(*notice)) {
            const auto &meta = *notice->rollingMetadata;
            if (observationRangeKey(meta) &&
                observationLifecycleRank(meta.lifecycle) >= 0) {
              obs_pass_seq++;
            }
          }
        }
      }
    }

    auto flush_block = [&](size_t current_turn, bool final_merge = false) {
      size_t rows_before = state.rows.size();
      size_t copyable_before = state.copyable_rows.size();

      std::vector<firmius::tui::LiveQuickSummaryCluster> live_clusters;
      if (final_merge && deps.live_quick_summary_provider) {
        live_clusters = deps.live_quick_summary_provider();
      }
      const firmius::tui::LiveQuickSummaryCluster *merge_cluster = nullptr;
      size_t live_start = 0;
      if (!live_clusters.empty() && live_clusters.front().merge_with_history) {
        merge_cluster = &live_clusters.front();
        live_start = 1;
      }

      auto grouped_rows = BuildQuickToolClusterRows(quick_cluster, merge_cluster);
      for (auto &row : grouped_rows) {
        state.rows.push_back(row);
      }
      quick_cluster.clear();

      const bool show_turn_footers =
          deps.show_turn_footers_getter ? deps.show_turn_footers_getter() : true;
      std::vector<std::string> footers_to_render =
          (grouped_rows.empty() || pending_turn_footers.empty())
              ? pending_turn_footers
              : std::vector<std::string>{pending_turn_footers.back()};
      for (const auto &footer_text :
           (show_turn_footers ? footers_to_render
                              : std::vector<std::string>{})) {
        const auto &theme = firmius::tui::ThemeManager::instance().getCurrentTheme();
        state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
            nullptr, [footer_text, theme] {
              return firmius::tui::IndentAgentRow(
                  ftxui::text(footer_text) | ftxui::color(theme.chat.timestamp));
            }));
        state.rows.push_back(
            ftxui::Make<firmius::tui::RowComponent>(nullptr, [] {
              return ftxui::text("");
            }));
      }
      pending_turn_footers.clear();

      if (final_merge) {
        QuickToolCluster empty;
        for (size_t i = live_start; i < live_clusters.size(); ++i) {
          auto lrows = BuildQuickToolClusterRows(empty, &live_clusters[i]);
          for (auto &row : lrows) {
            state.rows.push_back(row);
          }
        }
      }

      if (state.rows.size() > rows_before) {
        ChatHistoryCachedBlock b;
        b.turn_start = block_turn_start;
        b.turn_end = current_turn;
        b.rows_start = rows_before;
        b.rows_count = state.rows.size() - rows_before;
        b.copyable_start = copyable_before;
        b.copyable_count = state.copyable_rows.size() - copyable_before;
        state.block_cache.push_back(b);
        block_turn_start = current_turn + 1;
      }
    };

    for (; turn_index < history->turns.size(); ++turn_index) {
      const auto &t = history->turns[turn_index];
      state.turn_hashes.push_back(new_hashes[turn_index]);
      if (t.messages.empty()) {
        continue;
      }
      bool visible = false;
      for (const auto &m : t.messages) {
        if (!firmius::tui::ShouldHideMessageInTranscript(m, showInternalNudges,
                                                         t.turnId)) {
          visible = true;
          break;
        }
      }
      if (!visible) {
        continue;
      }

      const bool isCompactionStart = (t.turnId.rfind("compaction-start-", 0) == 0);
      const bool isCompactionSummary =
          (t.turnId.rfind("compaction-summary-", 0) == 0);
      const bool isCompactionEnd = (t.turnId.rfind("compaction-end-", 0) == 0);

      if (isCompactionStart || isCompactionSummary || isCompactionEnd) {
        flush_block(turn_index - 1);
        size_t rb = state.rows.size();
        size_t cb = state.copyable_rows.size();

        auto full_width_separator = [](const std::string &label) {
          return ftxui::hbox({ftxui::filler() | ftxui::xflex,
                              ftxui::text(" " + label + " ") | ftxui::dim,
                              ftxui::filler() | ftxui::xflex}) |
                 ftxui::xflex;
        };

        if (isCompactionStart ||
            (isCompactionSummary &&
             (turn_index == 0 ||
              history->turns[turn_index - 1].turnId.rfind("compaction-start-", 0) !=
                  0))) {
          state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
              nullptr, [full_width_separator] {
                return full_width_separator("Compaction");
              }));
        }

        for (const auto &msg : t.messages) {
          if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges,
                                                          t.turnId)) {
            continue;
          }
          for (const auto &part : msg.content) {
            if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
              auto text = std::make_shared<const std::string>(txt->text);
              AddCopyableRow(state.rows, state.copyable_rows,
                             [text](bool selected) {
                               (void)selected;
                               return firmius::tui::IndentAgentRow(
                                   firmius::tui::RenderMarkdown(
                                       transcriptPreview(*text)));
                             },
                             text);
            } else if (auto *thk =
                           std::get_if<firmius::shared::ThinkingContent>(&part)) {
              auto th = std::make_shared<const std::string>(thk->thinking);
              AddCopyableRow(state.rows, state.copyable_rows,
                             [th](bool selected) {
                               (void)selected;
                               return firmius::tui::IndentAgentRow(
                                   firmius::tui::RenderMarkdown(
                                       transcriptPreview(*th), true));
                             },
                             th);
            }
          }
        }
        if (isCompactionEnd ||
            (isCompactionSummary &&
             (turn_index + 1 >= history->turns.size() ||
              history->turns[turn_index + 1].turnId.rfind("compaction-end-", 0) !=
                  0))) {
          state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
              nullptr, [full_width_separator] {
                return full_width_separator("Compaction Complete");
              }));
        }

        ChatHistoryCachedBlock b;
        b.turn_start = turn_index;
        b.turn_end = turn_index;
        b.rows_start = rb;
        b.rows_count = state.rows.size() - rb;
        b.copyable_start = cb;
        b.copyable_count = state.copyable_rows.size() - cb;
        state.block_cache.push_back(b);
        block_turn_start = turn_index + 1;
        continue;
      }

      for (const auto &msg : t.messages) {
        if (firmius::tui::ShouldHideMessageInTranscript(msg, showInternalNudges,
                                                        t.turnId)) {
          continue;
        }
        const auto &theme = firmius::tui::ThemeManager::instance().getCurrentTheme();
        const std::string prefix = rolePrefix(msg.role);
        bool isUser = (msg.role == firmius::shared::Role::User);
        ftxui::Color prefixColor =
            isUser ? theme.chat.user_prefix : theme.chat.agent_prefix;

        auto makeTag = [&theme](const std::string &label) {
          return ftxui::text(" " + label + " ") | ftxui::bold |
                 ftxui::color(theme.base.bg) |
                 ftxui::bgcolor(theme.base.highlight);
        };
        auto renderUserMessage =
            [prefixColor, prefix, theme, makeTag](const std::string &text,
                                                  int image_count,
                                                  bool selected) {
              ftxui::Elements body;
              body.push_back(ftxui::hbox({
                                   ftxui::text(prefix) | ftxui::bold |
                                       ftxui::color(prefixColor),
                                   firmius::tui::RenderMarkdown(
                                       transcriptPreview(text)) |
                                       ftxui::xflex,
                               }) |
                             ftxui::xflex);
              if (image_count > 0) {
                ftxui::Elements tags;
                for (int i = 0; i < image_count; ++i) {
                  tags.push_back(makeTag("IMAGE " + std::to_string(i + 1)));
                }
                body.push_back(ftxui::hbox(std::move(tags)) | ftxui::xflex);
              }
              return ftxui::vbox({ftxui::text(""),
                                  ftxui::vbox(std::move(body)) | ftxui::xflex,
                                  ftxui::text("")}) |
                     ftxui::bgcolor(selected ? ftxui::Color::RGB(72, 96, 152)
                                             : theme.input.bg) |
                     ftxui::xflex;
            };
        auto decorateMsg = [prefixColor, prefix, isUser,
                            theme](const ftxui::Element &content) {
          auto e = ftxui::hbox({ftxui::text(prefix) | ftxui::bold |
                                    ftxui::color(prefixColor),
                                content | ftxui::xflex});
          return isUser ? e | ftxui::xflex : firmius::tui::IndentAgentRow(e);
        };

        if (isUser) {
          flush_block(turn_index - 1);
          size_t rb = state.rows.size();
          size_t cb = state.copyable_rows.size();
          std::string user_text;
          int image_count = 0;
          for (const auto &part : msg.content) {
            if (const auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
              if (!user_text.empty()) {
                user_text += "\n";
              }
              user_text += txt->text;
            } else if (std::holds_alternative<firmius::shared::ImageContent>(part)) {
              ++image_count;
            }
          }
          uint64_t msg_timestamp = msg.timestamp;
          auto user_text_ptr =
              std::make_shared<const std::string>(std::move(user_text));
          auto editable_selected_getter = deps.editable_message_selected_getter;
          AddCopyableRow(state.rows, state.copyable_rows,
                         [renderUserMessage, user_text_ptr, image_count,
                          msg_timestamp,
                          editable_selected_getter](bool selected) {
                           bool is_edit = editable_selected_getter
                                              ? editable_selected_getter(
                                                    msg_timestamp)
                                              : false;
                           return renderUserMessage(*user_text_ptr, image_count,
                                                    selected || is_edit);
                         },
                         user_text_ptr);
          state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
              nullptr, [] { return ftxui::text(""); }));
          ChatHistoryCachedBlock b;
          b.turn_start = turn_index;
          b.turn_end = turn_index;
          b.rows_start = rb;
          b.rows_count = state.rows.size() - rb;
          b.copyable_start = cb;
          b.copyable_count = state.copyable_rows.size() - cb;
          state.block_cache.push_back(b);
          block_turn_start = turn_index + 1;
          continue;
        }

        for (const auto &part : msg.content) {
          if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
            flush_block(turn_index - 1);
            size_t rb = state.rows.size();
            size_t cb = state.copyable_rows.size();
            auto text = std::make_shared<const std::string>(txt->text);
            AddCopyableRow(state.rows, state.copyable_rows,
                           [decorateMsg, text](bool selected) {
                             (void)selected;
                             return decorateMsg(firmius::tui::RenderMarkdown(
                                 transcriptPreview(*text)));
                           },
                           text);
            state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                nullptr, [] { return ftxui::text(""); }));
            ChatHistoryCachedBlock b;
            b.turn_start = turn_index;
            b.turn_end = turn_index;
            b.rows_start = rb;
            b.rows_count = state.rows.size() - rb;
            b.copyable_start = cb;
            b.copyable_count = state.copyable_rows.size() - cb;
            state.block_cache.push_back(b);
            block_turn_start = turn_index + 1;
          } else if (auto *thk =
                         std::get_if<firmius::shared::ThinkingContent>(&part)) {
            flush_block(turn_index - 1);
            size_t rb = state.rows.size();
            size_t cb = state.copyable_rows.size();
            auto th = std::make_shared<const std::string>(thk->thinking);
            AddCopyableRow(state.rows, state.copyable_rows,
                           [decorateMsg, th](bool selected) {
                             (void)selected;
                             return decorateMsg(firmius::tui::RenderMarkdown(
                                 transcriptPreview(*th), true));
                           },
                           th);
            state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                nullptr, [] { return ftxui::text(""); }));
            ChatHistoryCachedBlock b;
            b.turn_start = turn_index;
            b.turn_end = turn_index;
            b.rows_start = rb;
            b.rows_count = state.rows.size() - rb;
            b.copyable_start = cb;
            b.copyable_count = state.copyable_rows.size() - cb;
            state.block_cache.push_back(b);
            block_turn_start = turn_index + 1;
          } else if (auto *tc =
                         std::get_if<firmius::shared::ToolCallContent>(&part)) {
            auto &view = state.tool_views[tc->id];
            if (!view && deps.tool_view_provider) {
              view = deps.tool_view_provider(tc->id);
            }
            if (!view) {
              view = std::make_shared<firmius::tui::ToolCallView>();
              view->phase = firmius::tui::ToolPhase::Finished;
              view->success = true;
            }
            view->toolCallId = tc->id;
            view->name = tc->name;
            view->args = tc->args;
            if (!firmius::tui::ShouldRenderToolCallView(*view)) {
              continue;
            }
            if (view->phase != firmius::tui::ToolPhase::Finished &&
                view->phase != firmius::tui::ToolPhase::Error &&
                view->phase != firmius::tui::ToolPhase::BackgroundRunning) {
              view->phase = firmius::tui::ToolPhase::Called;
            }
            seen_tool_call[tc->id] = true;

            if (firmius::tui::IsQuickInspectionTool(view->name)) {
              quick_cluster.add(view,
                                (view->phase != firmius::tui::ToolPhase::Error));
              continue;
            }
            flush_block(turn_index - 1);
            size_t rb = state.rows.size();
            auto block = ftxui::Make<LazyToolBlock>(
                view, deps.sub_history_getter, deps.sub_stream_getter,
                deps.process_state_getter, deps.subagent_state_getter,
                deps.agent_focus_handler);
            state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                block, [block] { return block->Render() | ftxui::xflex; }));
            state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                nullptr, [] { return ftxui::text(""); }));
            ChatHistoryCachedBlock b;
            b.turn_start = turn_index;
            b.turn_end = turn_index;
            b.rows_start = rb;
            b.rows_count = state.rows.size() - rb;
            b.copyable_start = state.copyable_rows.size();
            b.copyable_count = 0;
            state.block_cache.push_back(b);
            block_turn_start = turn_index + 1;
          } else if (auto *tr =
                         std::get_if<firmius::shared::ToolResultContent>(&part)) {
            auto &view = state.tool_views[tr->toolCallId];
            if (!view && deps.tool_view_provider) {
              view = deps.tool_view_provider(tr->toolCallId);
            }
            if (!view) {
              view = std::make_shared<firmius::tui::ToolCallView>();
              view->phase = firmius::tui::ToolPhase::Finished;
            }
            view->toolCallId = tr->toolCallId;
            view->result = tr->result;
            view->success = tr->success;
            if (!tr->success) {
              view->phase = firmius::tui::ToolPhase::Error;
            } else if (view->phase !=
                       firmius::tui::ToolPhase::BackgroundRunning) {
              view->phase = firmius::tui::ToolPhase::Finished;
            }

            if (!firmius::tui::ShouldRenderToolCallView(*view)) {
              continue;
            }
            if (firmius::tui::IsQuickInspectionTool(view->name)) {
              if (!seen_tool_call[tr->toolCallId]) {
                quick_cluster.add(
                    view, (view->phase != firmius::tui::ToolPhase::Error));
              }
              continue;
            }
            if (!seen_tool_call[tr->toolCallId]) {
              flush_block(turn_index - 1);
              size_t rb = state.rows.size();
              auto block = ftxui::Make<LazyToolBlock>(
                  view, deps.sub_history_getter, deps.sub_stream_getter,
                  deps.process_state_getter, deps.subagent_state_getter,
                  deps.agent_focus_handler);
              state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                  block, [block] { return block->Render() | ftxui::xflex; }));
              state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                  nullptr, [] { return ftxui::text(""); }));
              seen_tool_call[tr->toolCallId] = true;
              ChatHistoryCachedBlock b;
              b.turn_start = turn_index;
              b.turn_end = turn_index;
              b.rows_start = rb;
              b.rows_count = state.rows.size() - rb;
              b.copyable_start = state.copyable_rows.size();
              b.copyable_count = 0;
              state.block_cache.push_back(b);
              block_turn_start = turn_index + 1;
            }
          } else if (auto *err =
                         std::get_if<firmius::shared::ErrorContent>(&part)) {
            if (!hideErrors) {
              flush_block(turn_index - 1);
              size_t rb = state.rows.size();
              auto error = *err;
              state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                  nullptr, [error, theme] {
                    return firmius::tui::IndentAgentRow(
                        RenderErrorDisplay(theme, error));
                  }));
              state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                  nullptr, [] { return ftxui::text(""); }));
              ChatHistoryCachedBlock b;
              b.turn_start = turn_index;
              b.turn_end = turn_index;
              b.rows_start = rb;
              b.rows_count = state.rows.size() - rb;
              b.copyable_start = state.copyable_rows.size();
              b.copyable_count = 0;
              state.block_cache.push_back(b);
              block_turn_start = turn_index + 1;
            }
          } else if (auto *notice =
                         std::get_if<firmius::shared::NoticeContent>(&part)) {
            bool suppressed = false;
            if (isObservationNotice(*notice)) {
              const auto &meta = *notice->rollingMetadata;
              const auto range_key = observationRangeKey(meta);
              const int lifecycle_rank = observationLifecycleRank(meta.lifecycle);
              if (range_key && lifecycle_rank >= 0) {
                const size_t seq = obs_pass_seq++;
                auto it = latest_obs.find(*range_key);
                if (it != latest_obs.end()) {
                  suppressed = it->second.lifecycle_rank > lifecycle_rank ||
                               (it->second.lifecycle_rank == lifecycle_rank &&
                                it->second.sequence > seq);
                }
              }
            }
            if (suppressed) {
              continue;
            }
            flush_block(turn_index - 1);
            size_t rb = state.rows.size();
            auto notice_copy = *notice;
            state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                nullptr, [notice_copy, theme] {
                  return firmius::tui::IndentAgentRow(
                      RenderNoticeDisplay(theme, notice_copy));
                }));
            state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                nullptr, [] { return ftxui::text(""); }));
            ChatHistoryCachedBlock b;
            b.turn_start = turn_index;
            b.turn_end = turn_index;
            b.rows_start = rb;
            b.rows_count = state.rows.size() - rb;
            b.copyable_start = state.copyable_rows.size();
            b.copyable_count = 0;
            state.block_cache.push_back(b);
            block_turn_start = turn_index + 1;
          } else if (std::holds_alternative<firmius::shared::ImageContent>(part)) {
            flush_block(turn_index - 1);
            size_t rb = state.rows.size();
            state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                nullptr, [decorateMsg] { return decorateMsg(ftxui::text("[Image]")); }));
            state.rows.push_back(ftxui::Make<firmius::tui::RowComponent>(
                nullptr, [] { return ftxui::text(""); }));
            ChatHistoryCachedBlock b;
            b.turn_start = turn_index;
            b.turn_end = turn_index;
            b.rows_start = rb;
            b.rows_count = state.rows.size() - rb;
            b.copyable_start = state.copyable_rows.size();
            b.copyable_count = 0;
            state.block_cache.push_back(b);
            block_turn_start = turn_index + 1;
          }
        }
      }
      if (auto footer = buildTurnFooterSummary(t, turn_index + 1)) {
        pending_turn_footers.push_back(*footer);
      }
    }
    flush_block(history->turns.size() - 1, true);
  }

  if (state.rows.empty()) {
    state.rows.push_back(
        ftxui::Make<firmius::tui::RowComponent>(nullptr, [] { return ftxui::text(""); }));
  }

  NoteTuiChatWindowRebuildIfAvailable(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - rebuild_begin));
}

} // namespace firmius::tui
