#ifndef FIRMIUS_TUI_WORKSPACE_MODEL_HPP
#define FIRMIUS_TUI_WORKSPACE_MODEL_HPP

#include <filesystem>
#include <vector>
#include <string>
#include <functional>

namespace firmius::tui {

class WorkspaceModel {
public:
  static WorkspaceModel& instance();

  bool file_reference_cache_ready = false;
  std::filesystem::path file_reference_cache_root;
  std::vector<std::string> file_reference_cache_paths;

  std::function<void()> on_cache_updated;

private:
  WorkspaceModel() = default;
};

} // namespace firmius::tui

#endif
