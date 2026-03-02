#ifndef FIRMIUS_CORE_AGENT_BENCH_HPP
#define FIRMIUS_CORE_AGENT_BENCH_HPP

#include "IBenchmark.hpp"
#include "agents/Agent.hpp"
#include <rapidjson/document.h>
#include <unordered_set>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Benchmark runner for AgentBench (OS interaction tasks).
 */
class AgentBench : public shared::IBenchmark {
public:
    /**
     * @brief Constructs an AgentBench runner.
     * @param agent The agent instance to evaluate.
     * @param host The host environment to use for execution.
     */
    AgentBench(Agent& agent, shared::IHost& host);
    
    std::vector<std::string> listTasks() override;
    bool prepareTask(const std::string& taskId) override;
    BenchmarkResult runTask(const std::string& taskId) override;

private:
    /**
     * @brief Downloads and caches the AgentBench dataset if not present.
     */
    void ensureDatasetLoaded();

    void ensureScriptsFetched();
    void collectScriptPaths(const rapidjson::Value& scriptObj, const std::string& scriptDir, std::unordered_set<std::string>& paths);
    std::string fetchScriptFromGitHub(const std::string& scriptPath, const std::string& localPath);
    std::tuple<std::string, std::string> loadScriptObj(const rapidjson::Value& scriptObj, const std::string& scriptDir);
    
    void executeInitScripts(const rapidjson::Value& createConfig, const std::string& scriptDir);
    void executeStartCommand(const rapidjson::Value& startConfig, const std::string& scriptDir);
    void detectAndSetUserContext();
    void terminateBackgroundProcesses();

    std::string extractAgentAnswer();
    bool evaluateByMatch(const std::string& answer, const rapidjson::Value& matchConfig);
    bool evaluateByCheckScripts(const std::string& answer, const rapidjson::Value& checkConfig);
    
    shared::ProcessResult executeScript(const std::string& language, const std::string& code, const std::vector<std::string>& params);
    shared::ProcessResult executeCheckScript(const std::string& language, const std::string& code, const std::vector<std::string>& params);
    
    std::string cleanTerminalOutput(const std::string& raw);
    std::string truncateOutput(const std::string& output);
    
    Agent& agent;
    shared::IHost& host;
    rapidjson::Document dataset;
    bool datasetLoaded = false;
    std::vector<std::string> backgroundProcessPids;
    std::string currentScriptDir;
    rapidjson::Document currentEvaluation;
};

}

#endif
