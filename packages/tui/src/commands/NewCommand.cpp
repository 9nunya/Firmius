#include "commands/NewCommand.hpp"
#include "harness/Harness.hpp"
#include <filesystem>

namespace firmius::tui {

void NewCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)ctx;
  (void)args;
  auto &h = firmius::core::Harness::instance();
  std::string cwd = std::filesystem::current_path().string();
  h.newThread(firmius::shared::HostType::Docker, cwd, "firmius");
}

} // namespace firmius::tui
