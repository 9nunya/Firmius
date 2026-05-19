#pragma once

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
