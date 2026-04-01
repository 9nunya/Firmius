#include "providers/NvidiaProvider.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using firmius::provider::NvidiaProvider;
using firmius::shared::ModelInfo;

namespace {

class TestNvidiaProvider : public NvidiaProvider {
public:
  TestNvidiaProvider() : NvidiaProvider("") {}

protected:
  std::string fetchUrl(
      const std::string &url,
      const std::map<std::string, std::string> &) const override {
    if (url == "https://integrate.api.nvidia.com/v1/models") {
      return R"json({
        "object":"list",
        "data":[
          {"id":"stepfun-ai/step-3.5-flash","object":"model","owned_by":"stepfun-ai"},
          {"id":"nvidia/nv-embed-v1","object":"model","owned_by":"nvidia"}
        ]
      })json";
    }

    if (url == "https://build.nvidia.com/stepfun-ai/step-3.5-flash/modelcard") {
      return R"html(
<!DOCTYPE html>
<html>
  <head>
    <title>step-3.5-flash Model by Stepfun-ai | NVIDIA NIM</title>
    <meta name="description" content="200B open-source reasoning engine with sparse MoE powering frontier agentic AI."/>
  </head>
  <body>
    <h1>Step 3.5 Flash</h1>
    <p><strong>Use Case:</strong> Developers and enterprises seeking a high-performance open-weight LLM for coding assistants, deep research agents, GUI automation, and complex multi-step reasoning tasks. The model is optimized for DGX Spark deployment with fast inference speeds and is particularly strong at tool-calling and agentic applications.</p>
    <p><strong>Input Types:</strong> Text <br/>
    <strong>Other Input Properties:</strong> Supports multi-turn conversations and tool-calling formats. <br/>
    <strong>Input Context Length (ISL):</strong> 256,000</p>
    <p><strong>Output Types:</strong> Text <br/>
    <strong>Other Output Properties:</strong> Generates coherent responses for coding, reasoning, and general text generation tasks.</p>
  </body>
</html>
)html";
    }

    if (url == "https://build.nvidia.com/nvidia/nv-embed-v1/modelcard") {
      return R"html(
<!DOCTYPE html>
<html>
  <body>
    <p><strong>Input Types:</strong> Text</p>
    <p><strong>Output Types:</strong> Embeddings</p>
  </body>
</html>
)html";
    }

    return "";
  }
};

} // namespace

TEST(NvidiaProviderTest, ListModelsFetchesDetailedMetadataFromModelcards) {
  TestNvidiaProvider provider;

  std::vector<ModelInfo> incrementalModels;
  provider.discoverModels(
      [&incrementalModels](const ModelInfo &model) { incrementalModels.push_back(model); });

  ASSERT_EQ(incrementalModels.size(), 1u);
  EXPECT_EQ(incrementalModels[0].id, "stepfun-ai/step-3.5-flash");
  EXPECT_EQ(incrementalModels[0].provider, "nvidia");
  EXPECT_EQ(incrementalModels[0].contextWindow, 256000u);
  EXPECT_EQ(incrementalModels[0].maxOutputTokens, 0u);
  EXPECT_EQ(incrementalModels[0].modalities,
            std::vector<std::string>({"text"}));
  EXPECT_TRUE(incrementalModels[0].supportsReasoning);

  const auto models = provider.listModels();
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0], incrementalModels[0]);
}
