#pragma once

#include <string>
#include <vector>

namespace firmius::audits {

struct MigrationCliOptions {
  std::string inputDbPath;
  std::string outputDbPath;
  bool applyInPlace = false;
  bool forceRemigrate = false;
  std::vector<std::string> threadIds;
};

MigrationCliOptions parseMigrationCliOptions(int argc, char **argv);
void runMigrationCli(const MigrationCliOptions &options);

} // namespace firmius::audits
