#include "components/ToolBlock.hpp"
#include "ThemeManager.hpp"
#include "SkinConfig.hpp"
#include "SkinConfig.hpp"
#include "UserPreferences.hpp"
#include "UserPreferences.hpp"
#include "components/GlintEffect.hpp"
#include "components/ToolPresentationBlock.hpp"
#include "tools/SubagentToolPresentation.hpp"
#include "tools/ToolPresentation.hpp"
#include "utils/ErrorCleaner.hpp"
#include "utils/Icons.hpp"
#include "utils/ModelUtil.hpp"
#include "utils/ToolSummaries.hpp"
#include <rapidjson/document.h>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <algorithm>
#include <sstream>
#include <ftxui/component/component.hpp>

namespace firmius::tui {

namespace {

std::string Trim(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(
      std::find_if(value.rbegin(), value.rend(), not_space).base(),
      value.end());
  return value;
}

std::string ClampLine(const std::string& text, size_t width) {
  std::string single = text;
  std::replace(single.begin(), single.end(), '\n', ' ');
  std::replace(single.begin(), single.end(), '\r', ' ');
  single = Trim(single);
  if (single.size() <= width) {
    return single;
  }
  if (width <= 3) {
    return single.substr(0, width);
  }
  return single.substr(0, width - 3) + "...";
}

bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

struct SubagentDescriptor {
  std::string agent_id;
  std::string title;
  std::string friendly_name;
  std::string task;
  std::string purpose;
  std::string model;
};

void FillFromSubagentArgs(SubagentDescriptor& desc,
                          const std::shared_ptr<ToolCallView>& view) {
  if (!view || view->args.empty()) {
    return;
  }
  rapidjson::Document doc;
  doc.Parse(view->args.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return;
  }
  if (desc.agent_id.empty() && doc.HasMember("agent_id") &&
      doc["agent_id"].IsString()) {
    desc.agent_id = doc["agent_id"].GetString();
  }
  if (desc.title.empty() && doc.HasMember("title") && doc["title"].IsString()) {
    desc.title = doc["title"].GetString();
  }
  if (desc.friendly_name.empty() && doc.HasMember("name") &&
      doc["name"].IsString()) {
    desc.friendly_name = doc["name"].GetString();
  }
  if (desc.task.empty() && doc.HasMember("task") && doc["task"].IsString()) {
    desc.task = doc["task"].GetString();
  }
  if (desc.purpose.empty() && doc.HasMember("persona") &&
      doc["persona"].IsString()) {
    desc.purpose = doc["persona"].GetString();
  }
  if (desc.model.empty() && doc.HasMember("model") && doc["model"].IsString()) {
    desc.model = doc["model"].GetString();
  }
}

void FillFromSubagentResult(SubagentDescriptor& desc,
                            const std::shared_ptr<ToolCallView>& view) {
  if (!view || view->result.empty()) {
    return;
  }
  rapidjson::Document doc;
  doc.Parse(view->result.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return;
  }
  if (desc.agent_id.empty() && doc.HasMember("agentId") &&
      doc["agentId"].IsString()) {
    desc.agent_id = doc["agentId"].GetString();
  }
}

SubagentDescriptor DescribeSubagent(const std::shared_ptr<ToolCallView>& view,
                                    const NormalizedSubagentState* state) {
  SubagentDescriptor desc;
  if (state) {
    desc.agent_id = state->child_agent_id;
    desc.title = state->child_title;
    desc.friendly_name = state->child_friendly_name;
    desc.task = state->task;
  }
  if (desc.agent_id.empty() && view) {
    desc.agent_id = view->subagent_id;
  }
  if (desc.title.empty() && view) {
    desc.title = view->subagent_title;
  }
  if (desc.friendly_name.empty() && view) {
    desc.friendly_name = view->subagent_slug;
  }
  FillFromSubagentArgs(desc, view);
  FillFromSubagentResult(desc, view);
  if (desc.title.empty()) {
    desc.title = !desc.friendly_name.empty() ? desc.friendly_name
                 : !desc.task.empty()        ? desc.task
                                             : "Subagent";
  }
  return desc;
}

std::string StatusText(const std::shared_ptr<ToolCallView>& view,
                       const NormalizedSubagentState* state,
                       const StreamState* stream) {
  if (stream && stream->is_thinking) {
    return "thinking";
  }
  if (state) {
    if (state->retrying) return "retrying";
    if (state->provider_waiting) return "provider waiting";
    if (state->waiting) return "waiting";
    if (state->running) return "working";
    if (state->wait_state == "cancelled") return "interrupted";
    if (state->wait_state == "failed") return "failed";
    if (state->wait_state == "completed" ||
        state->wait_state == "completed_no_summary") {
      return "completed";
    }
    if (!state->wait_state.empty()) return state->wait_state;
  }
  if (!view) return "idle";
  if (view->phase == ToolPhase::Preparing) return "preparing";
  if (view->phase == ToolPhase::Called || view->phase == ToolPhase::BackgroundRunning) {
    return "working";
  }
  if (view->phase == ToolPhase::Error || !view->success) return "failed";
  return "completed";
}

ftxui::Color StatusAccent(const Theme& theme, const std::string& status) {
  if (status == "failed") {
    return theme.status_bar.error.normal.fg;
  }
  if (status == "completed") {
    return theme.tool_blocks.specific.subagent.fg;
  }
  if (status == "interrupted") {
    return theme.base.highlight;
  }
  if (status == "provider waiting" || status == "waiting" || status == "thinking" ||
      status == "retrying" || status == "working" || status == "preparing") {
    return theme.status_bar.provider_waiting.normal.fg;
  }
  return theme.tool_blocks.specific.subagent.fg;
}

bool IsActiveStatus(const std::string& status) {
  return status == "preparing" || status == "working" || status == "thinking" ||
         status == "waiting" || status == "provider waiting" ||
         status == "retrying";
}

bool IsDoneStatus(const std::string& status) {
  return status == "completed" || status == "interrupted";
}

struct RenderedLogLine {
  std::string text;
  bool glint = false;
};

std::vector<RenderedLogLine> BuildSubagentLogLines(
    const std::shared_ptr<ToolCallView>& view, const NormalizedSubagentState* state,
    const StreamState* stream,
    HistoryGetter history_getter) {
  auto build_from_entries =
      [](const std::vector<firmius::shared::SubagentToolLogEntry>& entries) {
        std::vector<RenderedLogLine> lines;
        for (const auto& entry : entries) {
          std::string summary = Trim(entry.summary);
          if (summary.empty()) {
            continue;
          }
          if (StartsWith(summary, "State: ") || summary == "Done" ||
              summary == "provider waiting" ||
              StartsWith(summary, "Provider waiting") ||
              StartsWith(summary, "Spawned ")) {
            continue;
          }
          if (StartsWith(summary, "Failed: ")) {
            summary = firmius::shared::ErrorCleaner::clean(summary);
          }
          lines.push_back({summary, entry.phase == ToolPhase::Preparing});
        }
        return lines;
      };

  auto build_from_history =
      [](const firmius::shared::AgentHistory* history) {
        std::vector<RenderedLogLine> lines;
        if (!history) {
          return lines;
        }
        for (const auto& turn : history->turns) {
          for (const auto& msg : turn.messages) {
            for (const auto& content : msg.content) {
              if (auto* th = std::get_if<firmius::shared::ThinkingContent>(&content)) {
                const std::string thinking = Trim(th->thinking);
                if (!thinking.empty()) {
                  lines.push_back({"Thought", false});
                }
                continue;
              }
              if (auto* tc = std::get_if<firmius::shared::ToolCallContent>(&content)) {
                std::string summary = Trim(firmius::shared::SummarizeToolCall(
                    tc->name, tc->args, firmius::shared::ToolPhase::Finished));
                if (summary.empty() || summary == "provider waiting" ||
                    StartsWith(summary, "Provider waiting")) {
                  continue;
                }
                lines.push_back({summary, false});
                continue;
              }
              if (auto* txt = std::get_if<firmius::shared::TextContent>(&content)) {
                std::string text = Trim(txt->text);
                if (text.empty()) {
                  continue;
                }
                size_t newline = text.find('\n');
                if (newline != std::string::npos) {
                  text = text.substr(0, newline);
                }
                lines.push_back({text, false});
              }
            }
          }
        }
        return lines;
      };

  std::vector<RenderedLogLine> lines;
  if (state && !state->activity_log.empty()) {
    lines = build_from_entries(state->activity_log);
  }
  if (lines.empty() && view && !view->subagent_tool_log.empty()) {
    lines = build_from_entries(view->subagent_tool_log);
  }
  if (lines.empty() && state && !state->activity_log.empty()) {
    for (const auto& entry : state->activity_log) {
      std::string summary = Trim(entry.summary);
      if (summary.empty()) {
        continue;
      }
      lines.push_back({summary, entry.phase == ToolPhase::Preparing});
    }
  }
  if (lines.empty() && history_getter) {
    std::string child_agent_id;
    if (state && !state->child_agent_id.empty()) {
      child_agent_id = state->child_agent_id;
    } else if (view && !view->subagent_id.empty()) {
      child_agent_id = view->subagent_id;
    } else if (view && !view->result.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->result.c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("agentId") &&
          doc["agentId"].IsString()) {
        child_agent_id = doc["agentId"].GetString();
      }
    }
    if (!child_agent_id.empty()) {
      lines = build_from_history(history_getter(child_agent_id));
    }
  }

