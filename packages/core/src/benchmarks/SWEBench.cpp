#include "benchmarks/SWEBench.hpp"
#include "agents/ContextBudget.hpp"
#include "utils/Logger.hpp"

#include <array>
#include <cstdlib>
#include <curl/curl.h>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <regex>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <sys/wait.h>
#include <utility>

namespace firmius::core {
using namespace firmius::shared;

namespace {
size_t writeToString(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

std::vector<std::string> parseJsonArray(const std::string& jsonStr) {
    rapidjson::Document d;
    d.Parse(jsonStr.c_str());
    std::vector<std::string> result;
    if (d.IsArray()) {
        for (const auto& item : d.GetArray()) {
            if (item.IsString()) result.push_back(item.GetString());
        }
    }
    return result;
}

std::string normalizeTestName(const std::string& repo, const std::string& testName) {
    if (repo == "django/django") {
        // Convert "test_method (path.Class)" to "path.Class.test_method"
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
    std::array<char, 4096> buffer{};
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        result.output = "Failed to spawn shell command";
        return result;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output += buffer.data();
    }

    const int status = pclose(pipe);
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else {
        result.exitCode = status;
    }
    return result;
}

std::string compactOutputForLog(const std::string& text, size_t maxChars = 3000) {
    if (text.size() <= maxChars) {
        return text;
    }
    return text.substr(0, maxChars) + "\n... [truncated]";
}
}

SWEBench::SWEBench(BenchmarkConfig config) : session(std::move(config)) {}

std::vector<std::string> SWEBench::listTasks() {
    session.emitLog("SWE-bench: loading verified task catalog.");
    ensureDatasetLoaded();
    std::vector<std::string> tasks;
    if (dataset.HasMember("rows") && dataset["rows"].IsArray()) {
        for (const auto& row : dataset["rows"].GetArray()) {
            tasks.push_back(row["row"]["instance_id"].GetString());
        }
    }
    return tasks;
}

bool SWEBench::prepareTask(const std::string& taskId) {
    ensureDatasetLoaded();

    const rapidjson::Value* row = nullptr;
    for (const auto& r : dataset["rows"].GetArray()) {
        if (std::string(r["row"]["instance_id"].GetString()) == taskId) {
            row = &r["row"];
            break;
        }
    }

    if (!row) throw std::runtime_error("Task not found: " + taskId);

    std::string repo = (*row)["repo"].GetString();
    std::string baseCommit = (*row)["base_commit"].GetString();
    std::string failToPassStr = (*row)["FAIL_TO_PASS"].GetString();
    std::string testPatch = (*row)["test_patch"].GetString();
    std::vector<std::string> failToPass = parseJsonArray(failToPassStr);

    logInfo("\033[1;32m[SWEBench] PREPARING ENVIRONMENT for ", taskId, "\033[0m");
    session.emitLog("SWE-bench: preparing environment for " + taskId + ".");

    std::string home;
    if (const char* h = std::getenv("HOME")) home = h;
    if (const char* su = std::getenv("SUDO_USER")) {
        // If we are root via sudo, /home/{SUDO_USER} might be better
        std::string sudoHome = "/home/" + std::string(su);
        if (std::filesystem::exists(sudoHome)) home = sudoHome;
    }
    if (home.empty()) home = "/root";

    std::string hostCacheBase = home + "/.firmius/cache/swebench/repos/";
    std::string repoCacheDir = hostCacheBase + repo;

    if (!std::filesystem::exists(repoCacheDir)) {
        logDebug("[SWEBench][VERBOSE] Initial clone on host: ", repo, "...");
        session.emitLog("SWE-bench: cloning https://github.com/" + repo + ".git into host cache.");
        std::filesystem::create_directories(hostCacheBase);
        // Full clone to ensure all commits are available
        std::string cloneCmd = "git clone https://github.com/" + repo + ".git " + repoCacheDir;
        ShellCommandResult cloneRes = runShellCommandCapture(cloneCmd);
        if (cloneRes.exitCode != 0) {
            logError("[SWEBench][ERROR] Host clone failed (exit=", cloneRes.exitCode, "):\n",
                     compactOutputForLog(cloneRes.output));
            throw std::runtime_error("Failed to clone repository on host: " + repo);
        }
    } else {
        session.emitLog("SWE-bench: using cached repository for " + repo + ".");
        // Check if repo is shallow (incomplete history)
        std::string shallowFile = repoCacheDir + "/.git/shallow";
        bool isShallow = std::filesystem::exists(shallowFile);
        
        if (isShallow) {
            logDebug("[SWEBench][VERBOSE] Repo is shallow, fetching full history...");
            session.emitLog("SWE-bench: cached repo is shallow, fetching full history.");
            std::string fetchCmd = "cd " + repoCacheDir + " && git fetch --unshallow && git fetch --tags origin";
            ShellCommandResult fetchRes = runShellCommandCapture(fetchCmd);
            if (fetchRes.exitCode != 0) {
                logError("[SWEBench][ERROR] Host unshallow fetch failed (exit=", fetchRes.exitCode, "):\n",
                         compactOutputForLog(fetchRes.output));
            }
        } else {
            // Ensure the commit exists in host cache
            std::string checkCmd = "cd " + repoCacheDir + " && git rev-parse --verify " + baseCommit + " >/dev/null 2>&1";
            ShellCommandResult checkRes = runShellCommandCapture(checkCmd);
            if (checkRes.exitCode != 0) {
                logDebug("[SWEBench][VERBOSE] Fetching missing commit ", baseCommit, " on host...");
                session.emitLog("SWE-bench: fetching missing commit " + baseCommit + " into cache.");
                // Fetch everything to be safe if a specific commit fetch fails
                std::string fetchCmd = "cd " + repoCacheDir + " && git fetch origin && git fetch --tags origin";
                ShellCommandResult fetchRes = runShellCommandCapture(fetchCmd);
                if (fetchRes.exitCode != 0) {
                    logError("[SWEBench][ERROR] Host fetch for missing commit failed (exit=",
                             fetchRes.exitCode, "):\n", compactOutputForLog(fetchRes.output));
                }
            }
        }
    }

    logDebug("[SWEBench][VERBOSE] Resetting /work and copying from host cache...");
    session.emitLog("SWE-bench: resetting /work and staging repository files.");
    
    std::string hostId = session.getHost().getId();
    logDebug("[SWEBench][DEBUG] Host ID: ", hostId);
    
    // For Docker hosts, prefer mounted host cache and fall back to docker cp.
    if (hostId != "localhost") {
        session.getHost().exec("rm -rf /work/* /work/.[!.]* /work/..?* 2>/dev/null || true", "/work");
        session.getHost().exec("mkdir -p /work", "/work");

        const std::string mountedRepoDir = "/host_cache/" + repo;
        auto mountedRepoCheck = session.getHost().exec(
            "[ -d \"" + mountedRepoDir + "/.git\" ]", "/work");

        if (mountedRepoCheck.exitCode == 0) {
            session.emitLog("SWE-bench: using volume-mounted repository (host=" + hostId + ").");
            auto cpRes = session.getHost().exec(
                "cp -a \"" + mountedRepoDir + "/.\" /work/", "/work");
            if (cpRes.exitCode != 0) {
                const std::string copyError =
                    cpRes.stderrData.empty() ? cpRes.stdoutData : cpRes.stderrData;
                logError("[SWEBench][ERROR] cp from /host_cache failed: ", copyError);
                throw std::runtime_error("Failed to copy repository from volume mount: " +
                                         copyError);
            }
        } else {
            session.emitLog("SWE-bench: /host_cache not mounted; staging repository via docker cp.");
            const std::string dockerCopyCmd =
                "docker cp \"" + repoCacheDir + "/.\" " + hostId + ":/work/";
            ShellCommandResult dockerCopyRes = runShellCommandCapture(dockerCopyCmd);
            if (dockerCopyRes.exitCode != 0) {
                logError("[SWEBench][ERROR] docker cp failed (exit=",
                         dockerCopyRes.exitCode, "):\n",
                         compactOutputForLog(dockerCopyRes.output));
                throw std::runtime_error("Failed to copy repository into container: " +
                                         dockerCopyRes.output);
            }
        }

        // Verify .git was copied
        auto gitCheck = session.getHost().exec("ls -la /work/.git", "/work");
        logDebug("[SWEBench][DEBUG] .git in /work: ", gitCheck.stdoutData);
        if (gitCheck.exitCode != 0) {
            const std::string verifyError =
                gitCheck.stderrData.empty() ? gitCheck.stdoutData : gitCheck.stderrData;
            throw std::runtime_error("Repository staging failed: /work/.git missing (" +
                                     verifyError + ")");
        }
    } else {
        session.emitLog("SWE-bench: using local host copy.");
        session.getHost().exec("rm -rf /work/* /work/.[!.]* /work/..?* 2>/dev/null || true", "/work");
        session.getHost().exec("mkdir -p /work", "/work");
        session.getHost().exec("cp -a " + repoCacheDir + "/. /work/");
    }

    logDebug("[SWEBench][VERBOSE] Checking out ", baseCommit, "...");
    session.emitLog("SWE-bench: checking out base commit " + baseCommit + ".");
    session.getHost().exec("git config --global --add safe.directory /work", "/work");
    auto checkoutRes = session.getHost().exec("git checkout -f " + baseCommit, "/work");
    if (checkoutRes.exitCode != 0) {
        logDebug("[SWEBench][DEBUG] Checkout failed. Fetching full history...");
        session.emitLog("SWE-bench: fetching full history for commit " + baseCommit + ".");
        // Try to fetch full history in container
        auto fetchRes = session.getHost().exec("git fetch --unshallow 2>/dev/null || git fetch origin", "/work");
        if (fetchRes.exitCode == 0) {
            // Retry checkout after fetch
            checkoutRes = session.getHost().exec("git checkout -f " + baseCommit, "/work");
        }
        if (checkoutRes.exitCode != 0) {
            logDebug("[SWEBench][DEBUG] Checkout failed after fetch. The commit might be missing.");
            throw std::runtime_error("Git checkout failed in container: " + checkoutRes.stderrData);
        }
    }
    
    auto logRes = session.getHost().exec("git log -1 --format=%H", "/work");
    logDebug("[SWEBench][DEBUG] Current Commit in Container: ", logRes.stdoutData);
    if (logRes.stdoutData.find(baseCommit) == std::string::npos && baseCommit.find(logRes.stdoutData.substr(0, 7)) == std::string::npos) {
        logError("[SWEBench][ERROR] Checkout failed to reach target commit!");
    }

    logDebug("[SWEBench][VERBOSE] Applying test patch...");
    session.emitLog("SWE-bench: applying benchmark test patch.");
    session.getHost().writeFile("/work/test.patch", std::vector<uint8_t>(testPatch.begin(), testPatch.end()));
    auto applyRes = session.getHost().exec("git apply /work/test.patch", "/work");
    if (applyRes.exitCode != 0) {
        session.getHost().exec("patch -p1 < /work/test.patch", "/work");
    }

    // Fix for Python 3.10+ compatibility: collections.Mapping -> collections.abc.Mapping
    // This affects astropy's vendored configobj
    logDebug("[SWEBench][VERBOSE] Applying Python 3.10+ compatibility patches...");
    session.emitLog("SWE-bench: applying Python compatibility patches.");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.Mapping/collections.abc.Mapping/g' {} \\; 2>/dev/null || true", "/work");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.Iterable/collections.abc.Iterable/g' {} \\; 2>/dev/null || true", "/work");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.Callable/collections.abc.Callable/g' {} \\; 2>/dev/null || true", "/work");
    // Additional patches for Python 3.10+ (removed in Python 3.11)
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.MutableSequence/collections.abc.MutableSequence/g' {} \\; 2>/dev/null || true", "/work");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.MutableMapping/collections.abc.MutableMapping/g' {} \\; 2>/dev/null || true", "/work");
    session.getHost().exec("find . -name '*.py' -exec sed -i 's/collections\\.Sequence/collections.abc.Sequence/g' {} \\; 2>/dev/null || true", "/work");

