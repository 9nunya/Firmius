#include "benchmarks/SWEBenchTaskSpec.hpp"

#include <rapidjson/document.h>

namespace firmius::core {
namespace {

std::string getStringField(const rapidjson::Value& value, const char* key) {
    if (!value.HasMember(key) || !value[key].IsString()) {
        return "";
    }
    return value[key].GetString();
}

void appendStringArray(const rapidjson::Value& value, std::vector<std::string>& out) {
    if (!value.IsArray()) {
        return;
    }
    for (const auto& item : value.GetArray()) {
        if (item.IsString()) {
            out.push_back(item.GetString());
        }
    }
}

void appendCommands(const rapidjson::Value& value, std::vector<std::string>& out) {
    if (value.IsString()) {
        out.push_back(value.GetString());
        return;
    }
    appendStringArray(value, out);
}

void parseEnvironmentEntries(const rapidjson::Value& value,
                             std::map<std::string, std::string>& environment) {
    if (!value.IsObject()) {
        return;
    }
    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        if (it->value.IsString()) {
            environment[it->name.GetString()] = it->value.GetString();
        }
    }
}

void parseEnvironmentConfig(const rapidjson::Value& config, SWEBenchTaskSpec& spec) {
    if (!config.IsObject()) {
        return;
    }

    if (config.HasMember("env")) {
        parseEnvironmentEntries(config["env"], spec.environment);
    }
    if (config.HasMember("environment")) {
        parseEnvironmentEntries(config["environment"], spec.environment);
    }
    if (config.HasMember("setup")) {
        appendCommands(config["setup"], spec.installCommands);
    }
    if (config.HasMember("install")) {
        appendCommands(config["install"], spec.installCommands);
    }
    if (config.HasMember("build")) {
        appendCommands(config["build"], spec.buildCommands);
    }
    if (config.HasMember("test")) {
        appendCommands(config["test"], spec.evalCommands);
    }
    if (config.HasMember("eval")) {
        appendCommands(config["eval"], spec.evalCommands);
    }
    if (config.HasMember("evaluation")) {
        appendCommands(config["evaluation"], spec.evalCommands);
    }
}

} // namespace

std::vector<std::string> parseStringArrayField(const rapidjson::Value& value) {
    std::vector<std::string> result;
    if (value.IsString()) {
        rapidjson::Document document;
        document.Parse(value.GetString());
        if (document.IsArray()) {
            appendStringArray(document, result);
            return result;
        }
        if (!document.HasParseError()) {
            return result;
        }
        result.push_back(value.GetString());
        return result;
    }

    appendStringArray(value, result);
    return result;
}

SWEBenchTaskSpec parseSWEBenchTaskSpec(const rapidjson::Value& row) {
    SWEBenchTaskSpec spec;
    spec.instanceId = getStringField(row, "instance_id");
    spec.repo = getStringField(row, "repo");
    spec.baseCommit = getStringField(row, "base_commit");
    spec.problemStatement = getStringField(row, "problem_statement");
    spec.testPatch = getStringField(row, "test_patch");

    if (row.HasMember("FAIL_TO_PASS")) {
        spec.failToPass = parseStringArrayField(row["FAIL_TO_PASS"]);
    }
    if (row.HasMember("PASS_TO_PASS")) {
        spec.passToPass = parseStringArrayField(row["PASS_TO_PASS"]);
    }

    if (row.HasMember("environment_config")) {
        const auto& configValue = row["environment_config"];
        if (configValue.IsString()) {
            rapidjson::Document configDocument;
            configDocument.Parse(configValue.GetString());
            if (configDocument.IsObject()) {
                parseEnvironmentConfig(configDocument, spec);
            }
        } else if (configValue.IsObject()) {
            parseEnvironmentConfig(configValue, spec);
        }
    }

    return spec;
}

} // namespace firmius::core
