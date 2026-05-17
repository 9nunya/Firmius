#include "embedding/ModelDownloader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using firmius::core::embedding::ModelDownloader;
using firmius::core::embedding::DownloadProgress;

TEST(ModelDownloader, DefaultModelDirIsNonEmpty) {
  auto dir = ModelDownloader::defaultModelDir();
  EXPECT_FALSE(dir.empty());
  EXPECT_NE(dir.find(".firmius"), std::string::npos);
}

TEST(ModelDownloader, DefaultModelIdIsNonEmpty) {
  auto id = ModelDownloader::defaultModelId();
  EXPECT_FALSE(id.empty());
  EXPECT_EQ(id, "all-MiniLM-L6-v2");
}

TEST(ModelDownloader, ModelPathContainsModelId) {
  auto path = ModelDownloader::modelPath("test-model");
  EXPECT_NE(path.find("test-model"), std::string::npos);
  EXPECT_NE(path.find("model.onnx"), std::string::npos);
}

TEST(ModelDownloader, IsModelAvailableReturnsFalseForMissing) {
  EXPECT_FALSE(ModelDownloader::isModelAvailable("nonexistent-model-xyz"));
}

TEST(ModelDownloader, EnsureModelReportsErrorForBadUrl) {
  // This test verifies the error path works — it will actually try to
  // download and fail (since we're testing with a real model that may
  // or may not be reachable). We just verify it doesn't crash.
  bool reportedError = false;
  bool reportedDownloading = false;

  auto result = ModelDownloader::ensureModel(
      "nonexistent-model-xyz",
      [&](const DownloadProgress &p) {
        if (p.status == "downloading") reportedDownloading = true;
        if (p.status == "error") reportedError = true;
      });

  EXPECT_FALSE(result);
  EXPECT_TRUE(reportedDownloading || reportedError);
}
