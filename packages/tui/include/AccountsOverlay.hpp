#ifndef FIRMIUS_TUI_ACCOUNTSOVERLAY_HPP
#define FIRMIUS_TUI_ACCOUNTSOVERLAY_HPP

#include "Overlay.hpp"
#include "daemon/Protocol.hpp"

#include <functional>
#include <string>
#include <vector>

namespace firmius::tui {

class AccountsOverlay : public Overlay {
public:
  using DismissCallback = std::function<void()>;

  AccountsOverlay() = default;

  void load(std::string providerId,
            std::vector<firmius::daemon::AccountSnapshot> accounts,
            firmius::daemon::QuotaSnapshot quotas,
            int termWidth);

  void setOnDismiss(DismissCallback cb) { onDismiss_ = std::move(cb); }

  void open() override;
  void close() override;
  bool isActive() const override { return isOpen_; }

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;

  bool handleInput(const std::string& key) override;
  bool handleMouse(const MouseEvent& event,
                   int screenRow,
                   int screenCol) override;

private:
  struct AccountEntry {
    std::string displayId;   ///< email or identifier fallback
    std::string rawId;       ///< acct.identifier (for quota lookup)
    bool rateLimited = false;
    std::string planTier;
    int backoffSecsRemaining = 0;
    std::vector<firmius::shared::QuotaBucket> quotaBuckets;
    bool expanded = false;
  };

  int visibleRowCount() const;
  int rowsForEntry(int idx) const;
  int entryAtRow(int row) const;  ///< which entry owns this display row
  void clampScroll();

  std::string renderAccountHeader(const AccountEntry& e,
                                  bool focused,
                                  int width) const;
  std::vector<std::string> renderAccountBody(const AccountEntry& e,
                                             int width) const;

  std::string providerId_;
  std::vector<AccountEntry> entries_;
  int cursorIdx_ = 0;
  int mouseArmedIdx_ = -1;
  int scrollOffset_ = 0;
  int maxVisible_ = 18;
  int termWidth_ = 80;
  DismissCallback onDismiss_;
  bool isOpen_ = false;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_ACCOUNTSOVERLAY_HPP
