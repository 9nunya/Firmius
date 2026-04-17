#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class McpModal : public IModal {
public:
  std::string name() const override { return "mcp"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui
