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
#include "NotificationManager.hpp"
#include "utils/Clipboard.hpp"
#include "utils/Icons.hpp"
#include "utils/ToolSummaries.hpp"
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <ftxui/component/component.hpp>
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

template <typename T>
void HashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

std::size_t ComputeHistoryRevision(const firmius::shared::AgentHistory *history) {
  if (!history) {
    return 0;
  }

  std::size_t seed = history->turns.size();
  for (const auto &turn : history->turns) {
    HashCombine(seed, turn.turnId);
    HashCombine(seed, turn.messages.size());
    for (const auto &message : turn.messages) {
      HashCombine(seed, static_cast<int>(message.role));
      HashCombine(seed, message.content.size());
      for (const auto &part : message.content) {
        std::visit(
            [&](const auto &content) {
              using T = std::decay_t<decltype(content)>;
              HashCombine(seed, typeid(T).hash_code());
              if constexpr (std::is_same_v<T, firmius::shared::TextContent>) {
                HashCombine(seed, content.text.size());
              } else if constexpr (std::is_same_v<T,
                                                  firmius::shared::ThinkingContent>) {
                HashCombine(seed, content.thinking.size());
                HashCombine(seed, content.signature.size());
              } else if constexpr (std::is_same_v<T,
                                                  firmius::shared::ToolCallContent>) {
                HashCombine(seed, content.id);
                HashCombine(seed, content.name);
                HashCombine(seed, content.args.size());
              } else if constexpr (std::is_same_v<T,
                                                  firmius::shared::ToolResultContent>) {
                HashCombine(seed, content.toolCallId);
                HashCombine(seed, content.result.size());
                HashCombine(seed, content.success);
                HashCombine(seed, content.processId);
                HashCombine(seed, content.subagentId);
              } else if constexpr (std::is_same_v<T,
                                                  firmius::shared::ImageContent>) {
                HashCombine(seed, content.url.size());
                HashCombine(seed, content.mediaType.size());
                HashCombine(seed, content.detail.size());
              } else if constexpr (std::is_same_v<T,
                                                  firmius::shared::ErrorContent>) {
                HashCombine(seed, content.errorName.size());
                HashCombine(seed, content.description.size());
                HashCombine(seed, content.details.size());
              } else if constexpr (std::is_same_v<T,
                                                  firmius::shared::NoticeContent>) {
                HashCombine(seed, content.title.size());
                HashCombine(seed, content.message.size());
                HashCombine(seed, content.details.size());
                HashCombine(seed, static_cast<int>(content.severity));
                if (content.rollingMetadata) {
                  const auto &meta = *content.rollingMetadata;
                  HashCombine(seed, meta.eventKind);
                  HashCombine(seed, meta.lifecycle);
                  if (meta.modelLabel)
                    HashCombine(seed, *meta.modelLabel);
                  if (meta.sourceStartTurnId)
                    HashCombine(seed, *meta.sourceStartTurnId);
                  if (meta.sourceEndTurnId)
                    HashCombine(seed, *meta.sourceEndTurnId);
                  if (meta.sourceTurnCount)
                    HashCombine(seed, *meta.sourceTurnCount);
                  if (meta.sourceChunkCount)
                    HashCombine(seed, *meta.sourceChunkCount);
                  if (meta.sourceTokens)
                    HashCombine(seed, *meta.sourceTokens);
                  if (meta.summaryTokens)
                    HashCombine(seed, *meta.summaryTokens);
                  if (meta.savedTokens)
                    HashCombine(seed, *meta.savedTokens);
                }
              }

            },
            part);
      }
    }
  }

  return seed;
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
    const firmius::shared::AgentTurn &turn) {
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
    return ftxui::selectionStyleReset(render_());
  }
  bool Focusable() const override { return false; }
  bool OnEvent(ftxui::Event event) override {
    if (child_)
      return child_->OnEvent(event);
    return false;
  }

private:
  ftxui::Component child_;
  std::function<ftxui::Element()> render_;
};

class CopyableRowComponent : public ftxui::ComponentBase {
public:
  CopyableRowComponent(std::function<ftxui::Element(bool)> render,
                       std::string copy_text)
      : render_(std::move(render)), copy_text_(std::move(copy_text)) {}

