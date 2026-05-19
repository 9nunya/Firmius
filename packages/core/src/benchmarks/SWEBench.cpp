#include "benchmarks/SWEBench.hpp"
#include "agents/ContextBudget.hpp"
#include "benchmarks/SWEBenchTaskSpec.hpp"
#include "hosts/LocalHost.hpp"
#include "utils/Logger.hpp"
#include "utils/PlatformPaths.hpp"

#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <rapidjson/document.h>
#include <regex>
#include <sstream>
#include <utility>

namespace firmius::core {
using namespace firmius::shared;

namespace {
constexpr const char* kDefaultBenchmarkId = "swebench";
constexpr const char* kDefaultDatasetUrl =
    "https://datasets-server.huggingface.co/rows?dataset=princeton-nlp/SWE-bench_Verified&config=default&split=test&offset=0&limit=50";
constexpr const char* kDefaultDatasetCacheKey = "swebench_verified";

size_t writeToString(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

std::string normalizeTestName(const std::string& repo, const std::string& testName) {
    if (repo == "django/django") {
        std::regex reg(R"((.*) \((.*)\))");
        std::smatch match;
        if (std::regex_match(testName, match, reg)) {
            return std::string(match[2]) + "." + std::string(match[1]);
        }
    }
    return testName;
}

template <typename... Args>
std::string composeLog(Args&&... args) {
    std::ostringstream oss;
    (oss << ... << args);
    return oss.str();
}

template <typename... Args>
void logInfo(Args&&... args) {
    Logger::instance().log(LogLevel::Info,
                           composeLog(std::forward<Args>(args)...));
}

template <typename... Args>
void logDebug(Args&&... args) {
    Logger::instance().log(LogLevel::Debug,
                           composeLog(std::forward<Args>(args)...));
}

template <typename... Args>
void logError(Args&&... args) {
    Logger::instance().log(LogLevel::Error,
                           composeLog(std::forward<Args>(args)...));
}

struct ShellCommandResult {
    int exitCode = -1;
    std::string output;
};

ShellCommandResult runShellCommandCapture(const std::string& command) {
    ShellCommandResult result;
    LocalHost host;
    const auto execRes = host.exec(command);
    result.exitCode = execRes.exitCode;
    result.output = execRes.stdoutData;
    if (!execRes.stderrData.empty()) {
        if (!result.output.empty() && result.output.back() != '\n') {
            result.output += '\n';
        }
        result.output += execRes.stderrData;
    }
    return result;
}

std::string compactOutputForLog(const std::string& text, size_t maxChars = 3000) {
    if (text.size() <= maxChars) {
        return text;
    }
    return text.substr(0, maxChars) + "\n... [truncated]";
}

std::string quoteShellArg(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

bool runCommandsOrThrow(BenchmarkSession& session, const std::vector<std::string>& commands,
                        const std::string& stageDescription,
                        const std::map<std::string, std::string>& env = {}) {
    for (const auto& command : commands) {
        logDebug("[SWEBench][VERBOSE] ", stageDescription, ": ", command);
        auto res = session.getHost().exec(command, "/work", env);
        if (res.exitCode != 0) {
            logError("[SWEBench][ERROR] ", stageDescription, " failed for command: ",
                     command, "\n", compactOutputForLog(res.stderrData.empty()
                                                             ? res.stdoutData
                                                             : res.stderrData));
            throw std::runtime_error(stageDescription + " failed: " + command);
        }
    }
    return true;
}

std::string inferImportPackageName(const std::string& repo) {
    std::string packageName = repo.substr(repo.find('/') + 1);
    if (packageName.find("django") != std::string::npos) {
        return "django";
    }
    if (packageName.find("astropy") != std::string::npos) {
        return "astropy";
    }
    return packageName;
}

} // namespace

SWEBench::SWEBench(BenchmarkConfig config)
    : SWEBench(std::move(config), kDefaultBenchmarkId, kDefaultDatasetUrl,
               kDefaultDatasetCacheKey) {}

SWEBench::SWEBench(BenchmarkConfig config, std::string benchmarkId,
                   std::string datasetUrl, std::string datasetCacheKey)
    : session(std::move(config)), benchmarkId_(std::move(benchmarkId)),
      datasetUrl_(std::move(datasetUrl)), datasetCacheKey_(std::move(datasetCacheKey)) {}

std::vector<std::string> SWEBench::listTasks() {
    session.emitLog(benchmarkId_ + ": loading task catalog.");
    ensureDatasetLoaded();
    std::vector<std::string> tasks;
    if (dataset.HasMember("rows") && dataset["rows"].IsArray()) {
        for (const auto& row : dataset["rows"].GetArray()) {
            const auto& taskRow = row.HasMember("row") ? row["row"] : row;
            if (taskRow.HasMember("instance_id") && taskRow["instance_id"].IsString()) {
                tasks.push_back(taskRow["instance_id"].GetString());
            }
        }
    }
    return tasks;
}

const rapidjson::Value* SWEBench::findTaskRow(const std::string& taskId) {
    ensureDatasetLoaded();
    if (!dataset.HasMember("rows") || !dataset["rows"].IsArray()) {
        return nullptr;
    }
    for (const auto& row : dataset["rows"].GetArray()) {
        const auto& taskRow = row.HasMember("row") ? row["row"] : row;
        if (taskRow.HasMember("instance_id") && taskRow["instance_id"].IsString() &&
            std::string(taskRow["instance_id"].GetString()) == taskId) {
            return &taskRow;
        }
    }
    return nullptr;
}

SWEBenchTaskSpec SWEBench::requireTaskSpec(const std::string& taskId) {
    const rapidjson::Value* row = findTaskRow(taskId);
    if (!row) {
        throw std::runtime_error("Task not found: " + taskId);
    }
    return parseSWEBenchTaskSpec(*row);
}

std::string SWEBench::buildEvaluationCommand(const SWEBenchTaskSpec& spec) const {
    if (!spec.evalCommands.empty()) {
        std::ostringstream command;
        for (size_t i = 0; i < spec.evalCommands.size(); ++i) {
            if (i > 0) {
                command << " && ";
            }
            command << spec.evalCommands[i];
        }
        return command.str();
    }

    std::stringstream testCmdSS;
    const bool isDjango = (spec.repo == "django/django");
    if (isDjango) {
        testCmdSS << "python3 tests/runtests.py --settings=test_sqlite";
    } else {
        testCmdSS << "python3 -m pytest -v -p no:warnings";
    }

    for (const auto& testName : spec.failToPass) {
        testCmdSS << " " << quoteShellArg(normalizeTestName(spec.repo, testName));
    }
    return testCmdSS.str();
}

std::map<std::string, std::string> SWEBench::buildTestEnvironment(
    const SWEBenchTaskSpec& spec) const {
    std::map<std::string, std::string> env = spec.environment;
    if (!env.contains("PYTHONPATH")) {
        env["PYTHONPATH"] = "/work";
    }
    return env;
}

bool SWEBench::prepareTask(const std::string& taskId) {
    const SWEBenchTaskSpec spec = requireTaskSpec(taskId);

    logInfo("\033[1;32m[SWEBench] PREPARING ENVIRONMENT for ", taskId, "\033[0m");
    session.emitLog(benchmarkId_ + ": preparing environment for " + taskId + ".");

    std::string hostCacheBase = (firmius::shared::PlatformPaths::firmiusHomeDir() / "cache" / benchmarkId_ / "repos/").string();
    std::string repoCacheDir = hostCacheBase + spec.repo;

    if (!std::filesystem::exists(repoCacheDir)) {
        logDebug("[SWEBench][VERBOSE] Initial clone on host: ", spec.repo, "...");
        session.emitLog(benchmarkId_ + ": cloning https://github.com/" + spec.repo +
                        ".git into host cache.");
        std::filesystem::create_directories(hostCacheBase);
        std::string cloneCmd = "git clone https://github.com/" + spec.repo + ".git " + repoCacheDir;
        ShellCommandResult cloneRes = runShellCommandCapture(cloneCmd);
        if (cloneRes.exitCode != 0) {
            logError("[SWEBench][ERROR] Host clone failed (exit=", cloneRes.exitCode, "):\n",
                     compactOutputForLog(cloneRes.output));
            throw std::runtime_error("Failed to clone repository on host: " + spec.repo);
        }
    } else {
        session.emitLog(benchmarkId_ + ": using cached repository for " + spec.repo + ".");
        std::string shallowFile = repoCacheDir + "/.git/shallow";
        bool isShallow = std::filesystem::exists(shallowFile);

        if (isShallow) {
            session.emitLog(benchmarkId_ + ": cached repo is shallow, fetching full history.");
            std::string fetchCmd = "cd " + repoCacheDir +
                                   " && git fetch --unshallow && git fetch --tags origin";
            runShellCommandCapture(fetchCmd);
        } else {
            std::string checkCmd = "cd " + repoCacheDir + " && git rev-parse --verify " +
                                   spec.baseCommit + " >/dev/null 2>&1";
            ShellCommandResult checkRes = runShellCommandCapture(checkCmd);
            if (checkRes.exitCode != 0) {
                session.emitLog(benchmarkId_ + ": fetching missing commit " + spec.baseCommit +
                                " into cache.");
                std::string fetchCmd =
                    "cd " + repoCacheDir + " && git fetch origin && git fetch --tags origin";
                runShellCommandCapture(fetchCmd);
            }
        }
    }

    session.emitLog(benchmarkId_ + ": resetting /work and staging repository files.");
    std::string hostId = session.getHost().getId();
    if (hostId != "localhost") {
        session.getHost().exec("rm -rf /work/* /work/.[!.]* /work/..?* 2>/dev/null || true", "/work");
        session.getHost().exec("mkdir -p /work", "/work");

        const std::string mountedRepoDir = "/host_cache/" + spec.repo;
        auto mountedRepoCheck = session.getHost().exec(
            "[ -d \"" + mountedRepoDir + "/.git\" ]", "/work");
        if (mountedRepoCheck.exitCode == 0) {
            auto cpRes = session.getHost().exec(
                "cp -a \"" + mountedRepoDir + "/.\" /work/", "/work");
            if (cpRes.exitCode != 0) {
                const std::string copyError =
                    cpRes.stderrData.empty() ? cpRes.stdoutData : cpRes.stderrData;
                throw std::runtime_error("Failed to copy repository from volume mount: " +
                                         copyError);
            }
        } else {
            const std::string dockerCopyCmd =
                "docker cp \"" + repoCacheDir + "/.\" " + hostId + ":/work/";
            ShellCommandResult dockerCopyRes = runShellCommandCapture(dockerCopyCmd);
            if (dockerCopyRes.exitCode != 0) {
                throw std::runtime_error("Failed to copy repository into container: " +
                                         dockerCopyRes.output);
            }
        }
    } else {
        session.getHost().exec("rm -rf /work/* /work/.[!.]* /work/..?* 2>/dev/null || true", "/work");
        session.getHost().exec("mkdir -p /work", "/work");
        session.getHost().exec("cp -a " + repoCacheDir + "/. /work/");
    }

    session.emitLog(benchmarkId_ + ": checking out base commit " + spec.baseCommit + ".");
    session.getHost().exec("git config --global --add safe.directory /work", "/work");
    auto checkoutRes = session.getHost().exec("git checkout -f " + spec.baseCommit, "/work");
    if (checkoutRes.exitCode != 0) {
        auto fetchRes = session.getHost().exec("git fetch --unshallow 2>/dev/null || git fetch origin", "/work");
        if (fetchRes.exitCode == 0) {
            checkoutRes = session.getHost().exec("git checkout -f " + spec.baseCommit, "/work");
        }
        if (checkoutRes.exitCode != 0) {
            throw std::runtime_error("Git checkout failed in container: " + checkoutRes.stderrData);
        }
    }

    session.emitLog(benchmarkId_ + ": applying benchmark test patch.");
    session.getHost().writeFile("/work/test.patch",
                                std::vector<uint8_t>(spec.testPatch.begin(), spec.testPatch.end()));
    auto applyRes = session.getHost().exec("git apply /work/test.patch", "/work");
    if (applyRes.exitCode != 0) {
        session.getHost().exec("patch -p1 < /work/test.patch", "/work");
    }

    session.emitLog(benchmarkId_ + ": applying Python compatibility patches.");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.Mapping/collections.abc.Mapping/g' {} \\; 2>/dev/null || true", "/work");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.Iterable/collections.abc.Iterable/g' {} \\; 2>/dev/null || true", "/work");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.Callable/collections.abc.Callable/g' {} \\; 2>/dev/null || true", "/work");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.MutableSequence/collections.abc.MutableSequence/g' {} \\; 2>/dev/null || true", "/work");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.MutableMapping/collections.abc.MutableMapping/g' {} \\; 2>/dev/null || true", "/work");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.Sequence/collections.abc.Sequence/g' {} \\; 2>/dev/null || true", "/work");