    logDebug("[SWEBench][VERBOSE] Installing environment dependencies with uv...");
    session.emitLog("SWE-bench: installing repository dependencies with uv.");
    
    // Force clean to ensure no version conflicts
    session.getHost().exec("uv pip uninstall --system --break-system-packages numpy setuptools wheel", "/work");

    // Detect if we need legacy setuptools (before removal of dep_util and package_index)
    auto grepDepUtil = session.getHost().exec("grep -r \"setuptools.dep_util\" .", "/work");
    auto grepPackageIndex = session.getHost().exec("grep -r \"setuptools.package_index\" .", "/work");
    bool needsLegacySetuptools = (grepDepUtil.exitCode == 0 || grepPackageIndex.exitCode == 0);

    std::string depCmd;
    if (repo.find("astropy") != std::string::npos) {
        if (needsLegacySetuptools) {
            logDebug("[SWEBench][VERBOSE] Detected legacy setuptools requirement (dep_util)");
            depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools<60\" \"wheel\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
        } else {
            depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools>=68\" \"wheel\" \"numpy>=2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
        }
    } else if (repo.find("django") != std::string::npos) {
        depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools<60\" \"wheel\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
    } else {
        depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools<70\" \"wheel\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
    }


    auto depRes = session.getHost().exec(depCmd, "/work");
    if (depRes.exitCode != 0) {
        logError("[SWEBench][ERROR] Dependency Installation Failed:\n", depRes.stderrData);
        throw std::runtime_error("Environment dependency installation failed");
    }