  if (stream && stream->is_thinking) {
    if (!lines.empty() && StartsWith(lines.back().text, "Thought for ")) {
      lines.pop_back();
    }
    lines.push_back({"Thinking..", true});
  }

  if (lines.size() > 5) {
    lines.erase(lines.begin(), lines.end() - 5);
  }
  return lines;
}

class SubagentToolBlockComponent : public ftxui::ComponentBase {
public:
  SubagentToolBlockComponent(const std::shared_ptr<ToolCallView>& view,
                             HistoryGetter history_getter,
                             StreamGetter stream_getter,
                             SubagentStateGetter subagent_state_getter,
                             AgentFocusHandler agent_focus_handler)
      : view_(view),
        history_getter_(std::move(history_getter)),
        stream_getter_(std::move(stream_getter)),
        subagent_state_getter_(std::move(subagent_state_getter)),
        agent_focus_handler_(std::move(agent_focus_handler)) {}

  bool OnEvent(ftxui::Event event) override {
    if (!event.is_mouse()) {
      return ComponentBase::OnEvent(event);
    }
    const auto mouse = event.mouse();
    const bool inside = box_.Contain(mouse.x, mouse.y);
    hovered_ = inside;
    if (inside && mouse.button == ftxui::Mouse::Left &&
        mouse.motion == ftxui::Mouse::Pressed) {
      const auto* state = subagent_state_getter_ ? subagent_state_getter_(view_->toolCallId) : nullptr;
      auto desc = DescribeSubagent(view_, state);
      if (!desc.agent_id.empty()) {
        if (agent_focus_handler_) {
          agent_focus_handler_(desc.agent_id);
        }
        return true;
      }
    }
    return inside;
  }