    const auto testEnv = buildTestEnvironment(spec);
    if (!spec.installCommands.empty()) {
        session.emitLog(benchmarkId_ + ": running task install commands from environment_config.");
        runCommandsOrThrow(session, spec.installCommands, "install commands", testEnv);
    } else {
        session.emitLog(benchmarkId_ + ": installing repository dependencies with uv.");
        session.getHost().exec("uv pip uninstall --system --break-system-packages numpy setuptools wheel", "/work");
        auto grepDepUtil = session.getHost().exec("grep -r \"setuptools.dep_util\" .", "/work");
        auto grepPackageIndex = session.getHost().exec("grep -r \"setuptools.package_index\" .", "/work");
        bool needsLegacySetuptools = (grepDepUtil.exitCode == 0 || grepPackageIndex.exitCode == 0);

        std::string depCmd;
        if (spec.repo.find("astropy") != std::string::npos) {
            depCmd = needsLegacySetuptools
                         ? "uv pip install --system --break-system-packages --no-cache \"setuptools<60\" \"wheel\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml"
                         : "uv pip install --system --break-system-packages --no-cache \"setuptools>=68\" \"wheel\" \"numpy>=2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
        } else if (spec.repo.find("django") != std::string::npos) {
            depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools<60\" \"wheel\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
        } else {
            depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools<70\" \"wheel\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
        }

        auto depRes = session.getHost().exec(depCmd, "/work", testEnv);
        if (depRes.exitCode != 0) {
            throw std::runtime_error("Environment dependency installation failed");
        }
    }

