#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class ModelPickerModal : public IModal {
public:
  std::string name() const override { return "model_picker"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui
