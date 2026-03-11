#include "components/ANSIParser.hpp"
#include <ftxui/dom/elements.hpp>
#include <sstream>
#include <vector>

namespace firmius::tui {

// ANSI color code to FTXUI color
static ftxui::Color GetColorFromCode(int code) {
  if (code >= 30 && code <= 37) {
    // Standard foreground colors
    static const ftxui::Color colors[] = {
      ftxui::Color::Black,       // 30
      ftxui::Color::Red,         // 31
      ftxui::Color::Green,       // 32
      ftxui::Color::Yellow,      // 33
      ftxui::Color::Blue,        // 34
      ftxui::Color::Magenta,     // 35
      ftxui::Color::Cyan,        // 36
      ftxui::Color::White        // 37
    };
    return colors[code - 30];
  }
  if (code >= 90 && code <= 97) {
    // Bright foreground colors
    static const ftxui::Color colors[] = {
      ftxui::Color::GrayDark,    // 90
      ftxui::Color::RedLight,    // 91
      ftxui::Color::GreenLight,  // 92
      ftxui::Color::YellowLight, // 93
      ftxui::Color::BlueLight,   // 94
      ftxui::Color::MagentaLight,// 95
      ftxui::Color::CyanLight,   // 96
      ftxui::Color::White        // 97
    };
    return colors[code - 90];
  }
  if (code >= 40 && code <= 47) {
    // Background colors
    static const ftxui::Color colors[] = {
      ftxui::Color::Black,
      ftxui::Color::Red,
      ftxui::Color::Green,
      ftxui::Color::Yellow,
      ftxui::Color::Blue,
      ftxui::Color::Magenta,
      ftxui::Color::Cyan,
      ftxui::Color::White
    };
    return colors[code - 40];
  }
  return ftxui::Color::Default;
}

ftxui::Element ParseANSI(const std::string& text) {
  ftxui::Elements spans;
  std::string current_text;
  
  // Current style state
  ftxui::Color current_fg = ftxui::Color::Default;
  ftxui::Color current_bg = ftxui::Color::Default;
  bool bold = false;
  bool dim = false;
  bool underline = false;
  bool inverted = false;
  
  auto flush_span = [&]() {
    if (!current_text.empty()) {
      auto e = ftxui::text(current_text);
      if (current_fg != ftxui::Color::Default)
        e = e | ftxui::color(current_fg);
      if (current_bg != ftxui::Color::Default)
        e = e | ftxui::bgcolor(current_bg);
      if (bold)
        e = e | ftxui::bold;
      if (dim)
        e = e | ftxui::dim;
      if (underline)
        e = e | ftxui::underlined;
      if (inverted)
        e = e | ftxui::inverted;
      spans.push_back(e);
      current_text.clear();
    }
  };
  
  size_t i = 0;
  while (i < text.size()) {
    // Check for escape sequence
    if (i + 1 < text.size() && text[i] == '\x1b' && text[i + 1] == '[') {
      flush_span();
      
      // Find the end of the sequence (letter)
      size_t j = i + 2;
      while (j < text.size() && !std::isalpha(static_cast<unsigned char>(text[j]))) {
        j++;
      }
      
      if (j < text.size()) {
        char cmd = text[j];
        std::string params = text.substr(i + 2, j - i - 2);
        
        if (cmd == 'm') {
          // SGR (Select Graphic Rendition) - color/style
          if (params.empty()) {
            // Reset
            current_fg = ftxui::Color::Default;
            current_bg = ftxui::Color::Default;
            bold = false;
            dim = false;
            underline = false;
            inverted = false;
          } else {
            std::stringstream ss(params);
            std::string param;
            while (std::getline(ss, param, ';')) {
              int code = std::stoi(param);
              switch (code) {
                case 0: // Reset
                  current_fg = ftxui::Color::Default;
                  current_bg = ftxui::Color::Default;
                  bold = false;
                  dim = false;
                  underline = false;
                  inverted = false;
                  break;
                case 1: bold = true; break;
                case 2: dim = true; break;
                case 4: underline = true; break;
                case 7: inverted = true; break;
                case 22: bold = false; dim = false; break;
                case 24: underline = false; break;
                case 27: inverted = false; break;
                default:
                  if (code >= 30 && code <= 37) current_fg = GetColorFromCode(code);
                  else if (code >= 90 && code <= 97) current_fg = GetColorFromCode(code);
                  else if (code >= 40 && code <= 47) current_bg = GetColorFromCode(code);
                  break;
              }
            }
          }
        }
        // Ignore other commands
        
        i = j + 1;
        continue;
      }
    }
    
    current_text += text[i];
    i++;
  }
  
  flush_span();
  
  if (spans.empty())
    return ftxui::text("");
  
  return ftxui::hflow(std::move(spans));
}

std::vector<ftxui::Element> ParseANSILines(const std::string& text) {
  std::vector<ftxui::Element> lines;
  std::istringstream ss(text);
  std::string line;
  
  while (std::getline(ss, line)) {
    lines.push_back(ParseANSI(line));
  }
  
  // Handle trailing newline (empty last line)
  if (!text.empty() && text.back() == '\n') {
    lines.push_back(ftxui::text(""));
  }
  
  return lines;
}

} // namespace firmius::tui
