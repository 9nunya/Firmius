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
#include "utils/ToolSummaries.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <limits>
#include <memory>
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
              }
            },
            part);
      }
    }
  }

  return seed;
}

class RowComponent : public ftxui::ComponentBase {
public:
  RowComponent(ftxui::Component child, std::function<ftxui::Element()> render)
      : child_(std::move(child)), render_(std::move(render)) {
    if (child_)
      Add(child_);
  }

  ftxui::Element OnRender() override { return render_(); }
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
  std::string keyword;
  switch (summary.category) {
  case firmius::tui::QuickToolCategory::Read:
    keyword = summary.has_error ? "Failed Reading" : (live ? "Reading..." : "Read");
    break;
  case firmius::tui::QuickToolCategory::List:
    keyword = summary.has_error ? "Failed Listing" : (live ? "Listing..." : "Listed");
    break;
  case firmius::tui::QuickToolCategory::Search:
    keyword = summary.has_error ? "Failed Search" : (live ? "Searching..." : "Search");
    break;
  case firmius::tui::QuickToolCategory::None:
    return ftxui::text("");
  }
  const int live_count = summary.preparing_count + summary.live_count;
  if (live && live_count > 1) {
    keyword += " (x" + std::to_string(live_count) + ")";
  }

  std::vector<std::string> deduped_targets;
  std::unordered_map<std::string, bool> seen_targets;
  for (const auto &target : summary.targets) {
    if (target.empty() || seen_targets[target]) {
      continue;
    }
    seen_targets[target] = true;
    deduped_targets.push_back(target);
  }

  std::string joined_targets;
  for (size_t i = 0; i < deduped_targets.size(); ++i) {
    if (i > 0) {
      joined_targets += ", ";
    }
    joined_targets += deduped_targets[i];
  }

  auto accent = theme.status_bar.executing_tool.normal.fg;
  auto bullet_color = theme.base.dim;
  auto target_color = theme.base.fg;

  if (summary.category == firmius::tui::QuickToolCategory::Search) {
    accent = theme.tool_blocks.specific.file_read.fg;
  } else if (summary.category == firmius::tui::QuickToolCategory::List) {
    accent = theme.tool_blocks.specific.ls.fg;
  } else if (summary.category == firmius::tui::QuickToolCategory::Read) {
    accent = theme.tool_blocks.specific.file_read.fg;
  }

  if (summary.has_error) {
    accent = theme.status_bar.error.normal.fg;
    bullet_color = theme.status_bar.error.normal.fg;
    target_color = theme.status_bar.error.normal.fg;
  } else if (summary.has_live) {
    accent = theme.status_bar.streaming.normal.fg;
    target_color = theme.base.fg;
  } else if (summary.has_preparing) {
    accent = theme.status_bar.executing_tool.normal.fg;
    target_color = theme.base.fg;
  }

  auto keyword_el = ftxui::text(keyword) | ftxui::bold | ftxui::color(accent);
  auto targets_el =
      ftxui::paragraph(joined_targets.empty() ? "." : joined_targets) |
      ftxui::color(target_color) | ftxui::flex;
  if (summary.has_live) {
    targets_el = targets_el | ftxui::bold;
  }

  auto row_base = ftxui::hbox({
                      ftxui::text("· ") | ftxui::color(bullet_color),
                      keyword_el,
                      ftxui::text(" ") | ftxui::color(bullet_color),
                      targets_el,
                  }) |
                  ftxui::bgcolor(theme.tool_blocks.generic_bg) | ftxui::xflex;

  if (!summary.has_preparing && !summary.has_live) {
    return row_base;
  }

  firmius::tui::GlintConfig cfg;
  cfg.target = firmius::tui::GlintConfig::Target::Text;
  cfg.gradientColors = theme.tool_blocks.glint.empty()
                           ? std::vector<ftxui::Color>{accent, theme.base.fg,
                                                       accent}
                           : theme.tool_blocks.glint;
  cfg.glintSize = 10;
  cfg.intervalSeconds = summary.has_live ? 1.1f : 1.8f;
  cfg.durationSeconds = summary.has_live ? 0.9f : 1.1f;
  cfg.easing = firmius::tui::GlintEasing::EaseInOut;
  auto glint = firmius::tui::GlintEffect(row_base, cfg);
  return glint->Render();
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
      firmius::tui::HistoryGetter sub_history_getter,
      firmius::tui::StreamGetter sub_stream_getter,
      firmius::tui::LiveQuickSummaryProvider live_quick_summary_provider,
      std::function<bool()> show_internal_nudges_getter)
      : history_getter_(std::move(history_getter)),
        live_rows_provider_(std::move(live_rows_provider)),
        tool_view_provider_(std::move(tool_view_provider)),
        process_state_getter_(std::move(process_state_getter)),
        subagent_state_getter_(std::move(subagent_state_getter)),
        sub_history_getter_(std::move(sub_history_getter)),
        sub_stream_getter_(std::move(sub_stream_getter)),
        live_quick_summary_provider_(std::move(live_quick_summary_provider)),
        show_internal_nudges_getter_(
            std::move(show_internal_nudges_getter)) {

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

    RebuildIfNeeded();
    return scrollable_ ? scrollable_->OnEvent(event) : false;
  }