    // Surgical fix for pyproject.toml
    session.getHost().exec("sed -i 's/license = \"\\(.*\\)\"/license = {text = \"\\1\"}/g' pyproject.toml 2>/dev/null || true", "/work");
    session.getHost().exec("sed -i '/license-files/d' pyproject.toml 2>/dev/null || true", "/work");

    logDebug("[SWEBench][VERBOSE] Building extension modules...");
    session.emitLog("SWE-bench: building extension modules.");
    auto buildRes = session.getHost().exec("python3 setup.py build_ext --inplace", "/work");
    if (buildRes.exitCode != 0)
        logDebug("[SWEBench][DEBUG] Build Ext Warning (non-fatal):\n", buildRes.stderrData);

    logDebug("[SWEBench][VERBOSE] Installing repository in editable mode...");
    session.emitLog("SWE-bench: installing repository into the benchmark environment.");
    // Use --no-build-isolation to use our pinned versions
    auto installRes = session.getHost().exec("uv pip install --system --no-build-isolation -e .", "/work");
    if (installRes.exitCode != 0) {
        logDebug("[SWEBench][VERBOSE] Editable install not supported by this version, falling back to standard install...");
        session.getHost().exec("uv pip install --system --no-build-isolation .", "/work");
    }

    // VERIFY INSTALLATION

