#include "MigrationCli.hpp"

#include <exception>
#include <iostream>

int main(int argc, char **argv) {
  try {
    const auto options = firmius::audits::parseMigrationCliOptions(argc, argv);
    firmius::audits::runMigrationCli(options);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
