#ifndef FIRMIUS_TUI_ICONS_HPP
#define FIRMIUS_TUI_ICONS_HPP

#include <string>

namespace firmius::shared {

// Powerline separators
inline const std::string PL_LEFT_SEP = "";
inline const std::string PL_LEFT_SOFT_SEP = "";
inline const std::string PL_RIGHT_SEP = "";
inline const std::string PL_RIGHT_SOFT_SEP = "";

// Common Nerd Font icons
inline const std::string ICON_AGENT = "";
inline const std::string ICON_TOOL = "󱌣";
inline const std::string ICON_CONTEXT = "󰧑";
inline const std::string ICON_BRAIN = "󰧑";
inline const std::string ICON_METRICS = "󰓅";
inline const std::string ICON_ERROR = "󰅚";
inline const std::string ICON_WARNING = "󰀪";
inline const std::string ICON_CHECK = "󰄬";
inline const std::string ICON_WAIT = "󰔟";
inline const std::string ICON_GEAR = "󰒓";
inline const std::string ICON_FOLDER = "󰉋";
inline const std::string ICON_FILE = "󰈙";
inline const std::string ICON_FILE_EDIT = "󰏫";
inline const std::string ICON_SEARCH = "󰍉";
inline const std::string ICON_TERMINAL = "󰞷";
inline const std::string ICON_PERSONA = "󰧱";
inline const std::string ICON_CHIP = "󰘚";
inline const std::string ICON_TODO = "";
inline const std::string ICON_BOOK = "";
inline const std::string ICON_MEMORY = "󰍛";
inline const std::string ICON_ANCHOR = "󰯲";
inline const std::string ICON_DOWNLOAD = "󰇚";
inline const std::string ICON_MODEL = "⚙";

// Working-memory v2 surface
inline const std::string ICON_PIN = "󰐃";       // nf-md-pin
inline const std::string ICON_DEFLATE = "󰗈";   // nf-md-arrow-collapse-vertical
inline const std::string ICON_SAVINGS = "󰁆";   // nf-md-arrow-down-bold
inline const std::string ICON_RECALL = "󰑓";    // nf-md-restore (relevance fill)
inline const std::string ICON_EVICT = "󰩺";     // nf-md-broom (eviction)
inline const std::string ICON_THRESHOLD_OK = "󰗠"; // nf-md-check-circle
inline const std::string ICON_THRESHOLD_BUF = "󰂃";    // nf-md-battery-50
inline const std::string ICON_THRESHOLD_TGT = "󰂁";    // nf-md-battery-30
inline const std::string ICON_THRESHOLD_EMERG = "󰈸"; // nf-md-fire

} // namespace firmius::shared

#endif
