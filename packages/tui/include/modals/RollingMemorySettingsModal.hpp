#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class RollingMemorySettingsModal : public IModal {
public:
  std::string name() const override { return "rolling_memory_settings"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui
