#ifndef FIRMIUS_COMPONENTS_INPUT_BAR_HPP
#define FIRMIUS_COMPONENTS_INPUT_BAR_HPP

#include <ftxui/component/component_base.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

struct PastedBlock {
  std::string type;      // "image" or "text"
  std::string id;        // unique identifier
  size_t line_count = 0; // for text pastes
  std::string content;   // actual pasted content (for text or image base64)
  std::string mime_type; // MIME type for images (e.g., "image/png")
  size_t start_pos = 0;  // position in buffer where placeholder starts
  size_t end_pos = 0;    // position in buffer where placeholder ends
};

struct InputBarModel {
  std::string *buffer = nullptr;
  int *cursor = nullptr;
  std::string placeholder;
  std::vector<PastedBlock> pasted_blocks; // blocks embedded in buffer
  std::vector<PastedBlock> image_tags;    // images shown above input
  bool is_focused = true;
  
  // Function to check if current model supports vision
  // Returns true if model supports image input, false otherwise
  std::function<bool()> check_vision_capable = nullptr;
  
  // Function to show notification
  std::function<void(const std::string& title, const std::string& message)> show_notification = nullptr;
};

ftxui::Component InputBar(
    const std::shared_ptr<InputBarModel> &model,
    std::function<void(const std::string &,
                       const std::vector<PastedBlock> &images)>
        on_submit);

} // namespace firmius::tui

#endif
