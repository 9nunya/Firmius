#pragma once

#include "modals/IModal.hpp"
#include <string>

namespace firmius::tui {

class ThreadPickerModal : public IModal {
public:
  std::string name() const override { return "thread_picker"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui
