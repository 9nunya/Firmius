#include "tools/ArtifactWriteTool.hpp"
#include "IAgent.hpp"
#include "Serialization.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"
#include <cstdlib>
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

namespace {

std::string threadStorageRootPath() {
  if (const char *home = std::getenv("HOME")) {
    return std::string(home) + "/.firmius/threads";
  }
  return ".firmius/threads";
}

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

shared::ToolMetadata ArtifactWriteTool::getMetadata() const {
  return {"artifact_write", "Create or update a thread-scoped artifact file.",
          ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> ArtifactWriteTool::getSchema() const {
  return zObject(
             {{"name", zString()->describe("Artifact filename (e.g. REPORT.md)")},
              {"content", zString()->describe("Artifact content")},
              {"kind", zString()->setOptional()->describe("Optional artifact kind")},
              {"description", zString()->setOptional()->describe(
                                  "Optional artifact description")}})
      ->required({"name", "content"});
}

shared::ToolResult ArtifactWriteTool::execute(const ArtifactWriteInput &input,
                                              shared::ToolContext &ctx) {
  try {
    auto &agentCtx = ctx.agent.getContext();
    if (!agentCtx.history || agentCtx.history->threadId.empty()) {
      return ToolResult::fail("artifact_write requires an active thread");
    }
    if (agentCtx.identity.id.empty()) {
      return ToolResult::fail("artifact_write requires an agent identity");
    }

    ThreadManager tm(threadStorageRootPath());
    bool created = false;
    const ThreadArtifactMetadata metadata = tm.writeArtifact(
        agentCtx.history->threadId, agentCtx.identity.id,
        agentCtx.identity.friendlyName, shared::StringUtil::trim(input.name),
        input.content, &created, input.kind, input.description);

    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    doc.AddMember("status", created ? "created" : "updated", a);
    doc.AddMember("created", created, a);
    doc.AddMember("updated", !created, a);
    doc.AddMember("artifact", jsonFromArtifactMetadata(metadata, a), a);

    const std::string owner = displayOwnerName(metadata);
    const std::string reference = "@artifact:" + owner + "/" + metadata.filename;
    doc.AddMember("reference", rapidjson::Value(reference.c_str(), a), a);
    return ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