  ftxui::Element OnRender() override {
    const auto& theme = ThemeManager::instance().getCurrentTheme();
    const auto* state =
        subagent_state_getter_ ? subagent_state_getter_(view_->toolCallId) : nullptr;
    SubagentDescriptor desc = DescribeSubagent(view_, state);
    const StreamState* stream = nullptr;
    if (stream_getter_ && !desc.agent_id.empty()) {
      stream = stream_getter_(desc.agent_id);
    }
    const std::string status = StatusText(view_, state, stream);
    const bool active = IsActiveStatus(status);
    const bool done = IsDoneStatus(status);
    const auto sub_colors = theme.tool_blocks.specific.subagent;
    const ftxui::Color accent = StatusAccent(theme, status);
    const ftxui::Color block_bg =
        hovered_ ? theme.tool_blocks.generic_border
                 : theme.tool_blocks.generic_header_bg;
    const ftxui::Color header_bg = hovered_ ? sub_colors.bg : theme.tool_blocks.generic_bg;
    const ftxui::Color title_fg = done ? theme.tool_blocks.generic_title
                                       : sub_colors.fg;
    const ftxui::Color accent_fg = accent;
    const std::string icon =
        status == "failed" ? firmius::shared::ICON_ERROR
        : done            ? firmius::shared::ICON_CHECK
                          : firmius::shared::ICON_WAIT;

    auto title_text =
        ftxui::text(" " + icon + " " + desc.title + " ") | ftxui::bold |
        ftxui::color(title_fg) | ftxui::bgcolor(header_bg);
    ftxui::Element title_el = title_text;
    if (active) {
      GlintConfig cfg;
      const auto preferences = loadUserPreferences();
      const SkinKind skin = preferences.skin_kind.value_or(SkinKind::Firmius);
      const SkinConfig skin_config =
          skin == SkinKind::Claudex ? preferences.claudex_skin.value_or(defaultSkinConfig(SkinKind::Claudex)) : preferences.firmius_skin.value_or(defaultSkinConfig(SkinKind::Firmius));
      cfg.gradientColors = theme.tool_blocks.glint.empty()
                               ? std::vector<ftxui::Color>{title_fg,
                                                           theme.base.highlight}
                               : theme.tool_blocks.glint;
      cfg.durationSeconds = glintDurationSeconds(skin_config.glint_speed);
      cfg.intervalSeconds = glintIntervalSeconds(skin_config.glint_speed);
      cfg.easing = GlintEasing::EaseInOut;
      cfg.includeWhitespace = true;
      title_el = GlintEffect(title_text, cfg)->Render();
    }

    // meta line: what the user expects to see (friendly name / purpose / model)
    // Avoid surfacing internal agent IDs or task fragments here; those belong in detail views.
    std::vector<ftxui::Element> meta_parts;
    if (!desc.friendly_name.empty()) {
      meta_parts.push_back(ftxui::text(desc.friendly_name) |
                           ftxui::color(theme.base.dim));
    }
    if (!desc.purpose.empty()) {
      if (!meta_parts.empty()) meta_parts.push_back(ftxui::text("  •  ") | ftxui::color(theme.base.dim));
      meta_parts.push_back(ftxui::text(desc.purpose) | ftxui::color(theme.base.dim));
    }
    if (!desc.model.empty()) {
      if (!meta_parts.empty()) meta_parts.push_back(ftxui::text("  •  ") | ftxui::color(theme.base.dim));
      meta_parts.push_back(ftxui::text(desc.model) | ftxui::color(theme.base.dim));
    }
    if (state && !state->artifacts_created.empty()) {
      if (!meta_parts.empty()) meta_parts.push_back(ftxui::text("  •  ") | ftxui::color(theme.base.dim));
      meta_parts.push_back(ftxui::text(
                               "+" +
                               std::to_string(state->artifacts_created.size()) +
                               " artifact(s)") |
                           ftxui::color(theme.base.dim));
    }
    if (state && !state->artifacts_updated.empty()) {
      if (!meta_parts.empty()) meta_parts.push_back(ftxui::text("  •  ") | ftxui::color(theme.base.dim));
      meta_parts.push_back(ftxui::text(
                               "~" +
                               std::to_string(state->artifacts_updated.size()) +
                               " artifact(s)") |
                           ftxui::color(theme.base.dim));
    }

    std::vector<RenderedLogLine> log_lines =
        BuildSubagentLogLines(view_, state, stream, history_getter_);
    const int available_width = std::max(28, ftxui::Terminal::Size().dimx - 30);
    ftxui::Elements log_rows;
    if (log_lines.empty()) {
      log_rows.push_back(ftxui::text("  waiting for activity...") |
                         ftxui::color(theme.base.dim));
    } else {
      for (const auto& line : log_lines) {
        auto base = ftxui::text("  " + ClampLine(line.text, static_cast<size_t>(available_width))) |
                    ftxui::color(line.text.find("Failed") != std::string::npos
                                     ? theme.status_bar.error.normal.fg
                                     : theme.base.fg);
        ftxui::Element rendered = base;
        if (line.glint) {
          const auto preferences = loadUserPreferences();
          const SkinKind skin = preferences.skin_kind.value_or(SkinKind::Firmius);
          const SkinConfig skin_config =
              skin == SkinKind::Claudex ? preferences.claudex_skin.value_or(defaultSkinConfig(SkinKind::Claudex)) : preferences.firmius_skin.value_or(defaultSkinConfig(SkinKind::Firmius));
          GlintConfig cfg;
          cfg.gradientColors = theme.tool_blocks.glint.empty()
                                   ? std::vector<ftxui::Color>{line.text.find("Failed") != std::string::npos
                                                                 ? theme.status_bar.error.normal.fg
                                                                 : theme.base.fg,
                                                               theme.base.highlight}
                                   : theme.tool_blocks.glint;
          cfg.durationSeconds = glintDurationSeconds(skin_config.glint_speed);
          cfg.intervalSeconds = glintIntervalSeconds(skin_config.glint_speed);
          cfg.easing = GlintEasing::EaseInOut;
          cfg.includeWhitespace = true;
          rendered = GlintEffect(base, cfg)->Render();
        }
        log_rows.push_back(rendered);
      }
    }

    auto log_window =
        ftxui::vbox(std::move(log_rows)) | ftxui::bgcolor(block_bg);

    ftxui::Elements top_rows;
    top_rows.push_back(
        ftxui::hbox({title_el, ftxui::filler(),
                     ftxui::text(" " + status + " ") | ftxui::bold |
                         ftxui::color(accent_fg) | ftxui::bgcolor(header_bg)}) |
        ftxui::bgcolor(header_bg));
    if (!meta_parts.empty()) {
      top_rows.push_back(ftxui::hbox(std::move(meta_parts)) |
                         ftxui::bgcolor(block_bg));
    }
    top_rows.push_back(log_window);

    auto content = ftxui::vbox(std::move(top_rows)) | ftxui::bgcolor(block_bg) |
                   ftxui::xflex;
    auto edge =
        ftxui::text("│") | ftxui::color(accent) | ftxui::bgcolor(block_bg);
    return ftxui::hbox({edge, content}) | ftxui::xflex | ftxui::reflect(box_);
  }

private:
  std::shared_ptr<ToolCallView> view_;
  HistoryGetter history_getter_;
  StreamGetter stream_getter_;
  SubagentStateGetter subagent_state_getter_;
  AgentFocusHandler agent_focus_handler_;
  ftxui::Box box_;
  bool hovered_ = false;
};

} // namespace

