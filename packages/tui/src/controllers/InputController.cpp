#include "controllers/InputController.hpp"
#include "TUIState.hpp"
#include "models/TUIStore.hpp"
#include "harness/Harness.hpp"
#include "commands/CommandManager.hpp"
#include "utils/ReferenceAutocomplete.hpp"
#include <filesystem>
#include <system_error>

namespace firmius::tui {

InputController& InputController::instance() {
  static InputController inst;
  return inst;
}

void InputController::submit(firmius::core::Harness* harness) {
  auto& store = TUIStore::instance();
  auto& input = *store.input_model->buffer;
  
  if (input.empty()) return;

  if (input[0] == '/') {
     CommandCtx ctx{&TuiState::instance()};
     if (CommandManager::instance().executeCommand(ctx, input)) {
       input.clear();
       *store.input_model->cursor = 0;
       return;
     }
  }

  (void)harness;
  TuiState::instance().submitPrompt(input, {});

  input.clear();
  *store.input_model->cursor = 0;
}

std::vector<std::string> InputController::completeFileReferences(const std::string& query, const std::string& cwd) {
  const std::filesystem::path root = cwd.empty() ? std::filesystem::current_path() : std::filesystem::path(cwd);
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    std::lock_guard<std::mutex> lock(file_reference_cache_mutex_);
    file_reference_cache_ready_ = false;
    file_reference_cache_loading_ = false;
    file_reference_cache_paths_.clear();
    return {};
  }

  {
    std::lock_guard<std::mutex> lock(file_reference_cache_mutex_);
    if (file_reference_cache_ready_ && file_reference_cache_root_ == root) {
      return BuildFileReferenceSuggestions(file_reference_cache_paths_, query, 8);
    }

    if (file_reference_cache_loading_ && file_reference_cache_root_ == root) {
      return {};
    }

    file_reference_cache_root_ = root;
    file_reference_cache_ready_ = false;
    file_reference_cache_loading_ = true;
    file_reference_cache_paths_.clear();
  }

  TuiState::instance().runBackgroundTask([this, root]() {
    std::vector<std::string> paths;
    std::error_code ec;
    size_t visited = 0;
    for (std::filesystem::recursive_directory_iterator
             it(root, std::filesystem::directory_options::skip_permission_denied,
                ec),
         end;
         it != end; it.increment(ec)) {
      if (++visited > 20000 || paths.size() >= 1000)
        break;
      if (ec) {
        ec.clear();
        continue;
      }
      const auto name = it->path().filename().string();
      if (it->is_directory(ec)) {
        if ((!name.empty() && name[0] == '.') || name == "node_modules" ||
            name == "build" || name == "dist" || name == "__pycache__" ||
            name == "target" || name == "vendor" || name == "_build" ||
            name == "out" || name == "coverage" || name == "uploads" ||
            name == ".output" || name == "CMakeFiles") {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (!it->is_regular_file(ec))
        continue;

      const auto rel = std::filesystem::relative(it->path(), root, ec);
      if (ec) {
        ec.clear();
        continue;
      }
      paths.push_back(rel.generic_string());
    }

    TuiState::instance().deferUiMutation([this, root, paths = std::move(paths)]() {
      std::lock_guard<std::mutex> lock(file_reference_cache_mutex_);
      if (file_reference_cache_root_ != root)
        return;
      file_reference_cache_paths_ = paths;
      file_reference_cache_ready_ = true;
      file_reference_cache_loading_ = false;
    });
  });

  return {};
}

std::vector<std::string> InputController::completeArtifactReferences(const std::string& query, firmius::core::Harness* harness, const std::string& threadId) {
  if (!harness || threadId.empty()) {
    return {};
  }

  {
    std::lock_guard<std::mutex> lock(artifact_cache_mutex_);
    if (artifact_cache_ready_ && artifact_cache_thread_id_ == threadId) {
      return BuildArtifactReferenceSuggestions(artifact_cache_, query, 8);
    }
    if (artifact_cache_loading_ && artifact_cache_thread_id_ == threadId) {
      return {};
    }
    artifact_cache_thread_id_ = threadId;
    artifact_cache_ready_ = false;
    artifact_cache_loading_ = true;
    artifact_cache_.clear();
  }

  TuiState::instance().runBackgroundTask([this, threadId]() {
    std::vector<firmius::shared::ThreadArtifactMetadata> artifacts;
    try {
      artifacts = firmius::core::Harness::instance().listArtifacts(threadId);
    } catch (...) {
      artifacts.clear();
    }
    TuiState::instance().deferUiMutation(
        [this, threadId, artifacts = std::move(artifacts)]() mutable {
          std::lock_guard<std::mutex> lock(artifact_cache_mutex_);
          if (artifact_cache_thread_id_ != threadId)
            return;
          artifact_cache_ = std::move(artifacts);
          artifact_cache_ready_ = true;
          artifact_cache_loading_ = false;
        });
  });

  return {};
}

} // namespace firmius::tui
