#include "benchmarks/AgentBench.hpp"
#include "agents/ContextBudget.hpp"
#include "utils/Logger.hpp"
#include <curl/curl.h>
#include <fstream>
#include <filesystem>
#include <rapidjson/document.h>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unordered_set>
#include <algorithm>

namespace firmius::core {
using namespace firmius::shared;

namespace {
size_t writeToFile(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

const std::string SCRIPT_BASE_URL = "https://raw.githubusercontent.com/THUDM/AgentBench/main/data/os_interaction/scripts/";
}

AgentBench::AgentBench(BenchmarkConfig config) : session(std::move(config)) {}

std::vector<std::string> AgentBench::listTasks() {
    session.emitLog("AgentBench: loading OS interaction tasks.");
    ensureDatasetLoaded();
    std::vector<std::string> tasks;
    if (dataset.IsArray()) {
        for (rapidjson::SizeType i = 0; i < dataset.Size(); ++i) {
            tasks.push_back(std::to_string(i));
        }
    }
    return tasks;
}

bool AgentBench::prepareTask(const std::string& taskId) {
    ensureDatasetLoaded();
    ensureScriptsFetched();
    session.emitLog("AgentBench: preparing task " + taskId + ".");

    int index = std::stoi(taskId);
    if (index < 0 || index >= (int)dataset.Size()) throw std::runtime_error("Task index out of range");

    const auto& task = dataset[index];

    session.emitLog("AgentBench: resetting /work.");
    session.getHost().exec("rm -rf /work/* /work/.* 2>/dev/null || true");
    backgroundProcessPids.clear();

    currentScriptDir = "dev";

    if (task.HasMember("create") && task["create"].IsObject() && task["create"].HasMember("init")) {
        session.emitLog("AgentBench: running initialization scripts.");
        executeInitScripts(task["create"], currentScriptDir);
    }

    if (task.HasMember("start")) {
        session.emitLog("AgentBench: starting benchmark background services.");
        executeStartCommand(task["start"], currentScriptDir);
    }

    if (task.HasMember("evaluation")) {
        currentEvaluation.CopyFrom(task["evaluation"], currentEvaluation.GetAllocator());
    } else {
        currentEvaluation.SetNull();
    }

    detectAndSetUserContext();

    return true;
}

BenchmarkResult AgentBench::runTask(const std::string& taskId) {
    int index = std::stoi(taskId);
    if (index < 0 || index >= (int)dataset.Size()) throw std::runtime_error("Task index out of range");

    const auto& task = dataset[index];
    std::string description = task["description"].GetString();

    std::string fullPrompt = "Task: " + description + "\n\nProvide your final answer after you have found it. Do not call tools after you have found the answer.";

    session.emitLog("AgentBench: running worker on task " + taskId + ".");
    session.runAgentTask(fullPrompt);

    session.emitLog("AgentBench: terminating benchmark background services.");
    terminateBackgroundProcesses();

    BenchmarkResult result;
    result.taskId = taskId;

    std::string answer = extractAgentAnswer();

    bool passed = false;
    if (task.HasMember("evaluation")) {
        const auto& eval = task["evaluation"];

        if (eval.HasMember("match")) {
            passed = evaluateByMatch(answer, eval["match"]);
        } else if (eval.HasMember("check")) {
            passed = evaluateByCheckScripts(answer, eval["check"]);
        }
    }

    result.passed = passed;
    result.output = passed ? "Task passed" : "Task failed";
    result.metrics = session.getAgent().getContext().aggregateMetrics;
    result.output += "\nMetrics: " +
                     summarizeContextWindowMetrics(result.metrics.context, 4);
    session.emitLog("AgentBench: evaluation result for task " + taskId + " = " +
                    std::string(passed ? "PASS" : "FAIL") + ".");
    Logger::instance().logDebug("DEBUG: Evaluation result: " +
                               std::string(passed ? "PASSED" : "FAILED"));

    return result;
}

void AgentBench::ensureDatasetLoaded() {
    if (datasetLoaded) return;

    std::string home;
    if (const char* h = std::getenv("HOME")) home = h;
    if (const char* su = std::getenv("SUDO_USER")) {
        std::string sudoHome = "/home/" + std::string(su);
        if (std::filesystem::exists(sudoHome)) home = sudoHome;
    }
    if (home.empty()) home = "/root";

    std::string cacheDir = home + "/.firmius/cache/agentbench";
    std::string cacheFile = cacheDir + "/os_interaction_dev.json";

    if (!std::filesystem::exists(cacheFile)) {
        session.emitLog("AgentBench: downloading dataset cache.");
        std::filesystem::create_directories(cacheDir);
        CURL* curl = curl_easy_init();
        std::ofstream out(cacheFile, std::ios::binary);
        curl_easy_setopt(curl, CURLOPT_URL, "https://raw.githubusercontent.com/THUDM/AgentBench/main/data/os_interaction/data/dev.json");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        out.close();
    } else {
        session.emitLog("AgentBench: using cached dataset at " + cacheFile + ".");
    }

    std::ifstream in(cacheFile);
    std::stringstream buffer;
    buffer << in.rdbuf();
    dataset.Parse(buffer.str().c_str());
    datasetLoaded = true;
}

void AgentBench::ensureScriptsFetched() {
    session.emitLog("AgentBench: verifying cached helper scripts.");
    std::unordered_set<std::string> neededScripts;

    for (const auto& task : dataset.GetArray()) {
        std::string scriptDir = "dev"; // Dataset scripts are mostly in dev

        if (task.HasMember("create") && task["create"].IsObject() && task["create"].HasMember("init")) {
            collectScriptPaths(task["create"]["init"], scriptDir, neededScripts);
        }

        if (task.HasMember("start")) {
            if (task["start"].IsObject() && task["start"].HasMember("file")) {
                neededScripts.insert(scriptDir + "/" + task["start"]["file"].GetString());
            }
        }

        if (task.HasMember("evaluation")) {
            const auto& eval = task["evaluation"];
            if (eval.HasMember("check")) {
                collectScriptPaths(eval["check"], scriptDir, neededScripts);
            }
            if (eval.HasMember("example")) {
                collectScriptPaths(eval["example"], scriptDir, neededScripts);
            }
        }
    }

    std::string home;
    if (const char* h = std::getenv("HOME")) home = h;
    if (const char* su = std::getenv("SUDO_USER")) {
        std::string sudoHome = "/home/" + std::string(su);
        if (std::filesystem::exists(sudoHome)) home = sudoHome;
    }
    if (home.empty()) home = "/root";
    std::string scriptCacheDir = home + "/.firmius/cache/agentbench/scripts/";

    for (const auto& scriptPath : neededScripts) {
        std::string localPath = scriptCacheDir + scriptPath;
        std::filesystem::create_directories(std::filesystem::path(localPath).parent_path());

        if (!std::filesystem::exists(localPath)) {
            session.emitLog("AgentBench: fetching script " + scriptPath + ".");
            fetchScriptFromGitHub(scriptPath, localPath);
        }
    }
}

void AgentBench::collectScriptPaths(const rapidjson::Value& scriptObj, const std::string& scriptDir, std::unordered_set<std::string>& paths) {
    if (scriptObj.IsString()) {
        return;
    } else if (scriptObj.IsObject() && scriptObj.HasMember("file")) {
        paths.insert(scriptDir + "/" + scriptObj["file"].GetString());
    } else if (scriptObj.IsArray()) {
        for (const auto& item : scriptObj.GetArray()) {
            collectScriptPaths(item, scriptDir, paths);
        }
    }
}

std::string AgentBench::fetchScriptFromGitHub(const std::string& scriptPath, const std::string& localPath) {
    std::string url = SCRIPT_BASE_URL + scriptPath;

    CURL* curl = curl_easy_init();
    std::ofstream out(localPath, std::ios::binary);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    out.close();

    if (res != CURLE_OK) {
        std::filesystem::remove(localPath);
        throw std::runtime_error("Failed to fetch script: " + scriptPath + " (" + curl_easy_strerror(res) + ")");
    }

    std::ifstream in(localPath);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::tuple<std::string, std::string> AgentBench::loadScriptObj(const rapidjson::Value& scriptObj, const std::string& scriptDir) {
    std::string home;
    if (const char* h = std::getenv("HOME")) home = h;
    if (const char* su = std::getenv("SUDO_USER")) {
        std::string sudoHome = "/home/" + std::string(su);
        if (std::filesystem::exists(sudoHome)) home = sudoHome;
    }
    if (home.empty()) home = "/root";
    std::string scriptCacheDir = home + "/.firmius/cache/agentbench/scripts/";
    std::string language = "bash";

    if (scriptObj.IsString()) {
        return {"bash", scriptObj.GetString()};
    }

    if (scriptObj.HasMember("language")) {
        language = scriptObj["language"].GetString();
    }

    if (scriptObj.HasMember("file")) {
        std::string filePath = scriptDir + "/" + scriptObj["file"].GetString();
        std::string fullPath = scriptCacheDir + filePath;

        if (!std::filesystem::exists(fullPath)) {
            throw std::runtime_error("Script file not found: " + fullPath);
        }

        std::ifstream in(fullPath);
        std::stringstream buffer;
        buffer << in.rdbuf();
        return {language, buffer.str()};
    }

    if (scriptObj.HasMember("code")) {
        return {language, scriptObj["code"].GetString()};
    }

    throw std::runtime_error("Invalid script object");
}

void AgentBench::executeInitScripts(const rapidjson::Value& createConfig, const std::string& scriptDir) {
    const auto& init = createConfig["init"];
    std::string lastInitCode;

    auto runInit = [&](const rapidjson::Value& scriptObj) {
        if (scriptObj.IsNull()) return;
        auto [language, code] = loadScriptObj(scriptObj, scriptDir);
        session.emitLog("AgentBench: running init script (" + language + ").");
        lastInitCode = code;
        auto result = executeScript(language, code, {});
        if (result.exitCode != 0) {
            throw std::runtime_error("Init script failed: " + result.stderrData);
        }
    };

    if (init.IsString() || init.IsObject()) {
        runInit(init);
    } else if (init.IsArray()) {
        for (const auto& scriptObj : init.GetArray()) {
            runInit(scriptObj);
        }
    }

    // Heuristic: Check if the last init script ended with su - user
    std::regex suRegex(R"(su\s+-\s+([a-zA-Z0-9_-]+)\s*$)");
    std::smatch match;
    if (std::regex_search(lastInitCode, match, suRegex)) {
        session.getHost().setUser(match[1]);
    }
}

void AgentBench::executeStartCommand(const rapidjson::Value& startConfig, const std::string& scriptDir) {
    auto [language, startCommand] = loadScriptObj(startConfig, scriptDir);
    session.emitLog("AgentBench: starting service command (" + language + ").");

    std::string cmd = startCommand;
    if (cmd.find("&") == std::string::npos) {
        cmd += " &";
    }
    
    // Append echo $! to get the last background PID
    cmd += " echo $!";

    auto result = session.getHost().exec(cmd);
    std::string output = result.stdoutData;
    
    // Extract PID from the last line
    std::stringstream ss(output);
    std::string line;
    std::string lastPid;
    while (std::getline(ss, line)) {
        line.erase(0, line.find_first_not_of(" \t\n\r"));
        line.erase(line.find_last_not_of(" \t\n\r") + 1);
        if (!line.empty() && std::all_of(line.begin(), line.end(), ::isdigit)) {
            lastPid = line;
        }
    }

    if (!lastPid.empty()) {
        backgroundProcessPids.push_back(lastPid);
    }
}

void AgentBench::detectAndSetUserContext() {
    // Heuristic: Check if any init script ended with su - user
    // We should have tracked the last init script's code.
    // Since we don't have it easily here, let's try to run whoami 
    // but in a way that might catch a user switch if it was persistent (unlikely)
    // or just rely on a more thorough check.
    
    auto result = session.getHost().exec("whoami");
    std::string user = result.stdoutData;
    user.erase(std::remove_if(user.begin(), user.end(), ::isspace), user.end());

    if (!user.empty() && user != "root") {
        session.getHost().setUser(user);
    }
}

void AgentBench::terminateBackgroundProcesses() {
    for (const auto& pid : backgroundProcessPids) {
        session.emitLog("AgentBench: stopping background process " + pid + ".");
        session.getHost().exec("kill " + pid + " 2>/dev/null || true");
    }
    backgroundProcessPids.clear();
}

std::string AgentBench::extractAgentAnswer() {
    const auto& turns = session.getAgent().getContext().history->turns;
    if (turns.empty()) {
        Logger::instance().logDebug("DEBUG: No turns in history");
        return "";
    }

    for (auto it = turns.rbegin(); it != turns.rend(); ++it) {
        const auto& turn = *it;
        for (auto msgIt = turn.messages.rbegin(); msgIt != turn.messages.rend(); ++msgIt) {
            const auto& msg = *msgIt;
            if (msg.role != Role::Assistant) continue;
            
            std::string content;
            for (const auto& part : msg.content) {
                if (auto* txt = std::get_if<TextContent>(&part)) {
                    content += txt->text;
                }
            }

            if (content.empty()) continue;

            std::string cleaned = content;
            
            // Robust trim
            auto first = cleaned.find_first_not_of(" \t\n\r");
            if (first == std::string::npos) {
                cleaned.clear();
            } else {
                cleaned.erase(0, first);
                auto last = cleaned.find_last_not_of(" \t\n\r");
                if (last != std::string::npos) {
                    cleaned.erase(last + 1);
                }
            }

            if (cleaned.empty()) continue;
            
            // Prioritize "Answer: X" or "Final Answer: X"
            std::regex answerPrefixRegex(R"((?:[Aa]nswer|[Ff]inal [Aa]nswer)\s*[:\-]?\s*(\d+))");
            std::smatch match;
            std::string searchTarget = cleaned;
            std::string foundNumber;
            
            while (std::regex_search(searchTarget, match, answerPrefixRegex)) {
                foundNumber = match[1];
                searchTarget = match.suffix();
            }
            if (!foundNumber.empty()) return foundNumber;

            // Then try bolded numbers
            std::regex boldNumRegex(R"(\*\*(\d+)\*\*)");
            searchTarget = cleaned;
            while (std::regex_search(searchTarget, match, boldNumRegex)) {
                foundNumber = match[1];
                searchTarget = match.suffix();
            }
            if (!foundNumber.empty()) return foundNumber;

            // Then just the last number
            std::regex numRegex(R"((\d+))");
            searchTarget = cleaned;
            while (std::regex_search(searchTarget, match, numRegex)) {
                foundNumber = match[1];
                searchTarget = match.suffix();
            }
            if (!foundNumber.empty()) return foundNumber;

            return cleaned;
        }
    }

    return "";
}

bool AgentBench::evaluateByMatch(const std::string& answer, const rapidjson::Value& matchConfig) {
    std::string cleanedAnswer = answer;
    // Strip leading/trailing whitespace
    cleanedAnswer.erase(0, cleanedAnswer.find_first_not_of(" \t\n\r"));
    cleanedAnswer.erase(cleanedAnswer.find_last_not_of(" \t\n\r") + 1);

    if (matchConfig.IsString()) {
        std::string expected = matchConfig.GetString();
        expected.erase(0, expected.find_first_not_of(" \t\n\r"));
        expected.erase(expected.find_last_not_of(" \t\n\r") + 1);
        return cleanedAnswer == expected;
    } else if (matchConfig.IsObject()) {
        if (matchConfig.HasMember("answer")) {
            std::string expected = matchConfig["answer"].GetString();
            if (matchConfig.HasMember("strip") && matchConfig["strip"].GetBool()) {
                expected.erase(0, expected.find_first_not_of(" \t\n\r"));
                expected.erase(expected.find_last_not_of(" \t\n\r") + 1);
                cleanedAnswer.erase(0, cleanedAnswer.find_first_not_of(" \t\n\r"));
                cleanedAnswer.erase(cleanedAnswer.find_last_not_of(" \t\n\r") + 1);
            }
            return cleanedAnswer == expected;
        } else if (matchConfig.HasMember("regex")) {
            std::string pattern = matchConfig["regex"].GetString();
            std::regex re(pattern);
            return std::regex_search(answer, re);
        }
    }

    return false;
}

bool AgentBench::evaluateByCheckScripts(const std::string& answer, const rapidjson::Value& checkConfig) {
    std::vector<std::string> params = {answer};

    std::vector<const rapidjson::Value*> checkScripts;
    if (checkConfig.IsArray()) {
        for (const auto& v : checkConfig.GetArray()) {
            checkScripts.push_back(&v);
        }
    } else {
        checkScripts.push_back(&checkConfig);
    }

    for (const auto* scriptObj : checkScripts) {
        if (scriptObj->IsNull()) {
            // Use example script if available
            if (!currentEvaluation.IsNull() && currentEvaluation.HasMember("example")) {
                auto [language, code] = loadScriptObj(currentEvaluation["example"], currentScriptDir);
                auto result = executeCheckScript(language, code, params);
                if (result.exitCode != 0) return false;
                
                std::string stdoutClean = result.stdoutData;
                auto first = stdoutClean.find_first_not_of(" \t\n\r");
                if (first != std::string::npos) {
                    stdoutClean.erase(0, first);
                    auto last = stdoutClean.find_last_not_of(" \t\n\r");
                    if (last != std::string::npos) stdoutClean.erase(last + 1);
                } else {
                    stdoutClean.clear();
                }
                params.push_back(stdoutClean);
            }
            continue;
        }

        auto [language, code] = loadScriptObj(*scriptObj, currentScriptDir);
        auto result = executeCheckScript(language, code, params);
        
        if (result.exitCode != 0) {
            return false;
        }

        std::string stdoutClean = result.stdoutData;
        auto first = stdoutClean.find_first_not_of(" \t\n\r");
        if (first != std::string::npos) {
            stdoutClean.erase(0, first);
            auto last = stdoutClean.find_last_not_of(" \t\n\r");
            if (last != std::string::npos) stdoutClean.erase(last + 1);
        } else {
            stdoutClean.clear();
        }
        params.push_back(stdoutClean);
    }

    return true;
}

ProcessResult AgentBench::executeScript(const std::string& language, const std::string& code, const std::vector<std::string>& params) {
    std::string scriptPath = "/tmp/agentbench_script";
    if (language == "bash") {
        scriptPath += ".sh";
        session.getHost().writeFile(scriptPath, std::vector<uint8_t>(code.begin(), code.end()));
        std::string cmd = "bash " + scriptPath;
        for (const auto& param : params) {
            cmd += " \"" + param + "\"";
        }
        return session.getHost().exec(cmd);
    } else if (language == "python") {
        scriptPath += ".py";
        session.getHost().writeFile(scriptPath, std::vector<uint8_t>(code.begin(), code.end()));
        std::string cmd = "python3 " + scriptPath;
        for (const auto& param : params) {
            cmd += " \"" + param + "\"";
        }
        return session.getHost().exec(cmd);
    } else if (language == "c++" || language == "c") {
        std::string ext = (language == "c++") ? ".cpp" : ".c";
        scriptPath += ext;
        session.getHost().writeFile(scriptPath, std::vector<uint8_t>(code.begin(), code.end()));

        std::string compiler = (language == "c++") ? "g++" : "gcc";
        auto compileResult = session.getHost().exec(compiler + " -o /tmp/a.out " + scriptPath + " 2>&1");

        if (compileResult.exitCode != 0) {
            ProcessResult res;
            res.exitCode = compileResult.exitCode;
            res.stdoutData = "";
            res.stderrData = compileResult.stderrData;
            res.durationMs = 0.0;
            return res;
        }

        std::string cmd = "/tmp/a.out";
        for (const auto& param : params) {
            cmd += " " + param;
        }
        return session.getHost().exec(cmd);
    } else {
        throw std::runtime_error("Unsupported script language: " + language);
    }
}

ProcessResult AgentBench::executeCheckScript(const std::string& language, const std::string& code, const std::vector<std::string>& params) {
    auto result = executeScript(language, code, params);
    result.stdoutData = truncateOutput(result.stdoutData);
    result.stderrData = truncateOutput(result.stderrData);
    return result;
}

std::string AgentBench::cleanTerminalOutput(const std::string& raw) {
    std::string output = raw;

    output = std::regex_replace(output, std::regex("\\x1b.+@.+[#|$] "), "");
    output = std::regex_replace(output, std::regex("\\x1b\\[[0-9;]*[a-zA-Z]"), "");
    output = std::regex_replace(output, std::regex("\\x1b][0-9]*;[^\\x07]*\\x07"), "");
    output = std::regex_replace(output, std::regex("\\x1b\\[\\?2004[hl]"), "");
    output = std::regex_replace(output, std::regex("\\x07"), "");

    return output;
}

std::string AgentBench::truncateOutput(const std::string& output) {
    if (output.length() > 800) {
        return output.substr(0, 780) + "\n[truncated because the output is too long]";
    }
    return output;
}

}