    std::string packageName = repo.substr(repo.find('/') + 1);
    // Heuristic: astropy package is 'astropy', django is 'django'
    if (packageName.find("django") != std::string::npos) packageName = "django";
    if (packageName.find("astropy") != std::string::npos) packageName = "astropy";

    auto verifyRes = session.getHost().exec("python3 -c \"import " + packageName + "; print('SUCCESS')\"", "/work");
    if (verifyRes.exitCode != 0 || verifyRes.stdoutData.find("SUCCESS") == std::string::npos) {
        logError("[SWEBench][ERROR] Environment Verification Failed. Package not importable:\n", verifyRes.stderrData);
        // Last ditch effort: install from source without editable
        session.emitLog("SWE-bench: editable install verification failed, retrying fallback installation.");
        session.getHost().exec("python3 setup.py install --user", "/work");
        verifyRes = session.getHost().exec("python3 -c \"import " + packageName + "; print('SUCCESS')\"", "/work");
        if (verifyRes.exitCode != 0) throw std::runtime_error("Package verification failed after install");
    }

    logInfo("\n\033[1;33m[SWEBench] RUNNING BASELINE TESTS (EXPECTED TO FAIL)\033[0m");
    session.emitLog("SWE-bench: running baseline failing tests before the worker starts.");
    std::map<std::string, std::string> testEnv = {{"PYTHONPATH", "/work"}};

    std::stringstream testCmdSS;
    bool isDjango = (repo == "django/django");
    if (isDjango) {
        testCmdSS << "python3 tests/runtests.py --settings=test_sqlite";
    } else {
        testCmdSS << "python3 -m pytest -v -p no:warnings";
    }

    for (const auto& t : failToPass) {
        testCmdSS << " '" << normalizeTestName(repo, t) << "'";
    }

    auto baselineRes = session.getHost().exec(testCmdSS.str(), "/work", testEnv);
    logDebug("[SWEBench][DEBUG] Baseline Stdout:\n", baselineRes.stdoutData);

    int passed = 0, failed = 0, errors = 0;
    parseTestResults(baselineRes.stdoutData, passed, failed, errors);
    if (passed == 0 && failed == 0 && errors == 0) parseTestResults(baselineRes.stderrData, passed, failed, errors);

    logInfo("[SWEBench] Baseline Results -> Passed: ", passed, ", Failed: ", failed,
           ", Errors: ", errors);
    session.emitLog("SWE-bench: baseline results passed=" + std::to_string(passed) +
                    ", failed=" + std::to_string(failed) +
                    ", errors=" + std::to_string(errors) + ".");

