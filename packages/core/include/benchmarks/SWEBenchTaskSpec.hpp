#ifndef FIRMIUS_CORE_SWEBENCHTASKSPEC_HPP
#define FIRMIUS_CORE_SWEBENCHTASKSPEC_HPP

#include <map>
#include <rapidjson/fwd.h>
#include <string>
#include <vector>

namespace firmius::core {

struct SWEBenchTaskSpec {
    std::string instanceId;
    std::string repo;
    std::string baseCommit;
    std::string problemStatement;
    std::string testPatch;
    std::vector<std::string> failToPass;
    std::vector<std::string> passToPass;
    std::vector<std::string> installCommands;
    std::vector<std::string> buildCommands;
    std::vector<std::string> evalCommands;
    std::map<std::string, std::string> environment;
};

std::vector<std::string> parseStringArrayField(const rapidjson::Value& value);
SWEBenchTaskSpec parseSWEBenchTaskSpec(const rapidjson::Value& row);

} // namespace firmius::core

#endif