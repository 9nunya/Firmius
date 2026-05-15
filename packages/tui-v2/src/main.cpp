#include "App.hpp"

#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
  firmius::tui2::AppOptions options;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--thread") == 0 && i + 1 < argc) {
      options.threadId = argv[++i];
    } else if (std::strcmp(argv[i], "--persona") == 0 && i + 1 < argc) {
      options.persona = argv[++i];
    } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      options.mode = argv[++i];
    } else if (std::strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
      options.cwd = argv[++i];
    } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      std::cout << "Usage: firmius_tui_v2 [options]\n"
                << "  --thread <id>    Resume a specific thread\n"
                << "  --persona <name> Lead persona (default: lead)\n"
                << "  --mode <mode>    Initial mode (e.g., forge)\n"
                << "  --cwd <path>     Working directory\n"
                << "  -h, --help       Show this help\n";
      return 0;
    }
  }

  firmius::tui2::App app(std::move(options));
  return app.run();
}