  ftxui::Element OnRender() override {
    auto element = render_ ? render_(false) : ftxui::text("");
    return element | ftxui::selectionBackgroundColor(ftxui::Color::RGB(72, 96, 152)) |
           ftxui::selectionForegroundColor(ftxui::Color::RGB(245, 247, 252)) |
           ftxui::reflect(box_);
  }

  bool Focusable() const override { return false; }

  const ftxui::Box &box() const { return box_; }
  const std::string &copyText() const { return copy_text_; }

private:
  std::function<ftxui::Element(bool)> render_;
  std::string copy_text_;
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
    cfg.gradientColors = {pill_bg, theme.base.highlight, pill_bg};
    cfg.glintSize = 8;
    cfg.intervalSeconds = summary.has_live ? 0.9f : 1.4f;
    cfg.durationSeconds = summary.has_live ? 0.8f : 1.0f;
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
      std::function<bool()> show_internal_nudges_getter,
      std::function<bool()> hide_errors_getter)
      : history_getter_(std::move(history_getter)),
        live_rows_provider_(std::move(live_rows_provider)),
        tool_view_provider_(std::move(tool_view_provider)),
        process_state_getter_(std::move(process_state_getter)),
        subagent_state_getter_(std::move(subagent_state_getter)),
        agent_focus_handler_(std::move(agent_focus_handler)),
        sub_history_getter_(std::move(sub_history_getter)),
        sub_stream_getter_(std::move(sub_stream_getter)),
        live_quick_summary_provider_(std::move(live_quick_summary_provider)),
        show_internal_nudges_getter_(
            std::move(show_internal_nudges_getter)),
        hide_errors_getter_(std::move(hide_errors_getter)) {

    history_inner_ = ftxui::Container::Vertical({});
    history_container_ = ftxui::Renderer(history_inner_, [this] {
      ftxui::Elements elements;
      for (size_t i = 0; i < history_inner_->ChildCount(); ++i) {
        elements.push_back(history_inner_->ChildAt(i)->Render());
      }
      return ftxui::vbox(std::move(elements));
    });

    auto live_rows_cmp = ftxui::Make<RowComponent>(nullptr, [this] {
      if (!live_rows_provider_)
        return ftxui::text("");
      auto rows = live_rows_provider_();
      if (rows.empty())
        return ftxui::text("");

      return ftxui::vbox(std::move(rows));
    });

    tail_spacer_ =
        ftxui::Make<RowComponent>(nullptr, [] {
          ftxui::Elements padding_rows;
          padding_rows.reserve(kChatTailPaddingLines);
          for (int i = 0; i < kChatTailPaddingLines; ++i) {
            padding_rows.push_back(ftxui::text(" "));
          }
          return ftxui::vbox(std::move(padding_rows)) | ftxui::xflex;
        });

    container_ = ftxui::Container::Vertical(
        {history_container_, live_rows_cmp, tail_spacer_});
    container_ = ftxui::Renderer(container_, [this, live_rows_cmp] {
      return ftxui::vbox({
          history_container_->Render(),
          live_rows_cmp->Render(),
          tail_spacer_->Render(),
      });
    });

    scrollable_ = firmius::tui::ScrollableBox(
        container_, {.startAtBottom = true, .overlayScrollbar = true});
    Add(scrollable_);
  }

  ftxui::Element OnRender() override {
    RebuildIfNeeded();
    return scrollable_ ? scrollable_->Render() : ftxui::text("");
  }

  bool Focusable() const override { return false; }

  bool OnEvent(ftxui::Event event) override {
    if (event == ftxui::Event::Special("ThreadChanged") ||
        event == ftxui::Event::Special("ThemeChanged")) {
      last_history_revision_ = std::numeric_limits<std::size_t>::max();
      RebuildIfNeeded();
      if (scrollable_) {
        scrollable_->RequestScrollToBottom();
      }
      return true;
    }

    const bool copy_handler_consumed = HandleCopySelection(event);

    RebuildIfNeeded();
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

    RebuildIfNeeded();
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
                      std::string copy_text) {
    auto row = ftxui::Make<CopyableRowComponent>(std::move(render),
                                                 std::move(copy_text));
    copyable_rows_.push_back(row);
    rows_.push_back(row);
  }

  void RebuildIfNeeded() {
    auto *history = history_getter_ ? history_getter_() : nullptr;
    const std::size_t history_revision = ComputeHistoryRevision(history);
    if (history_revision == last_history_revision_)
      return;
    last_history_revision_ = history_revision;

    rows_.clear();
    copyable_rows_.clear();
    history_inner_->DetachAllChildren();

    if (history) {
      const bool showInternalNudges =
          show_internal_nudges_getter_ ? show_internal_nudges_getter_() : false;
      const bool hideErrors =
          hide_errors_getter_ ? hide_errors_getter_() : false;
      std::unordered_map<std::string, bool> seen_tool_call;
      QuickToolCluster quick_cluster;
      auto flush_quick_cluster = [&](bool merge_live = false) {
        std::vector<firmius::tui::LiveQuickSummaryCluster> live_clusters;
        if (merge_live && live_quick_summary_provider_) {
          live_clusters = live_quick_summary_provider_();
        }

        const firmius::tui::LiveQuickSummaryCluster *merge_cluster = nullptr;
        size_t start_index = 0;
        if (!live_clusters.empty() && live_clusters.front().merge_with_history) {
          merge_cluster = &live_clusters.front();
          start_index = 1;
        }

        auto grouped_rows =
            BuildQuickToolClusterRows(quick_cluster, merge_cluster);
        for (auto &row : grouped_rows) {
          rows_.push_back(row);
        }
        quick_cluster.clear();

        QuickToolCluster empty_cluster;
        for (size_t i = start_index; i < live_clusters.size(); ++i) {
          auto live_rows =
              BuildQuickToolClusterRows(empty_cluster, &live_clusters[i]);
          for (auto &row : live_rows) {
            rows_.push_back(row);
          }
        }
      };

      std::unordered_map<std::string, ObservationNoticeRenderState>
          latest_observation_notice_by_range;
      size_t observation_notice_sequence = 0;
      for (size_t turn_index = 0; turn_index < history->turns.size();
           ++turn_index) {
        const auto &t = history->turns[turn_index];
        for (const auto &msg : t.messages) {
          if (firmius::tui::ShouldHideMessageInTranscript(
                  msg, showInternalNudges, t.turnId)) {
            continue;
          }
          for (const auto &part : msg.content) {
            auto *notice = std::get_if<firmius::shared::NoticeContent>(&part);
            if (!notice || !isObservationNotice(*notice)) {
              continue;
            }
            const auto &meta = *notice->rollingMetadata;
            const auto range_key = observationRangeKey(meta);
            if (!range_key.has_value()) {
              continue;
            }
            const int lifecycle_rank = observationLifecycleRank(meta.lifecycle);
            if (lifecycle_rank < 0) {
              continue;
            }
            const size_t sequence = observation_notice_sequence++;
            auto &entry = latest_observation_notice_by_range[*range_key];
            if (lifecycle_rank > entry.lifecycle_rank ||
                (lifecycle_rank == entry.lifecycle_rank &&
                 sequence >= entry.sequence)) {
              entry.lifecycle_rank = lifecycle_rank;
              entry.sequence = sequence;
            }
          }
        }
      }

      observation_notice_sequence = 0;
      for (size_t turn_index = 0; turn_index < history->turns.size();
           ++turn_index) {
        const auto &t = history->turns[turn_index];
        if (t.messages.empty())
          continue;

        bool has_visible_message = false;
        for (const auto &msg : t.messages) {
          if (!firmius::tui::ShouldHideMessageInTranscript(
                  msg, showInternalNudges, t.turnId)) {
            has_visible_message = true;
            break;
          }
        }
        if (!has_visible_message) {
          continue;
        }

        const bool isCompactionStart =
            (t.turnId.rfind("compaction-start-", 0) == 0);
        const bool isCompactionSummary =
            (t.turnId.rfind("compaction-summary-", 0) == 0);
        const bool isCompactionEnd =
            (t.turnId.rfind("compaction-end-", 0) == 0);
        if (isCompactionStart || isCompactionSummary || isCompactionEnd) {
          const bool summaryHasExplicitStart =
              isCompactionSummary && turn_index > 0 &&
              history->turns[turn_index - 1].turnId.rfind("compaction-start-", 0) == 0;
          const bool summaryHasExplicitEnd =
              isCompactionSummary && (turn_index + 1) < history->turns.size() &&
              history->turns[turn_index + 1].turnId.rfind("compaction-end-", 0) == 0;
          flush_quick_cluster();
          auto full_width_separator = [](const std::string &label) {
            return ftxui::hbox({
                       ftxui::filler() | ftxui::xflex,
                       ftxui::text(" " + label + " ") | ftxui::dim,
                       ftxui::filler() | ftxui::xflex,
                   }) |
                   ftxui::xflex;
          };
          auto compaction_separator = [full_width_separator]() {
            return full_width_separator("Compaction");
          };
          auto compaction_separator_bottom = [full_width_separator]() {
            return full_width_separator("Compaction Complete");
          };

          if (isCompactionStart ||
              (isCompactionSummary && !summaryHasExplicitStart)) {
            rows_.push_back(
                ftxui::Make<RowComponent>(nullptr, [compaction_separator]() {
                  return compaction_separator();
                }));
          }

          for (const auto &msg : t.messages) {
            if (firmius::tui::ShouldHideMessageInTranscript(
                    msg, showInternalNudges, t.turnId)) {
              continue;
            }
            for (const auto &part : msg.content) {
              if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
                std::string text = txt->text;
                std::string copy_text = text;
                AddCopyableRow([text = std::move(text)](bool selected) {
                      (void)selected;
                      return firmius::tui::IndentAgentRow(
                          firmius::tui::RenderMarkdown(
                              transcriptPreview(text)));
                    }, std::move(copy_text));
              } else if (auto *thk = std::get_if<firmius::shared::ThinkingContent>(&part)) {
                std::string thinking = thk->thinking;
                std::string copy_text = thinking;
                AddCopyableRow([thinking = std::move(thinking)](bool selected) {
                      (void)selected;
                      return firmius::tui::IndentAgentRow(
                          firmius::tui::RenderMarkdown(
                              transcriptPreview(thinking), true));
                    }, std::move(copy_text));
              }
            }
          }

          if (isCompactionEnd || (isCompactionSummary && !summaryHasExplicitEnd)) {
            rows_.push_back(ftxui::Make<RowComponent>(
                nullptr, [compaction_separator_bottom]() {
                  return compaction_separator_bottom();
                }));
          }
          continue;
        }

        for (const auto &msg : t.messages) {
          if (firmius::tui::ShouldHideMessageInTranscript(
                  msg, showInternalNudges, t.turnId)) {
            continue;
          }
          const auto &theme =
              firmius::tui::ThemeManager::instance().getCurrentTheme();
          const std::string prefix = rolePrefix(msg.role);
          bool isUser = (msg.role == firmius::shared::Role::User);
          ftxui::Color prefixColor =
              isUser ? theme.chat.user_prefix : theme.chat.agent_prefix;
          auto makeTag = [&theme](const std::string &label) {
            return ftxui::text(" " + label + " ") | ftxui::bold |
                   ftxui::color(theme.base.bg) |
                   ftxui::bgcolor(theme.base.highlight);
          };
          auto renderUserMessage = [prefixColor, prefix, theme, makeTag](
                                       const std::string &text,
                                       int image_count,
                                       bool selected) {
            ftxui::Elements body;
            body.push_back(
                ftxui::hbox({
                    ftxui::text(prefix) | ftxui::bold |
                        ftxui::color(prefixColor),
                    firmius::tui::RenderMarkdown(
                        transcriptPreview(text)) | ftxui::xflex,
                }) |
                ftxui::xflex);
            if (image_count > 0) {
              ftxui::Elements tags;
              for (int i = 0; i < image_count; ++i) {
                if (i > 0) {
                  tags.push_back(ftxui::text(" "));
                }
                tags.push_back(makeTag("IMAGE " + std::to_string(i + 1)));
              }
              body.push_back(ftxui::hbox(std::move(tags)) | ftxui::xflex);
            }
            const auto bubble_bg =
                selected ? ftxui::Color::RGB(72, 96, 152) : theme.input.bg;
            return ftxui::vbox({
                       ftxui::text(""),
                       ftxui::vbox(std::move(body)) | ftxui::xflex,
                       ftxui::text(""),
                   }) |
                   ftxui::bgcolor(bubble_bg) | ftxui::xflex;
          };

          auto decorateMsg = [prefixColor,
                              prefix, isUser, theme](const ftxui::Element &content) {
            auto e = ftxui::hbox({
                ftxui::text(prefix) | ftxui::bold | ftxui::color(prefixColor),
                content | ftxui::xflex,
            });
            if (isUser) {
              return e | ftxui::xflex;
            }
            return firmius::tui::IndentAgentRow(e);
          };

          if (isUser) {
            flush_quick_cluster();
            std::string user_text;
            int image_count = 0;
            for (const auto &part : msg.content) {
              if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
                if (!user_text.empty()) {
                  user_text += "\n";
                }
                user_text += txt->text;
              } else if (std::holds_alternative<firmius::shared::ImageContent>(part)) {
                ++image_count;
              }
            }
            std::string copy_text = user_text;
            copyable_rows_.push_back(ftxui::Make<CopyableRowComponent>(
                [renderUserMessage, user_text, image_count](bool selected) {
                  return renderUserMessage(user_text, image_count, selected);
                },
                std::move(copy_text)));
            rows_.push_back(copyable_rows_.back());
            rows_.push_back(ftxui::Make<RowComponent>(
                nullptr, [] { return ftxui::text(""); }));
            continue;
          }

          for (const auto &part : msg.content) {
            if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
              flush_quick_cluster();
              std::string text = txt->text;
              std::string copy_text = text;
              AddCopyableRow([decorateMsg, text = std::move(text)](bool selected) {
                    (void)selected;
                    return decorateMsg(firmius::tui::RenderMarkdown(
                        transcriptPreview(text)));
                  }, std::move(copy_text));
              rows_.push_back(ftxui::Make<RowComponent>(
                  nullptr, [] { return ftxui::text(""); }));
            } else if (auto *thk =
                           std::get_if<firmius::shared::ThinkingContent>(
                               &part)) {
              flush_quick_cluster();
              std::string thinking = thk->thinking;
              std::string copy_text = thinking;
              AddCopyableRow([decorateMsg, thinking = std::move(thinking)](bool selected) {
                    (void)selected;
                    return decorateMsg(
                        firmius::tui::RenderMarkdown(
                            transcriptPreview(thinking), true));
                  }, std::move(copy_text));
              rows_.push_back(ftxui::Make<RowComponent>(
                  nullptr, [] { return ftxui::text(""); }));
            } else if (auto *tc = std::get_if<firmius::shared::ToolCallContent>(
                           &part)) {
              auto &view = tool_views_[tc->id];
              if (!view && tool_view_provider_)
                view = tool_view_provider_(tc->id);
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
                  view->phase != firmius::tui::ToolPhase::BackgroundRunning)
                view->phase = firmius::tui::ToolPhase::Called;
              seen_tool_call[tc->id] = true;

              if (firmius::tui::IsQuickInspectionTool(view->name)) {
                bool is_success = (view->phase != firmius::tui::ToolPhase::Error);
                quick_cluster.add(view, is_success);
                continue;
              }

              flush_quick_cluster();
              auto block = firmius::tui::ToolBlock(
                  view, sub_history_getter_, sub_stream_getter_,
                  process_state_getter_,
                  subagent_state_getter_, agent_focus_handler_);
              auto row = ftxui::Make<RowComponent>(block, [block] {
                return block->Render() | ftxui::xflex;
              });
              rows_.push_back(row);
              rows_.push_back(ftxui::Make<RowComponent>(
                  nullptr, [] { return ftxui::text(""); }));
            } else if (auto *tr =
                           std::get_if<firmius::shared::ToolResultContent>(
                               &part)) {
              auto &view = tool_views_[tr->toolCallId];
              if (!view && tool_view_provider_)
                view = tool_view_provider_(tr->toolCallId);
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
                  bool is_success = (view->phase != firmius::tui::ToolPhase::Error);
                  quick_cluster.add(view, is_success);
                }
                continue;
              }

              if (!seen_tool_call[tr->toolCallId]) {
                flush_quick_cluster();
                auto block = firmius::tui::ToolBlock(
                    view, sub_history_getter_, sub_stream_getter_,
                    process_state_getter_,
                    subagent_state_getter_, agent_focus_handler_);
                auto row =
                    ftxui::Make<RowComponent>(block, [block] {
                      return block->Render() | ftxui::xflex;
                    });
                rows_.push_back(row);
                rows_.push_back(ftxui::Make<RowComponent>(
                    nullptr, [] { return ftxui::text(""); }));
                seen_tool_call[tr->toolCallId] = true;
              }
            } else if (auto *err =
                           std::get_if<firmius::shared::ErrorContent>(&part)) {
              if (!hideErrors) {
                flush_quick_cluster();
                auto error = *err;
                auto row = ftxui::Make<RowComponent>(
                    nullptr, [error, theme] {
                      return firmius::tui::IndentAgentRow(
                          RenderErrorDisplay(theme, error));
                    });
                rows_.push_back(row);
                rows_.push_back(ftxui::Make<RowComponent>(
                    nullptr, [] { return ftxui::text(""); }));
              }
            } else if (auto *notice =
                           std::get_if<firmius::shared::NoticeContent>(
                               &part)) {
              flush_quick_cluster();
              auto notice_copy = *notice;
              bool suppress_notice = false;
              if (isObservationNotice(notice_copy)) {
                const auto &meta = *notice_copy.rollingMetadata;
                const auto range_key = observationRangeKey(meta);
                const int lifecycle_rank =
                    observationLifecycleRank(meta.lifecycle);
                if (range_key.has_value() && lifecycle_rank >= 0) {
                  const size_t sequence = observation_notice_sequence++;
                  auto latest_it =
                      latest_observation_notice_by_range.find(*range_key);
                  if (latest_it != latest_observation_notice_by_range.end()) {
                    const auto &latest = latest_it->second;
                    suppress_notice = latest.lifecycle_rank > lifecycle_rank ||
                                      (latest.lifecycle_rank == lifecycle_rank &&
                                       latest.sequence > sequence);
                  }
                }
              }
              if (suppress_notice) {
                continue;
              }
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [notice_copy, theme] {
                    return firmius::tui::IndentAgentRow(
                        RenderNoticeDisplay(theme, notice_copy));
                  });
              rows_.push_back(row);
              rows_.push_back(ftxui::Make<RowComponent>(
                  nullptr, [] { return ftxui::text(""); }));
            } else if (std::holds_alternative<firmius::shared::ImageContent>(
                           part)) {
              flush_quick_cluster();
              std::string indicator = "[Image]";
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [decorateMsg, indicator] {
                    return decorateMsg(ftxui::text(indicator));
                  });
              rows_.push_back(row);
              rows_.push_back(ftxui::Make<RowComponent>(
                  nullptr, [] { return ftxui::text(""); }));
            }
          }
        }

        if (auto footer = buildTurnFooterSummary(t); footer.has_value()) {
          flush_quick_cluster();
          const auto &theme =
              firmius::tui::ThemeManager::instance().getCurrentTheme();
          auto footer_text = *footer;
          auto row = ftxui::Make<RowComponent>(
              nullptr, [footer_text, theme] {
                return firmius::tui::IndentAgentRow(
                    ftxui::text(footer_text) |
                    ftxui::color(theme.chat.timestamp));
              });
          rows_.push_back(row);
          rows_.push_back(ftxui::Make<RowComponent>(
              nullptr, [] { return ftxui::text(""); }));
        }

      }
      flush_quick_cluster(true);
    }

    for (auto &row : rows_)
      history_inner_->Add(row);

    if (history_inner_->ChildCount() == 0) {
      history_inner_->Add(
          ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
    }

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
  std::function<bool()> show_internal_nudges_getter_;
  std::function<bool()> hide_errors_getter_;
  size_t last_history_revision_ = std::numeric_limits<std::size_t>::max();
  std::vector<ftxui::Component> rows_;
  std::vector<std::shared_ptr<CopyableRowComponent>> copyable_rows_;
  ftxui::Component history_inner_;
  ftxui::Component history_container_;
  ftxui::Component container_;
  ftxui::Component tail_spacer_;
  std::shared_ptr<firmius::tui::ScrollableBoxComponent> scrollable_;
  std::unordered_map<std::string, std::shared_ptr<firmius::tui::ToolCallView>>
      tool_views_;
  bool copy_drag_candidate_ = false;
  bool copy_drag_started_ = false;
  bool pending_copy_release_ = false;
  int press_x_ = -1;
  int press_y_ = -1;
  int last_drag_x_ = -1;
  int last_drag_y_ = -1;
  int pending_release_x_ = -1;
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
    std::function<bool()> show_internal_nudges_getter,
    std::function<bool()> hide_errors_getter) {
  return ftxui::Make<ChatWindowComponent>(
      std::move(history_getter), std::move(live_rows_provider),
      std::move(tool_view_provider), std::move(process_state_getter),
      std::move(subagent_state_getter), std::move(agent_focus_handler),
      std::move(sub_history_getter),
      std::move(sub_stream_getter), std::move(live_quick_summary_provider),
      std::move(show_internal_nudges_getter), std::move(hide_errors_getter));
}
