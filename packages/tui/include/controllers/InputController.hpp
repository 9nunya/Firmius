#ifndef FIRMIUS_TUI_CONTROLLERS_INPUT_CONTROLLER_HPP
#define FIRMIUS_TUI_CONTROLLERS_INPUT_CONTROLLER_HPP

#include "models/TUIStore.hpp"
#include "Context.hpp"
#include <atomic>
#include <string>
#include <vector>
#include <filesystem>
#include <mutex>

namespace firmius::core { class Harness; }

namespace firmius::tui {

class InputController {
public:
  static InputController& instance();
  // Autocomplete support
  std::vector<std::string> completeFileReferences(const std::string& query, const std::string& cwd);
  std::vector<std::string> completeArtifactReferences(const std::string& query, firmius::core::Harness* harness, const std::string& threadId);
  bool file_reference_cache_ready_ = false;
  bool file_reference_cache_loading_ = false;
  std::filesystem::path file_reference_cache_root_;
  std::vector<std::string> file_reference_cache_paths_;
  std::mutex file_reference_cache_mutex_;
  std::string artifact_cache_thread_id_;
  bool artifact_cache_ready_ = false;
  bool artifact_cache_loading_ = false;
  std::vector<firmius::shared::ThreadArtifactMetadata> artifact_cache_;
  std::mutex artifact_cache_mutex_;
  void submit(firmius::core::Harness* harness);

private:
  InputController() = default;
};

} // namespace firmius::tui

#endif
