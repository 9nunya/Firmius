#include "tools/SkillLoadTool.hpp"

#include "agents/Agent.hpp"
#include "agents/SkillLoader.hpp"

#include "utils/FSUtil.hpp"
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace firmius::core {

namespace {



std::optional<std::pair<std::string, std::optional<std::string>>>
parseSkillSelector(const std::string &rawWhat) {
  const std::size_t slash = rawWhat.find('/');
  if (slash == std::string::npos) {
    return std::make_pair(rawWhat, std::optional<std::string>{});
  }

  const std::string skillId = rawWhat.substr(0, slash);
  const std::string nestedPath = rawWhat.substr(slash + 1);
  return std::make_pair(skillId, std::optional<std::string>{nestedPath});
}

} // namespace

shared::ToolMetadata SkillLoadTool::getMetadata() const {
  return {"Skill",
          "Load a skill entrypoint or nested skill file from ~/.agents/skills",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> SkillLoadTool::getSchema() const {
  return shared::zObject(
             {{"what",
               shared::zString()->describe(
                   "Skill selector: '<skill_id>' or '<skill_id>/path/to/file'")}})
      ->required({"what"});
}

shared::ToolResult SkillLoadTool::execute(const SkillLoadInput &input,
                                          shared::ToolContext &ctx) {
  try {
    if (input.what.empty()) {
      return shared::ToolResult::fail("'what' must not be empty");
    }

    const auto parsed = parseSkillSelector(input.what);
    if (!parsed.has_value()) {
      return shared::ToolResult::fail("Invalid skill selector in 'what'");
    }

    const std::string &skillId = parsed->first;
    const std::optional<std::string> &nestedPathRaw = parsed->second;
    if (skillId.empty()) {
      return shared::ToolResult::fail(
          "Skill selector must start with a skill id before '/'");
    }

    const auto maybeSkill = SkillLoader::getSkill(skillId);
    if (!maybeSkill.has_value()) {
      return shared::ToolResult::fail("Skill not found: " + skillId);
    }
    const auto &skill = maybeSkill.value();

    std::filesystem::path targetPath(skill.skillMdPath);
    std::string relativePath = "SKILL.md";

    if (nestedPathRaw.has_value()) {
      if (nestedPathRaw->empty()) {
        return shared::ToolResult::fail(
            "Nested skill path after skill id cannot be empty");
      }

      std::filesystem::path nested = std::filesystem::path(*nestedPathRaw);
      if (nested.is_absolute()) {
        return shared::ToolResult::fail(
            "Nested skill path must be relative to the skill root");
      }

      nested = nested.lexically_normal();
      for (const auto &component : nested) {
        if (component == "..") {
          return shared::ToolResult::fail(
              "Nested skill path cannot traverse outside the skill root");
        }
      }

      targetPath =
          (std::filesystem::path(skill.skillRoot) / nested).lexically_normal();
      if (!shared::FSUtil::isCanonicalSubpath(targetPath, std::filesystem::path(skill.skillRoot))) {
        return shared::ToolResult::fail(
            "Resolved skill path must stay within the skill root (canonical check)");
      }
      relativePath = nested.string();
    }

    if (!std::filesystem::exists(targetPath) ||
        !std::filesystem::is_regular_file(targetPath)) {
      return shared::ToolResult::fail("Skill file not found: " +
                                      targetPath.string());
    }

    ctx.agent.getPermissions()->validatePathAccess(targetPath.string(),
                                                   shared::AccessMode::READ);

    const std::vector<uint8_t> data = ctx.host.readFile(targetPath.string());
    const std::string content(data.begin(), data.end());

    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();

    doc.AddMember("skill_id", rapidjson::Value(skillId.c_str(), a).Move(), a);
    doc.AddMember("name", rapidjson::Value(skill.name.c_str(), a).Move(), a);
    doc.AddMember("description",
                  rapidjson::Value(skill.description.c_str(), a).Move(), a);
    doc.AddMember("skill_root", rapidjson::Value(skill.skillRoot.c_str(), a).Move(),
                  a);
    const std::string absolutePath = targetPath.string();
    doc.AddMember("path", rapidjson::Value(absolutePath.c_str(), a).Move(), a);
    doc.AddMember("relative_path", rapidjson::Value(relativePath.c_str(), a).Move(),
                  a);
    doc.AddMember("content", rapidjson::Value(content.c_str(), a).Move(), a);

    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
