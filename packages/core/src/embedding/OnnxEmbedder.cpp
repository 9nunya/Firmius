#include "embedding/OnnxEmbedder.hpp"

#ifdef FIRMIUS_HAS_ONNX
#include <onnxruntime_c_api.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace firmius::core::embedding {

#ifdef FIRMIUS_HAS_ONNX

namespace {

const OrtApi *gApi = nullptr;

const OrtApi &api() {
  if (!gApi) gApi = OrtGetApiBase()->GetApi(ORT_API_VERSION);
  return *gApi;
}

void checkStatus(OrtStatus *status) {
  if (status) {
    std::string msg = api().GetErrorMessage(status);
    api().ReleaseStatus(status);
    throw std::runtime_error("ONNX Runtime error: " + msg);
  }
}

/// Simple tokenizer for sentence-transformers: whitespace split + wordpiece-like
/// For MiniLM, we need input_ids, attention_mask, token_type_ids
/// This is a simplified tokenizer — splits on whitespace and maps to character-level IDs.
/// For production, integrate a proper tokenizer (e.g., tokenizers-cpp).
std::vector<int64_t> tokenize(const std::string &text, size_t maxLen = 128) {
  std::vector<int64_t> ids;
  ids.reserve(maxLen);
  // CLS token (101 for BERT-based models)
  ids.push_back(101);
  // Character-level tokenization as fallback
  for (char c : text) {
    if (ids.size() >= maxLen - 1) break;
    ids.push_back(static_cast<int64_t>(static_cast<unsigned char>(c)) + 1000);
  }
  // SEP token (102 for BERT-based models)
  ids.push_back(102);
  // Pad to maxLen
  while (ids.size() < maxLen) ids.push_back(0);
  return ids;
}

} // namespace

class OnnxEmbedder::Impl {
public:
  explicit Impl(const std::string &modelPath) {
    checkStatus(api().CreateEnv(ORT_LOGGING_LEVEL_WARNING, "firmius", &env_));
    checkStatus(api().CreateSessionOptions(&sessionOpts_));
    checkStatus(api().SetIntraOpNumThreads(sessionOpts_, 1));
    checkStatus(api().CreateSession(env_, modelPath.c_str(), sessionOpts_, &session_));
    checkStatus(api().CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memoryInfo_));
    checkStatus(api().GetAllocatorWithDefaultOptions(&allocator_));

    // Get input/output names — keep the allocator-allocated strings alive
    size_t numInputs;
    checkStatus(api().SessionGetInputCount(session_, &numInputs));
    inputNames_.resize(numInputs);
    for (size_t i = 0; i < numInputs; ++i) {
      char *name = nullptr;
      checkStatus(api().SessionGetInputName(session_, i, allocator_, &name));
      inputNames_[i] = name;
    }

