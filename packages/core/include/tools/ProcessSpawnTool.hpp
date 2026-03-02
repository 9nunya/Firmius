#ifndef FIRMIUS_CORE_PROCESS_SPAWN_TOOL_HPP
#define FIRMIUS_CORE_PROCESS_SPAWN_TOOL_HPP

#include "ITool.hpp"
#include <string>
#include <map>

namespace firmius::core {
using namespace firmius::shared;

struct ProcessSpawnInput {
    std::string command;
    std::string cwd;
    std::map<std::string, std::string> env;
};

class ProcessSpawnTool : public shared::TypedTool<ProcessSpawnInput> {
public:
    shared::ToolMetadata getMetadata() const override {
        return {"process_spawn", "Spawns a background process and returns a process_id.", ToolScope::Process};
    }

    std::shared_ptr<shared::JSONSchema> getSchema() const override {
        return zObject({
            {"command", zString()->describe("The command to run")},
            {"cwd", zString()->describe("Working directory for the process")->setOptional()},
            {"env", zObject({})->describe("Environment variables")->setOptional()}
        })->required({"command"});
    }

    START_MAPPING(ProcessSpawnInput)
        MAP_STRING(command, "command")
        MAP_STRING(cwd, "cwd")
        if (json.HasMember("env") && json["env"].IsObject()) {
            for (auto it = json["env"].MemberBegin(); it != json["env"].MemberEnd(); ++it) {
                if (it->value.IsString()) input.env[it->name.GetString()] = it->value.GetString();
            }
        }
    END_MAPPING

    shared::ToolResult execute(const ProcessSpawnInput& input, shared::ToolContext& ctx) override;
};

}

#endif