    session.getHost().exec("sed -i 's/license = \"\\(.*\\)\"/license = {text = \"\\1\"}/g' pyproject.toml 2>/dev/null || true", "/work");
    session.getHost().exec("sed -i '/license-files/d' pyproject.toml 2>/dev/null || true", "/work");

    if (!spec.buildCommands.empty()) {
        session.emitLog(benchmarkId_ + ": running task build commands from environment_config.");
        runCommandsOrThrow(session, spec.buildCommands, "build commands", testEnv);
    } else {
        session.emitLog(benchmarkId_ + ": building extension modules.");
        session.getHost().exec("python3 setup.py build_ext --inplace", "/work", testEnv);

        session.emitLog(benchmarkId_ + ": installing repository into the benchmark environment.");
        auto installRes = session.getHost().exec("uv pip install --system --no-build-isolation -e .", "/work", testEnv);
        if (installRes.exitCode != 0) {
            session.getHost().exec("uv pip install --system --no-build-isolation .", "/work", testEnv);
        }
    }

    std::string packageName = inferImportPackageName(spec.repo);
    auto verifyRes = session.getHost().exec(
        "python3 -c \"import " + packageName + "; print('SUCCESS')\"", "/work", testEnv);
    if (verifyRes.exitCode != 0 || verifyRes.stdoutData.find("SUCCESS") == std::string::npos) {
        session.emitLog(benchmarkId_ + ": editable install verification failed, retrying fallback installation.");
        session.getHost().exec("python3 setup.py install --user", "/work", testEnv);
        verifyRes = session.getHost().exec(
            "python3 -c \"import " + packageName + "; print('SUCCESS')\"", "/work", testEnv);
        if (verifyRes.exitCode != 0) {
            throw std::runtime_error("Package verification failed after install");
        }
    }

