#ifndef FIRMIUS_TUI_OVERLAY_HPP
#define FIRMIUS_TUI_OVERLAY_HPP

#include "Component.hpp"
#include "Terminal.hpp"

#include <string>
#include <vector>

namespace firmius::tui {

class Overlay : public Component {
public:
  virtual ~Overlay() = default;

  virtual bool isActive() const = 0;

  virtual bool handleInput(const std::string& key) = 0;

  virtual bool handleMouse(const MouseEvent& event,
                           int screenRow,
                           int screenCol) = 0;

  virtual void open() = 0;
  virtual void close() = 0;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_OVERLAY_HPP
