#include "tools/ArtifactListTool.hpp"
#include "IAgent.hpp"
#include "Serialization.hpp"
#include "persistence/ThreadManager.hpp"
#include <cstdlib>
#include <unordered_map>

namespace firmius::core {
using namespace firmius::shared;

namespace {

std::string threadStorageRootPath() { return ThreadManager::defaultBasePath(); }

rapidjson::Value jsonFromArtifactMetadata(const ThreadArtifactMetadata &metadata,
                                          rapidjson::Document::AllocatorType &a) {
  rapidjson::Document doc = toJson(metadata);
  rapidjson::Value value;
  value.CopyFrom(doc, a);
  return value;
}

std::string displayOwnerName(const ThreadArtifactMetadata &metadata) {
  if (!metadata.ownerFriendlyName.empty()) {
    return metadata.ownerFriendlyName;
  }
  return metadata.ownerAgentId;
}

} // namespace

shared::ToolMetadata ArtifactListTool::getMetadata() const {
  return {"artifact_list", "List thread-scoped artifacts available for handoff.",
          ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> ArtifactListTool::getSchema() const {
  return zObject({});
}

shared::ToolResult ArtifactListTool::execute(const rapidjson::Value &,
                                             shared::ToolContext &ctx) {
  try {
    auto &agentCtx = ctx.agent.getContext();
    if (!agentCtx.history || agentCtx.history->threadId.empty()) {
      return ToolResult::fail("artifact_list requires an active thread");
    }
    const std::string threadId = agentCtx.history->threadId;

    ThreadManager tm(threadStorageRootPath());
    const auto artifacts = tm.listArtifacts(threadId);

    std::unordered_map<std::string, int> filenameCounts;
    for (const auto &artifact : artifacts) {
      filenameCounts[artifact.filename]++;
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    rapidjson::Value items(rapidjson::kArrayType);
    for (const auto &artifact : artifacts) {
      rapidjson::Value entry(rapidjson::kObjectType);
      entry.AddMember("artifact", jsonFromArtifactMetadata(artifact, a), a);

      const std::string qualifiedDisplay =
          displayOwnerName(artifact) + "/" + artifact.filename;
      const bool ambiguous = filenameCounts[artifact.filename] > 1;
      const std::string display = ambiguous ? qualifiedDisplay : artifact.filename;
      const std::string reference = "@artifact:" + display;
      entry.AddMember("display", rapidjson::Value(display.c_str(), a), a);
      entry.AddMember("reference", rapidjson::Value(reference.c_str(), a), a);
      entry.AddMember("ambiguous_filename", ambiguous, a);
      items.PushBack(entry, a);
    }
    doc.AddMember("artifacts", items, a);
    return ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
