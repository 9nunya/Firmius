#include "tools/ArtifactsTool.hpp"

#include "IAgent.hpp"
#include "Serialization.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/SpillIfLarge.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace firmius::core {
using namespace firmius::shared;

namespace {

constexpr const char *kArtifactPrefix = "@artifact:";
constexpr const char *kArtifactPrefixBare = "artifact:";

std::string threadStorageRootPath() { return ThreadManager::defaultBasePath(); }

// Token-waste pass 5: jsonFromArtifactMetadata removed. The full
// ThreadArtifactMetadata struct is no longer emitted in any Artifacts
// result — Read/Write/List all rely on the @artifact:<owner>/<name>
// reference string instead.

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

std::string stringField(const rapidjson::Value &input, const char *key) {
  if (input.IsObject() && input.HasMember(key) && input[key].IsString()) {
    return input[key].GetString();
  }
  return "";
}

} // namespace

shared::ToolMetadata ArtifactsTool::getMetadata() const {
  return {"Artifacts",
          R"(Artifact operations for writing, reading, and listing durable thread artifacts.

USAGE GUIDANCE:
- Use Write to persist generated outputs that should be referenced later (notes, reports, snapshots, generated docs, etc.).
- Use Read to retrieve a specific artifact by reference or selector.
- Use List to discover available artifacts for the current thread.
- Artifact references are stable handles; prefer them over pasting huge blobs repeatedly into chat.

ACTIONS:
- Write: create/update an artifact.
- Read: retrieve artifact content and metadata.
- List: enumerate artifacts in the current thread context.
)",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> ArtifactsTool::getSchema() const {
  return shared::zObject({
      {"action", shared::zEnum({"Write", "Read", "List"})
                     ->describe(
                         "Artifact operation to execute.\n\n"
                         "- Write: create or update an artifact\n"
                         "- Read: fetch one artifact by selector/reference\n"
                         "- List: enumerate artifacts")},
      {"name", shared::zString()->setOptional()->describe(
          "Artifact filename / logical name. Used by Write, and can also help select an artifact for Read.")},
      {"reference", shared::zString()->setOptional()->describe(
          "Artifact reference handle, typically in @artifact:owner/name form. Preferred selector for Read.")},
      {"content", shared::zString()->setOptional()->describe(
          "Artifact body for Write. Required for Write.")},
      {"kind", shared::zString()->setOptional()->describe(
          "Optional artifact kind/category for Write (for example report, note, transcript, snapshot).")},
      {"description", shared::zString()->setOptional()->describe(
          "Optional human-readable description for Write.")},
      {"owner_friendly_name", shared::zString()->setOptional()->describe(
          "Optional owner selector for Read when resolving an artifact by friendly name.")},
      {"owner_agent_id", shared::zString()->setOptional()->describe(
          "Optional owner selector for Read when resolving an artifact by exact agent id.")},
  });
}

