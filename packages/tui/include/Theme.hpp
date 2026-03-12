#ifndef FIRMIUS_TUI_THEME_HPP
#define FIRMIUS_TUI_THEME_HPP

#include <ftxui/screen/color.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

struct ColorGroup {
  ftxui::Color bg;
  ftxui::Color fg;
};

struct StateColors {
  ColorGroup normal;
  ColorGroup focused;
  ColorGroup busy;
  ColorGroup error;
  std::vector<ftxui::Color> glint;
};

struct Theme {
  std::string name;

  struct Base {
    ftxui::Color bg;
    ftxui::Color fg;
    ftxui::Color border;
    ftxui::Color separator;
    ftxui::Color highlight;
    ftxui::Color dim;
  } base;

  struct StatusBar {
    StateColors idle;
    StateColors streaming;
    StateColors executing_tool;
    StateColors provider_waiting;
    StateColors compacting;
    StateColors error;

    ftxui::Color agent_bg;
    ftxui::Color agent_fg;
    ftxui::Color pill_bg;
    ftxui::Color pill_fg;
    ftxui::Color filler_bg;

    struct Context {
      ftxui::Color bg;
      ftxui::Color icon;
      ftxui::Color low;
      ftxui::Color medium;
      ftxui::Color high;
    } context;
  } status_bar;

  struct AgentStrip {
    ftxui::Color bg;
    StateColors item;

    struct Pills {
      ftxui::Color slug_bg;
      ftxui::Color slug_fg;
      ftxui::Color purpose_bg;
      ftxui::Color purpose_fg;
      ftxui::Color model_bg;
      ftxui::Color model_fg;
      ftxui::Color state_bg;
      ftxui::Color state_fg;
      ftxui::Color tool_bg;
      ftxui::Color tool_fg;
      ftxui::Color context_bg;
    } pills;
  } agent_strip;

  struct Chat {
    ftxui::Color bg;
    ftxui::Color user_prefix;
    ftxui::Color agent_prefix;
    ftxui::Color timestamp;

    struct Markdown {
      ftxui::Color text;
      ftxui::Color header;
      ftxui::Color code_bg;
      ftxui::Color code_fg;
      ftxui::Color link;
      ftxui::Color quote_bar;
      ftxui::Color quote_text;
    } markdown;
  } chat;

  struct Syntax {
    ftxui::Color keyword;
    ftxui::Color string;
    ftxui::Color comment;
    ftxui::Color number;
    ftxui::Color function;
    ftxui::Color type;
    ftxui::Color op;   // operator
    ftxui::Color attr; // attribute
    ftxui::Color constant;
    ftxui::Color variable;
    ftxui::Color tag;
  } syntax;

  struct ToolBlocks {
    ftxui::Color generic_bg;
    ftxui::Color generic_border;
    ftxui::Color generic_header_bg;
    ftxui::Color generic_title;
    ftxui::Color generic_icon;

    struct Specific {
      ColorGroup file_read;
      ColorGroup file_edit;
      ColorGroup terminal;
      ColorGroup subagent;
      ColorGroup ls;
      ColorGroup wait;
    } specific;

    std::vector<ftxui::Color> glint;
  } tool_blocks;

  struct Input {
    ftxui::Color bg;
    ftxui::Color fg;
    ftxui::Color prompt;
    ftxui::Color cursor;
    ftxui::Color placeholder;
  } input;

  struct Modals {
    ftxui::Color overlay; // dim background
    ftxui::Color bg;
    ftxui::Color fg;
    ftxui::Color border;
    ftxui::Color title;
    ftxui::Color highlight_bg;
    ftxui::Color highlight_fg;
    ftxui::Color button_bg;
    ftxui::Color button_fg;
  } modals;
};

} // namespace firmius::tui

#endif
