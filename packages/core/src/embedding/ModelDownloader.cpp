#include "embedding/ModelDownloader.hpp"

#include "utils/PlatformPaths.hpp"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>

namespace firmius::core::embedding {

namespace fs = std::filesystem;

namespace {

const std::string kDefaultModelId = "all-MiniLM-L6-v2";
const std::string kDownloadUrl =
    "https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/"
    "resolve/main/onnx/model.onnx";

fs::path homeDir() {
  return firmius::shared::PlatformPaths::firmiusHomeDir();
}

struct ProgressContext {
  ProgressCallback callback;
  std::string modelId;
  uint64_t totalBytes = 0;
};

size_t writeToFile(void *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *ofs = static_cast<std::ofstream *>(userdata);
  ofs->write(static_cast<const char *>(ptr), static_cast<std::streamsize>(size * nmemb));
  return size * nmemb;
}

int progressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                     curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  auto *ctx = static_cast<ProgressContext *>(clientp);
  if (ctx->callback && dltotal > 0) {
    ctx->totalBytes = static_cast<uint64_t>(dltotal);
    DownloadProgress p;
    p.modelId = ctx->modelId;
    p.status = "downloading";
    p.bytesDownloaded = static_cast<uint64_t>(dlnow);
    p.totalBytes = static_cast<uint64_t>(dltotal);
    ctx->callback(p);
  }
  return 0; // non-zero cancels the transfer
}

} // namespace

std::string ModelDownloader::defaultModelDir() {
  return (homeDir() / "models").string();
}

bool ModelDownloader::isModelAvailable(const std::string &modelId) {
  return fs::exists(modelPath(modelId));
}

std::string ModelDownloader::modelPath(const std::string &modelId) {
  return (fs::path(defaultModelDir()) / modelId / "model.onnx").string();
}

std::string ModelDownloader::defaultModelId() { return kDefaultModelId; }

bool ModelDownloader::ensureModel(const std::string &modelId,
                                  ProgressCallback onProgress) {
  if (isModelAvailable(modelId)) {
    if (onProgress) {
      DownloadProgress p;
      p.modelId = modelId;
      p.status = "ready";
      p.bytesDownloaded = fs::file_size(modelPath(modelId));
      p.totalBytes = p.bytesDownloaded;
      onProgress(p);
    }
    return true;
  }

  fs::path dest = fs::path(modelPath(modelId));
  fs::create_directories(dest.parent_path());
  std::string tmpPath = dest.string() + ".part";

  ProgressContext ctx{onProgress, modelId, 0};

  // Notify download starting
  if (onProgress) {
    DownloadProgress p;
    p.modelId = modelId;
    p.status = "downloading";
    onProgress(p);
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    if (onProgress) {
      DownloadProgress p;
      p.modelId = modelId;
      p.status = "error";
      p.error = "Failed to initialize curl";
      onProgress(p);
    }
    return false;
  }

  std::ofstream ofs(tmpPath, std::ios::binary);
  if (!ofs) {
    curl_easy_cleanup(curl);
    if (onProgress) {
      DownloadProgress p;
      p.modelId = modelId;
      p.status = "error";
      p.error = "Failed to open output file";
      onProgress(p);
    }
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, kDownloadUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ofs);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  CURLcode res = curl_easy_perform(curl);
  ofs.close();
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    fs::remove(tmpPath);
    if (onProgress) {
      DownloadProgress p;
      p.modelId = modelId;
      p.status = "error";
      p.error = curl_easy_strerror(res);
      onProgress(p);
    }
    return false;
  }

  if (!fs::exists(tmpPath) || fs::file_size(tmpPath) == 0) {
    fs::remove(tmpPath);
    if (onProgress) {
      DownloadProgress p;
      p.modelId = modelId;
      p.status = "error";
      p.error = "Downloaded file is empty";
      onProgress(p);
    }
    return false;
  }

  fs::rename(tmpPath, dest);

  if (onProgress) {
    DownloadProgress p;
    p.modelId = modelId;
    p.status = "ready";
    p.bytesDownloaded = fs::file_size(dest);
    p.totalBytes = p.bytesDownloaded;
    onProgress(p);
  }
  return true;
}

} // namespace firmius::core::embedding
