#include "commands/NewCommand.hpp"
#include "harness/Harness.hpp"
#include <filesystem>

namespace firmius::tui {

void NewCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)ctx;
  (void)args;
  auto &h = firmius::core::Harness::instance();
  std::string cwd = std::filesystem::current_path().string();
  auto cfg = h.getConfig();
  std::string lead =
      cfg.defaultLeadPersona.empty() ? "aster" : cfg.defaultLeadPersona;
  h.newThread({}, cwd, lead);
}

} // namespace firmius::tui