    logInfo("\n\033[1;33m[SWEBench] RUNNING BASELINE TESTS (EXPECTED TO FAIL)\033[0m");
    session.emitLog(benchmarkId_ + ": running baseline failing tests before the worker starts.");

    auto baselineRes = session.getHost().exec(buildEvaluationCommand(spec), "/work", testEnv);
    int passed = 0, failed = 0, errors = 0;
    parseTestResults(baselineRes.stdoutData, passed, failed, errors);
    if (passed == 0 && failed == 0 && errors == 0) {
        parseTestResults(baselineRes.stderrData, passed, failed, errors);
    }

    session.emitLog(benchmarkId_ + ": baseline results passed=" + std::to_string(passed) +
                    ", failed=" + std::to_string(failed) +
                    ", errors=" + std::to_string(errors) + ".");
    return true;
}

BenchmarkResult SWEBench::runTask(const std::string& taskId) {
    const SWEBenchTaskSpec spec = requireTaskSpec(taskId);

    logInfo("\n\033[1;34m[SWEBench] SUMMONING AGENT\033[0m");
    session.emitLog(benchmarkId_ + ": running worker on task " + taskId + ".");
    std::stringstream prompt;
    prompt << "Task: " << spec.problemStatement << "\n\n"
           << "The repository has been cloned to /work and checked out at the base commit. "
           << "The relevant tests have been added to the codebase.\n"
           << "The following tests are currently FAILING. You must fix the code so they PASS:\n";
    for (const auto& testName : spec.failToPass) {
        prompt << "- " << testName << "\n";
    }
    if (!spec.installCommands.empty() || !spec.buildCommands.empty() || !spec.evalCommands.empty() ||
        !spec.environment.empty()) {
        prompt << "\nTask-specific environment data is present in environment_config. "
               << "Respect existing build/test commands and environment variables in the workspace.\n";
    }
    prompt << "\nFix the issue in the codebase. After finishing, provide a summary of your changes.";

    session.runAgentTask(prompt.str());

    logInfo("\n\033[1;32m[SWEBench] RUNNING FINAL EVALUATION\033[0m");
    session.emitLog(benchmarkId_ + ": running final evaluation.");
    auto finalRes = session.getHost().exec(buildEvaluationCommand(spec), "/work",
                                           buildTestEnvironment(spec));
    int passed = 0, failed = 0, errors = 0;
    parseTestResults(finalRes.stdoutData, passed, failed, errors);
    if (passed == 0 && failed == 0 && errors == 0) {
        parseTestResults(finalRes.stderrData, passed, failed, errors);
    }

    session.emitLog(benchmarkId_ + ": final results passed=" + std::to_string(passed) +
                    ", failed=" + std::to_string(failed) +
                    ", errors=" + std::to_string(errors) + ".");

    BenchmarkResult result;
    result.taskId = taskId;
    result.passed = (failed == 0 && errors == 0 && passed >= static_cast<int>(spec.failToPass.size()));
    result.metrics = session.getAgent().getContext().aggregateMetrics;
    result.output = "Final (Passed=" + std::to_string(passed) + ", Failed=" +
                    std::to_string(failed) + ")\nMetrics: " +
                    summarizeContextWindowMetrics(result.metrics.context, 4);
    return result;
}

void SWEBench::ensureDatasetLoaded() {
    if (datasetLoaded) {
        return;
    }
    std::string home = firmius::shared::PlatformPaths::firmiusHomeDir().string();
    std::string cacheFile = home + "/cache/" + datasetCacheKey_ + ".json";

    if (!std::filesystem::exists(cacheFile)) {
        session.emitLog(benchmarkId_ + ": downloading dataset cache.");
        std::filesystem::create_directories(std::filesystem::path(cacheFile).parent_path());
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize curl for benchmark dataset download");
        }
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, datasetUrl_.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        std::ofstream out(cacheFile);
        out << response;
        out.close();
    } else {
        session.emitLog(benchmarkId_ + ": using cached dataset at " + cacheFile + ".");
    }

