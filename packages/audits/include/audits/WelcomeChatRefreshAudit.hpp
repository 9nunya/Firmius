#ifndef FIRMIUS_AUDITS_WELCOME_CHAT_REFRESH_AUDIT_HPP
#define FIRMIUS_AUDITS_WELCOME_CHAT_REFRESH_AUDIT_HPP

#include "IAudit.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

// Regression repro for the welcome->chat hydration bug where, after the user
// sends from the welcome screen, the chat surface does not render the just-
// committed user turn and does not refresh on agent streaming until a manual
// ThreadChanged (e.g. Ctrl+N) re-binds everything.
//
// The audit drives the real TuiState::root() component through the full
// no-thread init path that TuiRunner uses on a fresh launch, then exercises
// the post-send code path (applyThreadOpened) and asserts that rendered
// frames actually contain (a) the persisted user turn and (b) the streaming
// delta. Failing frames are emitted verbatim so regressions are obvious.
class WelcomeChatRefreshAudit final : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif
