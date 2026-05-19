#ifndef FIRMIUS_CORE_ONNXEMBEDDER_HPP
#define FIRMIUS_CORE_ONNXEMBEDDER_HPP

#include <memory>
#include <string>
#include <vector>

namespace firmius::core::embedding {

/// ONNX Runtime-based embedding inference for sentence-transformers models.
/// Loads an ONNX model and runs inference to produce dense vector embeddings.
class OnnxEmbedder {
public:
  /// Construct with path to ONNX model file.
  /// Throws std::runtime_error if model fails to load.
  explicit OnnxEmbedder(const std::string &modelPath);
  ~OnnxEmbedder();

  OnnxEmbedder(OnnxEmbedder &&) noexcept;
  OnnxEmbedder &operator=(OnnxEmbedder &&) noexcept;

  /// Produce an embedding vector for the given text.
  /// Returns empty vector on failure.
  std::vector<float> embed(const std::string &text) const;

  /// Embedding dimension (determined from model output).
  size_t dimension() const;

  /// Check if the model loaded successfully.
  bool isValid() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace firmius::core::embedding

#endif
