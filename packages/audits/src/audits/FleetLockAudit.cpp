#include "audits/FleetLockAudit.hpp"
#include "Enums.hpp"
#include "IAudit.hpp"
#include "harness/Harness.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

std::string FleetLockAudit::getId() const { return "fleet_lock"; }
std::string FleetLockAudit::getDescription() const {
  return "Tests if the lock system works!!";
}

shared::AuditResult FleetLockAudit::run(const std::vector<std::string> &args) {
  auto &h = core::Harness::instance();
  h.init();

  shared::HostCreationOptions hco;
  hco.type = shared::HostType::Local;

  auto t = h.newThread(hco, "/tmp");
  h.switchModel("antigravity", "gemini-3.1-pro");

  h.send("Create 3 files: ");
}

} // namespace firmius::audits
