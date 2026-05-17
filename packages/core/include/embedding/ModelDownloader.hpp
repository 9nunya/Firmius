#ifndef FIRMIUS_CORE_EMBEDDING_MODEL_DOWNLOADER_HPP
#define FIRMIUS_CORE_EMBEDDING_MODEL_DOWNLOADER_HPP

#include <cstdint>
#include <functional>
#include <string>

namespace firmius::core::embedding {

struct DownloadProgress {
  uint64_t bytesDownloaded = 0;
  uint64_t totalBytes = 0;
  std::string status; // "downloading", "extracting", "ready", "error"
  std::string modelId;
  std::string error;
};

using ProgressCallback = std::function<void(const DownloadProgress &)>;

class ModelDownloader {
public:
  /// Returns ~/.firmius/models/
  static std::string defaultModelDir();

  /// Check if model files exist on disk
  static bool isModelAvailable(const std::string &modelId);

  /// Get the path to the model file
  static std::string modelPath(const std::string &modelId);

  /// Download model if not present. Reports progress via callback.
  /// Returns true if model is available (either already present or
  /// successfully downloaded).
  static bool ensureModel(const std::string &modelId,
                          ProgressCallback onProgress = nullptr);

  /// Default model ID
  static std::string defaultModelId();
};

} // namespace firmius::core::embedding

#endif
