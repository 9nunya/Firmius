#pragma once
#include <ftxui/screen/color.hpp>

namespace firmius::tui::colors {

// Terminal-respectful semantic colors
inline ftxui::Color thinking() { return ftxui::Color::GrayDark; }
inline ftxui::Color codeBlockBg() { return ftxui::Color::RGB(30, 30, 40); }
inline ftxui::Color codeBlockBorder() { return ftxui::Color::RGB(80, 80, 100); }
inline ftxui::Color toolName() { return ftxui::Color::Cyan; }
inline ftxui::Color toolSuccess() { return ftxui::Color::Green; }
inline ftxui::Color toolError() { return ftxui::Color::Red; }
inline ftxui::Color diffAdd() { return ftxui::Color::Green; }
inline ftxui::Color diffRemove() { return ftxui::Color::Red; }
inline ftxui::Color interrupted() { return ftxui::Color::Yellow; }
inline ftxui::Color footer() { return ftxui::Color::GrayDark; }
inline ftxui::Color subagentStatus() { return ftxui::Color::Magenta; }
inline ftxui::Color inputPrompt() { return ftxui::Color::Blue; }
inline ftxui::Color slashCommand() { return ftxui::Color::Yellow; }
inline ftxui::Color compaction() { return ftxui::Color::Yellow; }
inline ftxui::Color retry() { return ftxui::Color::Yellow; }
inline ftxui::Color queuedTag() { return ftxui::Color::GrayDark; }

} // namespace firmius::tui::colors