    size_t numOutputs;
    checkStatus(api().SessionGetOutputCount(session_, &numOutputs));
    outputNames_.resize(numOutputs);
    for (size_t i = 0; i < numOutputs; ++i) {
      char *name = nullptr;
      checkStatus(api().SessionGetOutputName(session_, i, allocator_, &name));
      outputNames_[i] = name;
    }
  }

  ~Impl() {
    if (memoryInfo_) api().ReleaseMemoryInfo(memoryInfo_);
    if (session_) api().ReleaseSession(session_);
    if (sessionOpts_) api().ReleaseSessionOptions(sessionOpts_);
    if (env_) api().ReleaseEnv(env_);
  }

  std::vector<float> embed(const std::string &text) const {
    constexpr size_t maxLen = 128;
    auto inputIds = tokenize(text, maxLen);

    // Create attention_mask (1 for real tokens, 0 for padding)
    std::vector<int64_t> attentionMask(maxLen, 0);
    for (size_t i = 0; i < maxLen; ++i) {
      attentionMask[i] = (inputIds[i] != 0) ? 1 : 0;
    }

    // Create token_type_ids (all 0 for single sentence)
    std::vector<int64_t> tokenTypeIds(maxLen, 0);

    // Create tensors
    int64_t dims[2] = {1, static_cast<int64_t>(maxLen)};

    OrtValue *inputIdsTensor = nullptr;
    OrtValue *attMaskTensor = nullptr;
    OrtValue *tokenTypeTensor = nullptr;

    checkStatus(api().CreateTensorWithDataAsOrtValue(
        memoryInfo_, inputIds.data(), inputIds.size() * sizeof(int64_t),
        dims, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &inputIdsTensor));

    checkStatus(api().CreateTensorWithDataAsOrtValue(
        memoryInfo_, attentionMask.data(), attentionMask.size() * sizeof(int64_t),
        dims, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &attMaskTensor));

    checkStatus(api().CreateTensorWithDataAsOrtValue(
        memoryInfo_, tokenTypeIds.data(), tokenTypeIds.size() * sizeof(int64_t),
        dims, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &tokenTypeTensor));

    std::vector<OrtValue *> inputs = {inputIdsTensor, attMaskTensor, tokenTypeTensor};

    // Run inference
    OrtValue *outputTensor = nullptr;
    checkStatus(api().Run(session_, nullptr, inputNames_.data(), inputs.data(),
                          inputs.size(), outputNames_.data(), 1, &outputTensor));

    // Extract embedding from output
    // For sentence-transformers, output shape is [1, hidden_size] or [1, seq_len, hidden_size]
    OrtTensorTypeAndShapeInfo *typeInfo;
    checkStatus(api().GetTensorTypeAndShape(outputTensor, &typeInfo));

    size_t numDims;
    checkStatus(api().GetDimensionsCount(typeInfo, &numDims));
    std::vector<int64_t> outDims(numDims);
    checkStatus(api().GetDimensions(typeInfo, outDims.data(), numDims));

    const float *outputData;
    checkStatus(api().GetTensorMutableData(outputTensor, (void **)&outputData));

    // Calculate embedding dimension — last dim is always the hidden size
    size_t hiddenSize = outDims[numDims - 1];

    // For [1, hidden_size]: use directly
    // For [1, seq_len, hidden_size]: mean-pool over sequence
    std::vector<float> embedding(hiddenSize);
    if (numDims == 2) {
      std::memcpy(embedding.data(), outputData, hiddenSize * sizeof(float));
    } else if (numDims == 3) {
      int64_t seqLen = outDims[1];
      // Mean pooling over non-padded tokens
      int64_t realTokens = 0;
      for (int64_t t = 0; t < seqLen; ++t) {
        if (t < static_cast<int64_t>(maxLen) && attentionMask[t]) {
          for (size_t h = 0; h < hiddenSize; ++h) {
            embedding[h] += outputData[t * hiddenSize + h];
          }
          realTokens++;
        }
      }
      if (realTokens > 0) {
        for (auto &v : embedding) v /= static_cast<float>(realTokens);
      }
    }

    // L2 normalize
    float norm = 0.0f;
    for (float v : embedding) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
      for (auto &v : embedding) v /= norm;
    }

    api().ReleaseTensorTypeAndShapeInfo(typeInfo);
    api().ReleaseValue(outputTensor);
    api().ReleaseValue(inputIdsTensor);
    api().ReleaseValue(attMaskTensor);
    api().ReleaseValue(tokenTypeTensor);

    return embedding;
  }

  size_t dimension() const { return 384; } // MiniLM default
  bool isValid() const { return session_ != nullptr; }

private:
  OrtEnv *env_ = nullptr;
  OrtSessionOptions *sessionOpts_ = nullptr;
  OrtSession *session_ = nullptr;
  OrtMemoryInfo *memoryInfo_ = nullptr;
  OrtAllocator *allocator_ = nullptr;

  std::vector<const char *> inputNames_;
  std::vector<const char *> outputNames_;
  std::vector<std::string> inputNamesCstr_;
  std::vector<std::string> outputNamesCstr_;
};

#else // No ONNX Runtime

class OnnxEmbedder::Impl {
public:
  explicit Impl(const std::string &) {
    throw std::runtime_error("ONNX Runtime not available — rebuild with FIRMIUS_ENABLE_ONNX=ON");
  }
  std::vector<float> embed(const std::string &) const { return {}; }
  size_t dimension() const { return 0; }
  bool isValid() const { return false; }
};

#endif

OnnxEmbedder::OnnxEmbedder(const std::string &modelPath)
    : impl_(std::make_unique<Impl>(modelPath)) {}

OnnxEmbedder::~OnnxEmbedder() = default;
OnnxEmbedder::OnnxEmbedder(OnnxEmbedder &&) noexcept = default;
OnnxEmbedder &OnnxEmbedder::operator=(OnnxEmbedder &&) noexcept = default;

std::vector<float> OnnxEmbedder::embed(const std::string &text) const {
  return impl_->embed(text);
}

size_t OnnxEmbedder::dimension() const { return impl_->dimension(); }
bool OnnxEmbedder::isValid() const { return impl_->isValid(); }

} // namespace firmius::core::embedding
