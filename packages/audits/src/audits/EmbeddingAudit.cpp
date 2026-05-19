#include "audits/EmbeddingAudit.hpp"
#include "embedding/EmbeddingStore.hpp"
#include "embedding/EmbeddingWorker.hpp"
#include "embedding/ModelDownloader.hpp"
#include "embedding/OnnxEmbedder.hpp"
#include "utils/MathUtil.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

namespace firmius::audits {
namespace fs = std::filesystem;
using core::embedding::EmbeddingRef;
using core::embedding::EmbeddingStore;
using core::embedding::EmbeddingWorker;
using core::embedding::DownloadProgress;
using core::embedding::ModelDownloader;
using core::embedding::OnnxEmbedder;

std::string EmbeddingAudit::getId() const { return "embedding"; }

std::string EmbeddingAudit::getDescription() const {
  return "Tests embedding model download, ONNX inference, and progress reporting";
}

shared::AuditResult EmbeddingAudit::run(const std::vector<std::string> &) {
  shared::AuditResult result;
  result.auditId = getId();
  result.passed = true;

  std::string modelDir = (std::filesystem::temp_directory_path() / "firmius_audit_embedding").string();
  std::string storePath = modelDir + "/store";
  int downloadProgressUpdates = 0;
  uint64_t lastBytes = 0;

  fs::remove_all(modelDir);
  fs::create_directories(modelDir);

  auto fail = [&](const std::string &msg) {
    result.passed = false;
    result.exitCode = 1;
    result.output += "FAIL: " + msg + "\n";
    fs::remove_all(modelDir);
  };

  // ═══════════════════════════════════════════════════════════════
  // TEST 1: ModelDownloader API
  // ═══════════════════════════════════════════════════════════════
  {
    std::string defaultDir = ModelDownloader::defaultModelDir();
    std::string defaultId = ModelDownloader::defaultModelId();
    std::string path = ModelDownloader::modelPath(defaultId);

    if (defaultDir.empty() || defaultId.empty() || path.empty()) {
      fail("ModelDownloader API returned empty values");
      return result;
    }
    result.output += "PASS: ModelDownloader API (dir=" + defaultDir +
                     " id=" + defaultId + ")\n";
  }

  // ═══════════════════════════════════════════════════════════════
  // TEST 2: Model download with progress
  // ═══════════════════════════════════════════════════════════════
  {
    std::string progress;
    std::mutex mtx;

    ModelDownloader::ensureModel(ModelDownloader::defaultModelId(),
                                 [&](const DownloadProgress &p) {
                                   std::lock_guard lk(mtx);
                                   if (p.status == "downloading") {
                                     downloadProgressUpdates++;
                                     if (p.bytesDownloaded > lastBytes)
                                       lastBytes = p.bytesDownloaded;
                                   }
                                   progress = p.status;
                                 });

    if (progress != "ready") {
      fail("Model download failed: " + progress);
      return result;
    }
    if (!ModelDownloader::isModelAvailable(ModelDownloader::defaultModelId())) {
      fail("Model not available after download");
      return result;
    }
    result.output += "PASS: Model download (" +
                     std::to_string(downloadProgressUpdates) + " progress updates, " +
                     std::to_string(lastBytes) + " bytes)\n";
  }

  // ═══════════════════════════════════════════════════════════════
  // TEST 3: ONNX inference
  // ═══════════════════════════════════════════════════════════════
  {
    std::string modelPath =
        ModelDownloader::modelPath(ModelDownloader::defaultModelId());
    OnnxEmbedder embedder(modelPath);

    if (!embedder.isValid()) {
      fail("OnnxEmbedder failed to load model");
      return result;
    }
    if (embedder.dimension() == 0) {
      fail("OnnxEmbedder dimension is 0");
      return result;
    }

    auto emb = embedder.embed("Hello world");
    if (emb.empty()) {
      fail("embed() returned empty vector");
      return result;
    }
    if (emb.size() != embedder.dimension()) {
      fail("Embedding size mismatch: got " + std::to_string(emb.size()) +
           " expected " + std::to_string(embedder.dimension()));
      return result;
    }

    // Verify L2 normalization
    float norm = 0;
    for (float v : emb) norm += v * v;
    norm = std::sqrt(norm);
    if (std::abs(norm - 1.0f) > 0.01f) {
      fail("Embedding not L2 normalized: norm=" + std::to_string(norm));
      return result;
    }

    result.output += "PASS: ONNX inference (dim=" +
                     std::to_string(embedder.dimension()) +
                     " norm=" + std::to_string(norm) + ")\n";
  }

  // ═══════════════════════════════════════════════════════════════
  // TEST 4: Semantic similarity
  // ═══════════════════════════════════════════════════════════════
  {
    std::string modelPath =
        ModelDownloader::modelPath(ModelDownloader::defaultModelId());
    OnnxEmbedder embedder(modelPath);

    auto embCat = embedder.embed("The cat sat on the mat");
    auto embDog = embedder.embed("The dog lay on the rug");
    auto embCar = embedder.embed("Quantum physics is fascinating");

    double simCatDog = firmius::shared::cosineSimilarity(embCat, embDog);
    double simCatCar = firmius::shared::cosineSimilarity(embCat, embCar);

    if (simCatDog <= simCatCar) {
      fail("Semantic similarity wrong: cat-dog=" + std::to_string(simCatDog) +
           " should be > cat-car=" + std::to_string(simCatCar));
      return result;
    }

    result.output += "PASS: Semantic similarity (cat-dog=" +
                     std::to_string(simCatDog) +
                     " cat-car=" + std::to_string(simCatCar) + ")\n";
  }

  // ═══════════════════════════════════════════════════════════════
  // TEST 5: EmbeddingWorker end-to-end
  // ═══════════════════════════════════════════════════════════════
  {
    fs::remove_all(storePath);

    std::string modelPath =
        ModelDownloader::modelPath(ModelDownloader::defaultModelId());
    auto embedFn = [&modelPath](const std::string &text) -> std::vector<float> {
      static std::once_flag flag;
      static std::unique_ptr<OnnxEmbedder> emb;
      std::call_once(flag, [&]() {
        emb = std::make_unique<OnnxEmbedder>(modelPath);
      });
      return emb->embed(text);
    };

    {
      EmbeddingWorker worker(embedFn, storePath,
                             ModelDownloader::defaultModelId());
      for (uint32_t i = 0; i < 5; ++i) {
        worker.enqueue({i, 384}, "Test sentence " + std::to_string(i));
      }
    } // worker destroyed → flushes store to disk

    EmbeddingStore store(storePath, 384);
    if (store.size() == 0) {
      fail("EmbeddingStore empty after worker processing");
      return result;
    }

    result.output += "PASS: EmbeddingWorker (" +
                     std::to_string(store.size()) + " items stored)\n";
  }

  // ═══════════════════════════════════════════════════════════════
  // TEST 6: EmbeddingStore search
  // ═══════════════════════════════════════════════════════════════
  {
    EmbeddingStore store(storePath);
    std::string modelPath =
        ModelDownloader::modelPath(ModelDownloader::defaultModelId());
    OnnxEmbedder embedder(modelPath);

    auto query = embedder.embed("Test sentence 0");
    auto results = store.search(query, 3);
    if (results.empty()) {
      fail("EmbeddingStore search returned empty");
      return result;
    }
    if (results[0].second < 0.5f) {
      fail("Top search result too low: " + std::to_string(results[0].second));
      return result;
    }

    result.output += "PASS: EmbeddingStore search (top score=" +
                     std::to_string(results[0].second) + ")\n";
  }

  fs::remove_all(modelDir);
  result.output += "ALL TESTS PASSED\n";
  return result;
}

} // namespace firmius::audits
