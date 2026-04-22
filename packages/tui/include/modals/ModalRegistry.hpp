#pragma once

#include "IModal.hpp"
#include <map>
#include <memory>
#include <string>

namespace firmius::tui {

class TuiState; // Forward declaration

/// Singleton registry of all available modals.
/// Commands open modals by name; this class resolves the name to an IModal,
/// calls create(), and pushes the result onto TuiState's modal stack.
class ModalRegistry {
public:
  static ModalRegistry &instance();

  ModalRegistry(const ModalRegistry &) = delete;
  ModalRegistry &operator=(const ModalRegistry &) = delete;

  void registerModal(std::shared_ptr<IModal> modal);

  /// Look up a modal by name, call create(), and push onto state.
  /// Returns false if the modal name is not found.
  bool openModal(const std::string &name, TuiState &state,
                  bool profile_open = true);

private:
  ModalRegistry() = default;
  ~ModalRegistry() = default;

  std::map<std::string, std::shared_ptr<IModal>> modals_;
};

} // namespace firmius::tui