    std::ifstream in(cacheFile);
    std::stringstream buffer;
    buffer << in.rdbuf();
    dataset.Parse(buffer.str().c_str());
    datasetLoaded = true;
}

bool SWEBench::parseTestResults(const std::string& output, int& passed, int& failed, int& errors) {
    passed = 0;
    failed = 0;
    errors = 0;
    bool found = false;
    std::regex pReg(R"((\d+) passed)");
    std::regex fReg(R"((\d+) failed)");
    std::regex eReg(R"((\d+) error)");
    std::smatch m;
    if (std::regex_search(output, m, pReg)) {
        passed = std::stoi(m[1]);
        found = true;
    }
    if (std::regex_search(output, m, fReg)) {
        failed = std::stoi(m[1]);
        found = true;
    }
    if (std::regex_search(output, m, eReg)) {
        errors = std::stoi(m[1]);
        found = true;
    }
    if (!found) {
        size_t pos = 0;
        while ((pos = output.find("PASSED", pos)) != std::string::npos) {
            ++passed;
            pos += 6;
            found = true;
        }
        pos = 0;
        while ((pos = output.find("FAILED", pos)) != std::string::npos) {
            ++failed;
            pos += 6;
            found = true;
        }
        pos = 0;
        while ((pos = output.find("ERROR", pos)) != std::string::npos) {
            ++errors;
            pos += 5;
            found = true;
        }
    }
    return found;
}

} // namespace firmius::core
