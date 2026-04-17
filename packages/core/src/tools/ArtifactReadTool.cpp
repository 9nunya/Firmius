#include "tools/ArtifactReadTool.hpp"
#include "IAgent.hpp"
#include "Serialization.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"
#include <cstdlib>
#include <sstream>

namespace firmius::core {
using namespace firmius::shared;

namespace {

constexpr const char *kArtifactPrefix = "@artifact:";
constexpr const char *kArtifactPrefixBare = "artifact:";

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

struct ParsedArtifactReference {
  std::optional<std::string> ownerFriendlyName;
  std::string filename;
};

ParsedArtifactReference parseArtifactReference(const std::string &rawReference) {
  std::string ref = shared::StringUtil::trim(rawReference);
  if (ref.rfind(kArtifactPrefix, 0) == 0) {
    ref = ref.substr(std::char_traits<char>::length(kArtifactPrefix));
  } else if (ref.rfind(kArtifactPrefixBare, 0) == 0) {
    ref = ref.substr(std::char_traits<char>::length(kArtifactPrefixBare));
  } else if (!ref.empty() && ref.front() == '@') {
    throw std::runtime_error("Malformed artifact reference: " + rawReference);
  }

  if (ref.empty()) {
    throw std::runtime_error("Artifact reference is empty");
  }

  ParsedArtifactReference parsed;
  const std::size_t slash = ref.find('/');
  if (slash == std::string::npos) {
    parsed.filename = ref;
    return parsed;
  }

  if (slash == 0 || slash + 1 >= ref.size()) {
    throw std::runtime_error("Malformed artifact reference: " + rawReference);
  }
  parsed.ownerFriendlyName = ref.substr(0, slash);
  parsed.filename = ref.substr(slash + 1);
  return parsed;
}

} // namespace

shared::ToolMetadata ArtifactReadTool::getMetadata() const {
  return {"artifact_read", "Read artifact content from thread-scoped storage.",
          ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> ArtifactReadTool::getSchema() const {
  return zObject(
      {{"reference",
        zString()
            ->setOptional()
            ->describe("Artifact reference (e.g. @artifact:lead/REPORT.md)")},
       {"name",
        zString()
            ->setOptional()
            ->describe("Artifact filename (shorthand selector)")},
       {"owner_friendly_name",
        zString()->setOptional()->describe(
            "Optional artifact owner friendly name")},
       {"owner_agent_id",
        zString()->setOptional()->describe("Optional artifact owner agent id")}});
}

shared::ToolResult ArtifactReadTool::execute(const ArtifactReadInput &input,
                                             shared::ToolContext &ctx) {
  try {
    auto &agentCtx = ctx.agent.getContext();
    if (!agentCtx.history || agentCtx.history->threadId.empty()) {
      return ToolResult::fail("artifact_read requires an active thread");
    }
    const std::string threadId = agentCtx.history->threadId;

    ThreadManager tm(threadStorageRootPath());
    const auto artifacts = tm.listArtifacts(threadId);

    std::optional<std::string> ownerFriendly = input.owner_friendly_name;
    std::optional<std::string> ownerAgentId = input.owner_agent_id;
    std::string filename =
        input.name.has_value() ? shared::StringUtil::trim(*input.name) : "";

    if (input.reference.has_value()) {
      const auto parsed = parseArtifactReference(*input.reference);
      if (parsed.ownerFriendlyName.has_value()) {
        if (ownerFriendly.has_value() &&
            *ownerFriendly != *parsed.ownerFriendlyName) {
          return ToolResult::fail(
              "owner_friendly_name conflicts with artifact reference");
        }
        ownerFriendly = parsed.ownerFriendlyName;
      }
      if (!parsed.filename.empty()) {
        filename = parsed.filename;
      }
    }

    if (filename.empty()) {
      return ToolResult::fail(
          "artifact_read requires a reference or filename selector");
    }

    if (ownerAgentId.has_value() && ownerFriendly.has_value()) {
      auto mapped = tm.findAgentIdByFriendlyName(threadId, *ownerFriendly);
      if (!mapped.has_value() || *mapped != *ownerAgentId) {
        return ToolResult::fail(
            "owner_agent_id and owner_friendly_name refer to different agents");
      }
    } else if (!ownerAgentId.has_value() && ownerFriendly.has_value()) {
      auto mapped = tm.findAgentIdByFriendlyName(threadId, *ownerFriendly);
      if (!mapped.has_value()) {
        return ToolResult::fail("Unknown or ambiguous artifact owner friendly name: " +
                                *ownerFriendly);
      }
      ownerAgentId = *mapped;
    }

    if (!ownerAgentId.has_value()) {
      std::vector<ThreadArtifactMetadata> matches;
      for (const auto &artifact : artifacts) {
        if (artifact.filename == filename) {
          matches.push_back(artifact);
        }
      }
      if (matches.empty()) {
        return ToolResult::fail("Artifact not found: " + filename);
      }
      if (matches.size() > 1) {
        std::ostringstream error;
        error << "Artifact reference is ambiguous for '" << filename
              << "'. Use @artifact:friendly-name/" << filename;
        return ToolResult::fail(error.str());
      }
      ownerAgentId = matches.front().ownerAgentId;
    }

    const std::string content = tm.readArtifact(threadId, *ownerAgentId, filename);

    ThreadArtifactMetadata metadata;
    bool foundMetadata = false;
    for (const auto &artifact : artifacts) {
      if (artifact.ownerAgentId == *ownerAgentId && artifact.filename == filename) {
        metadata = artifact;
        foundMetadata = true;
        break;
      }
    }
    if (!foundMetadata) {
      metadata.threadId = threadId;
      metadata.ownerAgentId = *ownerAgentId;
      metadata.ownerFriendlyName =
          ownerFriendly.has_value() ? *ownerFriendly : "";
      metadata.filename = filename;
      metadata.storagePath =
          "artifacts/" + *ownerAgentId + "/" + filename;
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    doc.AddMember("content", rapidjson::Value(content.c_str(), a), a);
    doc.AddMember("artifact", jsonFromArtifactMetadata(metadata, a), a);
    const std::string reference = "@artifact:" + displayOwnerName(metadata) +
                                  "/" + metadata.filename;
    doc.AddMember("reference", rapidjson::Value(reference.c_str(), a), a);
    return ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