ftxui::Component ToolBlock(const std::shared_ptr<ToolCallView> &view,
                           HistoryGetter history_getter,
                           StreamGetter stream_getter,
                           ProcessStateGetter process_state_getter,
                           SubagentStateGetter subagent_state_getter,
                           AgentFocusHandler agent_focus_handler) {
  if (!view)
    return ftxui::Renderer([] { return ftxui::text("Missing tool view"); });

  (void)history_getter;
  (void)stream_getter;

  if (view->name == "Delegate" &&
      view->args.find("\"action\":\"Spawn\"") != std::string::npos) {
    return ftxui::Make<SubagentToolBlockComponent>(
        view, std::move(history_getter), std::move(stream_getter),
        std::move(subagent_state_getter), std::move(agent_focus_handler));
  }

  return ToolPresentationBlock(view, [view, process_state_getter,
                                      subagent_state_getter] {
    const NormalizedProcessState *process_state = nullptr;
    const NormalizedSubagentState *subagent_state = nullptr;
    if (process_state_getter && view) {
      process_state = process_state_getter(view->toolCallId);
    }
    if (subagent_state_getter && view) {
      subagent_state = subagent_state_getter(view->toolCallId);
    }
    return BuildToolPresentation(*view, process_state, subagent_state);
  }, []() {
    const auto preferences = loadUserPreferences();
    return preferences.skin_kind.value_or(SkinKind::Firmius) ==
           SkinKind::Claudex;
  });
}

} // namespace firmius::tui
