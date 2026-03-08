#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace firmius {

struct WizardPrompt {
  std::string message;
  bool isSecret = false;
};

class OAuthWizard {
public:
  virtual ~OAuthWizard() = default;

  // Returns the next question/instruction to show the user, or std::nullopt if
  // finished compiling inputs
  virtual std::optional<WizardPrompt> nextPrompt() = 0;

  // Submit the user's answer to the current prompt
  virtual void submitAnswer(const std::string &answer) = 0;

  // Whether the interactive question sequence is complete
  virtual bool isComplete() const = 0;

  // Executes the final network exchange (e.g. exchanging an auth code for
  // tokens)
  virtual bool finalizeExchange(std::string &outErrorMessage) = 0;

  // Returns the final success message to print to the chat
  virtual std::string getFinalMessage() const = 0;
};

} // namespace firmius