private:
  void RebuildIfNeeded() {
    auto *history = history_getter_ ? history_getter_() : nullptr;
    const std::size_t history_revision = ComputeHistoryRevision(history);
    if (history_revision == last_history_revision_)
      return;
    last_history_revision_ = history_revision;

    rows_.clear();
    history_inner_->DetachAllChildren();

    if (history) {
      const bool showInternalNudges =
          show_internal_nudges_getter_ ? show_internal_nudges_getter_() : false;
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
            int width = ftxui::Terminal::Size().dimx;
            std::string label_text = " " + label + " ";
            if (width <= static_cast<int>(label_text.size())) {
              return ftxui::text(label_text) | ftxui::dim | ftxui::center |
                     ftxui::flex;
            }
            int left = (width - static_cast<int>(label_text.size())) / 2;
            int right = width - static_cast<int>(label_text.size()) - left;
            std::string line =
                std::string(left, '-') + label_text + std::string(right, '-');
            return ftxui::text(line) | ftxui::dim | ftxui::flex;
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
                rows_.push_back(ftxui::Make<RowComponent>(
                    nullptr, [text = std::move(text)] {
                      return firmius::tui::IndentAgentRow(
                          firmius::tui::RenderMarkdown(text));
                    }));
              } else if (auto *thk = std::get_if<firmius::shared::ThinkingContent>(&part)) {
                std::string thinking = thk->thinking;
                rows_.push_back(ftxui::Make<RowComponent>(
                    nullptr, [thinking = std::move(thinking)] {
                      return firmius::tui::IndentAgentRow(
                          firmius::tui::RenderMarkdown(thinking, true));
                    }));
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

          auto decorateMsg = [prefixColor,
                              prefix, isUser](const ftxui::Element &content) {
            auto e = ftxui::hbox({
                ftxui::text(prefix) | ftxui::bold | ftxui::color(prefixColor),
                content | ftxui::xflex,
            });
            if (isUser) {
              return e | ftxui::xflex;
            }
            return firmius::tui::IndentAgentRow(e);
          };

          for (const auto &part : msg.content) {
            if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
              flush_quick_cluster();
              std::string text = txt->text;
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [decorateMsg, text = std::move(text)] {
                    return decorateMsg(firmius::tui::RenderMarkdown(text));
                  });
              rows_.push_back(row);
            } else if (auto *thk =
                           std::get_if<firmius::shared::ThinkingContent>(
                               &part)) {
              flush_quick_cluster();
              std::string thinking = thk->thinking;
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [decorateMsg, thinking = std::move(thinking)] {
                    return decorateMsg(
                        firmius::tui::RenderMarkdown(thinking, true));
                  });
              rows_.push_back(row);
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
                  view, nullptr, nullptr, process_state_getter_,
                  subagent_state_getter_);
              auto row = ftxui::Make<RowComponent>(block, [decorateMsg, block] {
                return decorateMsg(block->Render());
              });
              rows_.push_back(row);
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
                    view, nullptr, nullptr, process_state_getter_,
                    subagent_state_getter_);
                auto row =
                    ftxui::Make<RowComponent>(block, [decorateMsg, block] {
                      return decorateMsg(block->Render());
                    });
                rows_.push_back(row);
                seen_tool_call[tr->toolCallId] = true;
              }
            } else if (auto *err =
                           std::get_if<firmius::shared::ErrorContent>(&part)) {
              flush_quick_cluster();
              auto error = *err;
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [error, theme] {
                    return firmius::tui::IndentAgentRow(
                        RenderErrorDisplay(theme, error));
                  });
              rows_.push_back(row);
            } else if (auto *notice =
                           std::get_if<firmius::shared::NoticeContent>(
                               &part)) {
              flush_quick_cluster();
              auto notice_copy = *notice;
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [notice_copy, theme] {
                    return firmius::tui::IndentAgentRow(
                        RenderNoticeDisplay(theme, notice_copy));
                  });
              rows_.push_back(row);
            } else if (std::holds_alternative<firmius::shared::ImageContent>(
                           part)) {
              flush_quick_cluster();
              std::string indicator = "[Image]";
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [decorateMsg, indicator] {
                    return decorateMsg(ftxui::text(indicator));
                  });
              rows_.push_back(row);
            }
          }
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
  firmius::tui::HistoryGetter sub_history_getter_;
  firmius::tui::StreamGetter sub_stream_getter_;
  firmius::tui::LiveQuickSummaryProvider live_quick_summary_provider_;
  std::function<bool()> show_internal_nudges_getter_;
  size_t last_history_revision_ = std::numeric_limits<std::size_t>::max();
  std::vector<ftxui::Component> rows_;
  ftxui::Component history_inner_;
  ftxui::Component history_container_;
  ftxui::Component container_;
  ftxui::Component tail_spacer_;
  std::shared_ptr<firmius::tui::ScrollableBoxComponent> scrollable_;
  std::unordered_map<std::string, std::shared_ptr<firmius::tui::ToolCallView>>
      tool_views_;
};

} // namespace

ftxui::Component firmius::tui::ChatWindow(
    std::function<const shared::AgentHistory *()> history_getter,
    std::function<std::vector<ftxui::Element>()> live_rows_provider,
    ToolViewProvider tool_view_provider,
    ProcessStateGetter process_state_getter,
    SubagentStateGetter subagent_state_getter,
    firmius::tui::HistoryGetter sub_history_getter,
    firmius::tui::StreamGetter sub_stream_getter,
    firmius::tui::LiveQuickSummaryProvider live_quick_summary_provider,
    std::function<bool()> show_internal_nudges_getter) {
  return ftxui::Make<ChatWindowComponent>(
      std::move(history_getter), std::move(live_rows_provider),
      std::move(tool_view_provider), std::move(process_state_getter),
      std::move(subagent_state_getter),
      std::move(sub_history_getter),
      std::move(sub_stream_getter), std::move(live_quick_summary_provider),
      std::move(show_internal_nudges_getter));
}