shared::ToolResult ArtifactsTool::execute(const rapidjson::Value &input,
                                          shared::ToolContext &ctx) {
  try {
    if (!input.IsObject() || !input.HasMember("action") ||
        !input["action"].IsString()) {
      return ToolResult::fail(
          "Artifacts.action must be one of Write, Read, or List");
    }

    const std::string action = input["action"].GetString();
    auto &agentCtx = ctx.agent.getContext();
    if (!agentCtx.history || agentCtx.history->threadId.empty()) {
      return ToolResult::fail("Artifacts requires an active thread");
    }
    const std::string threadId = agentCtx.history->threadId;
    ThreadManager tm(threadStorageRootPath());

    if (action == "Write") {
      if (agentCtx.identity.id.empty()) {
        return ToolResult::fail("Artifacts.Write requires an agent identity");
      }
      const std::string name = shared::StringUtil::trim(stringField(input, "name"));
      const std::string content = stringField(input, "content");
      if (name.empty()) {
        return ToolResult::fail("Artifacts.Write requires name");
      }
      if (!input.HasMember("content") || !input["content"].IsString()) {
        return ToolResult::fail("Artifacts.Write requires content");
      }

      // Token-waste pass 5: dropped the previous-content read and echo.
      // The agent that just overwrote the artifact does not need a free
      // copy of the old content sent back; if it actually wanted the
      // prior version it would have called Read first.

      std::optional<std::string> kind;
      std::optional<std::string> description;
      if (input.HasMember("kind") && input["kind"].IsString()) {
        kind = std::string(input["kind"].GetString());
      }
      if (input.HasMember("description") && input["description"].IsString()) {
        description = std::string(input["description"].GetString());
      }

      bool created = false;
      const ThreadArtifactMetadata metadata = tm.writeArtifact(
          threadId, agentCtx.identity.id, agentCtx.identity.friendlyName, name,
          content, &created, kind, description);

      // Token-waste pass 5: prose-first {result, reference}. Dropped
      // status/created/updated/artifact (status string was just verb form
      // of the boolean pair, and the artifact metadata block was the
      // entire ThreadArtifactMetadata struct re-emitted).
      rapidjson::Document doc;
      doc.SetObject();
      auto &a = doc.GetAllocator();
      const std::string owner = displayOwnerName(metadata);
      const std::string reference =
          "@artifact:" + owner + "/" + metadata.filename;
      std::ostringstream prose;
      prose << (created ? "Created " : "Updated ") << reference << " ("
            << content.size() << " B).";
      const std::string proseStr = prose.str();
      doc.AddMember(
          "result",
          rapidjson::Value(proseStr.c_str(),
                           static_cast<rapidjson::SizeType>(proseStr.size()),
                           a).Move(),
          a);
      doc.AddMember("reference", rapidjson::Value(reference.c_str(), a), a);
      return ToolResult::ok(doc);
    }

    if (action == "Read") {
      std::optional<std::string> ownerFriendly;
      std::optional<std::string> ownerAgentId;
      std::string filename = shared::StringUtil::trim(stringField(input, "name"));
      const std::string reference = stringField(input, "reference");
      if (!reference.empty()) {
        const auto parsed = parseArtifactReference(reference);
        if (parsed.ownerFriendlyName.has_value()) {
          ownerFriendly = parsed.ownerFriendlyName;
        }
        if (!parsed.filename.empty()) {
          filename = parsed.filename;
        }
      }
      if (input.HasMember("owner_friendly_name") && input["owner_friendly_name"].IsString()) {
        ownerFriendly = std::string(input["owner_friendly_name"].GetString());
      }
      if (input.HasMember("owner_agent_id") && input["owner_agent_id"].IsString()) {
        ownerAgentId = std::string(input["owner_agent_id"].GetString());
      }
      if (filename.empty()) {
        return ToolResult::fail("Artifacts.Read requires a reference or filename selector");
      }

      const auto artifacts = tm.listArtifacts(threadId);
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
        metadata.ownerFriendlyName = ownerFriendly.has_value() ? *ownerFriendly : "";
        metadata.filename = filename;
        metadata.storagePath = "artifacts/" + *ownerAgentId + "/" + filename;
      }

      // Token-waste pass 4: large artifacts spill to /tmp instead of being
      // returned in full. The model can grep/read the spill file when it
      // actually needs the body. Threshold 64 KB matches Process/Python.
      constexpr std::size_t kArtifactSpillThreshold = 64 * 1024;
      constexpr std::size_t kArtifactTailBytes = 4 * 1024;
      auto spill = shared::utils::spillIfLarge(
          content, kArtifactSpillThreshold,
          "firmius_artifact_" + metadata.filename, kArtifactTailBytes);

      rapidjson::Document doc;
      doc.SetObject();
      auto &a = doc.GetAllocator();
      const std::string resolved =
          "@artifact:" + displayOwnerName(metadata) + "/" + metadata.filename;
      if (spill.spilled) {
        std::ostringstream prose;
        prose << "Read " << resolved << " (" << spill.totalBytes << " bytes / "
              << spill.totalLines << " lines); content spilled to "
              << spill.refPath << " (showing last "
              << spill.tail.size() << " B).";
        const std::string proseStr = prose.str();
        doc.AddMember(
            "result",
            rapidjson::Value(proseStr.c_str(),
                             static_cast<rapidjson::SizeType>(proseStr.size()),
                             a).Move(),
            a);
        doc.AddMember(
            "tail",
            rapidjson::Value(spill.tail.c_str(),
                             static_cast<rapidjson::SizeType>(spill.tail.size()),
                             a).Move(),
            a);
        doc.AddMember("ref",
                      rapidjson::Value(spill.refPath.c_str(), a).Move(), a);
        doc.AddMember("size",
                      static_cast<uint64_t>(spill.totalBytes), a);
      } else {
        doc.AddMember("content", rapidjson::Value(content.c_str(), a), a);
      }
      doc.AddMember("reference", rapidjson::Value(resolved.c_str(), a), a);
      return ToolResult::ok(doc);
    }

    if (action == "List") {
      const auto artifacts = tm.listArtifacts(threadId);
      std::unordered_map<std::string, int> filenameCounts;
      for (const auto &artifact : artifacts) {
        filenameCounts[artifact.filename]++;
      }
      // Token-waste pass 3: prose-first artifacts list. Each entry was
      // previously the full ThreadArtifactMetadata struct + display +
      // reference + ambiguous flag — six-plus fields per artifact. The
      // model effectively only needs the reference string to fetch the
      // artifact later; the prose enumerates them and the parenthetical
      // shows the kind when set.
      rapidjson::Document doc;
      doc.SetObject();
      auto &a = doc.GetAllocator();
      std::ostringstream prose;
      if (artifacts.empty()) {
        prose << "No artifacts in this thread.";
      } else {
        prose << artifacts.size() << " artifact"
              << (artifacts.size() == 1 ? "" : "s") << " in this thread:\n";
        for (const auto &artifact : artifacts) {
          const std::string qualifiedDisplay =
              displayOwnerName(artifact) + "/" + artifact.filename;
          const bool ambiguous = filenameCounts[artifact.filename] > 1;
          const std::string display =
              ambiguous ? qualifiedDisplay : artifact.filename;
          prose << "  @artifact:" << display;
          if (artifact.kind.has_value() && !artifact.kind->empty()) {
            prose << " (" << *artifact.kind << ")";
          }
          prose << "\n";
        }
      }
      const std::string proseStr = prose.str();
      doc.AddMember(
          "result",
          rapidjson::Value(proseStr.c_str(),
                           static_cast<rapidjson::SizeType>(proseStr.size()),
                           a).Move(),
          a);
      doc.AddMember("count", static_cast<uint32_t>(artifacts.size()), a);
      return ToolResult::ok(doc);
    }

    return ToolResult::fail("Artifacts.action must be Write, Read, or List");
  } catch (const std::exception &e) {
    return ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
