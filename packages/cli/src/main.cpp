#include "TuiRunner.hpp"
#include "WebRunner.hpp"
#include <string>

int main(int argc, char **argv) {
  bool continue_last = false;
  bool debugging_mode = false;
  bool web = false;
  std::string web_hostname = "127.0.0.1";
  int web_port = 9173;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "web" && i == 1) {
      web = true;
    } else if (arg == "-c" && !web) {
      continue_last = true;
    } else if (arg == "--i-am-debugging" && !web) {
      debugging_mode = true;
    } else if ((arg == "--host" || arg == "-h") && web) {
      web_hostname = argv[i + 1];
    }
  }

  if (!web)
    firmius::tui::runTui(debugging_mode, continue_last);
  else
    firmius::web::runWeb(web_hostname, web_port);

  return 0;
}
