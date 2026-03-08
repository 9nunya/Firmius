#pragma once

#include "modals/IModal.hpp"
#include <functional>
#include <string>

namespace firmius::tui {

class ConfirmationModal : public IModal {
public:
  ConfirmationModal(std::string title, std::string message,
                    std::function<void()> onConfirm,
                    std::function<void()> onCancel = nullptr);

  std::string name() const override { return "confirmation"; }
  ftxui::Component create(TuiState &state) override;

private:
  std::string title_;
  std::string message_;
  std::function<void()> onConfirm_;
  std::function<void()> onCancel_;
};

} // namespace firmius::tui