    return true;
}

BenchmarkResult SWEBench::runTask(const std::string& taskId) {
    ensureDatasetLoaded();
    const rapidjson::Value* row = nullptr;
    for (const auto& r : dataset["rows"].GetArray()) {
        if (std::string(r["row"]["instance_id"].GetString()) == taskId) {
            row = &r["row"];
            break;
        }
    }
    if (!row) throw std::runtime_error("Task not found");

    std::string repo = (*row)["repo"].GetString();
    std::string problemStatement = (*row)["problem_statement"].GetString();
    std::string failToPassStr = (*row)["FAIL_TO_PASS"].GetString();
    std::vector<std::string> failToPass = parseJsonArray(failToPassStr);

    logInfo("\n\033[1;34m[SWEBench] SUMMONING AGENT\033[0m");
    session.emitLog("SWE-bench: running worker on task " + taskId + ".");
    std::stringstream prompt;
    prompt << "Task: " << problemStatement << "\n\n"
           << "The repository has been cloned to /work and checked out at the base commit. "
           << "The relevant tests have been added to the codebase.\n"
           << "The following tests are currently FAILING. You must fix the code so they PASS:\n";
    for (const auto& t : failToPass) prompt << "- " << t << "\n";
    prompt << "\nFix the issue in the codebase. After finishing, provide a summary of your changes.";

    session.runAgentTask(prompt.str());

    logInfo("\n\033[1;32m[SWEBench] RUNNING FINAL EVALUATION\033[0m");
    session.emitLog("SWE-bench: running final evaluation.");
    std::stringstream testCmdSS;
    bool isDjango = (repo == "django/django");
    if (isDjango) {
        testCmdSS << "python3 tests/runtests.py --settings=test_sqlite";
    } else {
        testCmdSS << "python3 -m pytest -v -p no:warnings";
    }

    for (const auto& t : failToPass) {
        testCmdSS << " '" << normalizeTestName(repo, t) << "'";
    }

    auto finalRes = session.getHost().exec(testCmdSS.str(), "/work", {{"PYTHONPATH", "/work"}});
    int passed = 0, failed = 0, errors = 0;
    parseTestResults(finalRes.stdoutData, passed, failed, errors);
    if (passed == 0 && failed == 0 && errors == 0) parseTestResults(finalRes.stderrData, passed, failed, errors);

    logInfo("[SWEBench] Final Summary -> Passed: ", passed, ", Failed: ", failed,
           ", Errors: ", errors);
    session.emitLog("SWE-bench: final results passed=" + std::to_string(passed) +
                    ", failed=" + std::to_string(failed) +
                    ", errors=" + std::to_string(errors) + ".");

    BenchmarkResult result;
    result.taskId = taskId;
    result.passed = (failed == 0 && errors == 0 && passed >= (int)failToPass.size());
    result.metrics = session.getAgent().getContext().aggregateMetrics;
    result.output = "Final (Passed=" + std::to_string(passed) + ", Failed=" +
                    std::to_string(failed) + ")\nMetrics: " +
                    summarizeContextWindowMetrics(result.metrics.context, 4);
    return result;
}

void SWEBench::ensureDatasetLoaded() {
    if (datasetLoaded) return;
    std::string home = getenv("HOME") ? getenv("HOME") : "/root";
    std::string cacheFile = home + "/.firmius/cache/swebench_verified.json";

    if (!std::filesystem::exists(cacheFile)) {
        session.emitLog("SWE-bench: downloading SWE-bench Verified dataset cache.");
        std::filesystem::create_directories(std::filesystem::path(cacheFile).parent_path());
        CURL* curl = curl_easy_init();
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, "https://datasets-server.huggingface.co/rows?dataset=princeton-nlp/SWE-bench_Verified&config=default&split=test&offset=0&limit=50");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        std::ofstream out(cacheFile);
        out << response;
        out.close();
    } else {
        session.emitLog("SWE-bench: using cached dataset at " + cacheFile + ".");
    }

    std::ifstream in(cacheFile);
    std::stringstream buffer;
    buffer << in.rdbuf();
    dataset.Parse(buffer.str().c_str());
    datasetLoaded = true;
}

bool SWEBench::parseTestResults(const std::string& output, int& passed, int& failed, int& errors) {
    passed = 0; failed = 0; errors = 0;
    bool found = false;
    std::regex p_reg(R"((\d+) passed)"), f_reg(R"((\d+) failed)"), e_reg(R"((\d+) error)");
    std::smatch m;
    if (std::regex_search(output, m, p_reg)) { passed = std::stoi(m[1]); found = true; }
    if (std::regex_search(output, m, f_reg)) { failed = std::stoi(m[1]); found = true; }
    if (std::regex_search(output, m, e_reg)) { errors = std::stoi(m[1]); found = true; }
    if (!found) {
        size_t pos = 0;
        while ((pos = output.find("PASSED", pos)) != std::string::npos) { passed++; pos += 6; found = true; }
        pos = 0;
        while ((pos = output.find("FAILED", pos)) != std::string::npos) { failed++; pos += 6; found = true; }
        pos = 0;
        while ((pos = output.find("ERROR", pos)) != std::string::npos) { errors++; pos += 5; found = true; }
    }
    return found;
}

}
