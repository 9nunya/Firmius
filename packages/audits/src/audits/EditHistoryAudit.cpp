#include "audits/EditHistoryAudit.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace firmius::audits {

using firmius::shared::AuditResult;

namespace {

int runCommand(const std::filesystem::path &cwd, const std::string &command) {
  const auto previous = std::filesystem::current_path();
  std::error_code ec;
  std::filesystem::current_path(cwd, ec);
  if (ec) {
    return 127;
  }
  const int code = std::system(command.c_str());
  std::filesystem::current_path(previous, ec);
  return code;
}

} // namespace

std::string EditHistoryAudit::getId() const { return "edit_history"; }

std::string EditHistoryAudit::getDescription() const {
  return "Run stress-focused edit history and fallback diff tests covering reload and undo-sensitive edit flows.";
}

AuditResult EditHistoryAudit::run(const std::vector<std::string>&) {
  AuditResult result;
  result.auditId = getId();

  const auto cwd = std::filesystem::current_path();
  const auto testTools =
      cwd / "build" / "tests" / "unit" / "test_tools";
  const auto testHistory =
      cwd / "build" / "tests" / "unit" / "test_stream_state_manager_history";

  std::ostringstream out;
  out << "cwd=" << cwd.string() << "\n";
  out << "test_tools=" << testTools.string() << "\n";
  out << "test_stream_state_manager_history=" << testHistory.string() << "\n";

  if (!std::filesystem::exists(testTools) ||
      !std::filesystem::exists(testHistory)) {
    result.passed = false;
    result.exitCode = 1;
    out << "error=required unit test binaries are missing; build tests first\n";
    result.output = out.str();
    std::cerr << "AUDIT FAILED: edit history audit requires built unit test binaries\n";
    return result;
  }

  const std::string toolsCmd =
      "./build/tests/unit/test_tools --gtest_filter="
      "'FileEditAnchorToolTest.topLevelUnifiedMultiFilePatchSplitsPerFile:"
      "FileEditAnchorToolTest.overwriteExistingFileNoLongerRequiresFullyRead'";
  const std::string historyCmd =
      "./build/tests/unit/test_stream_state_manager_history --gtest_filter="
      "'StreamStateManagerHistoryTest.RebuildDerivesFileEditFallbackSignalsFromOperationsWhenPreviewMissing'";

  const int toolsCode = runCommand(cwd, toolsCmd);
  const int historyCode = runCommand(cwd, historyCmd);

  out << "tools_exit_code=" << toolsCode << "\n";
  out << "history_exit_code=" << historyCode << "\n";

  result.passed = (toolsCode == 0 && historyCode == 0);
  result.exitCode = result.passed ? 0 : 1;
  result.output = out.str();
  if (result.passed) {
    std::cout << "AUDIT PASSED: edit history stress checks succeeded\n";
  } else {
    std::cerr << "AUDIT FAILED: edit history stress checks failed\n";
  }
  return result;
}

} // namespace firmius::audits
