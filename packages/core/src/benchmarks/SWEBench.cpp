#include "benchmarks/SWEBench.hpp"
#include <cstdlib>
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <regex>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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
}

SWEBench::SWEBench(Agent& a, shared::IHost& h) : agent(a), host(h) {}

std::vector<std::string> SWEBench::listTasks() {
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

    std::cout << "\033[1;32m[SWEBench] PREPARING ENVIRONMENT for " << taskId << "\033[0m" << std::endl;

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
        std::cout << "[SWEBench][VERBOSE] Initial clone on host: " << repo << "..." << std::endl;
        std::filesystem::create_directories(hostCacheBase);
        std::string cloneCmd = "git clone https://github.com/" + repo + ".git " + repoCacheDir;
        int res = std::system(cloneCmd.c_str());
        if (res != 0) throw std::runtime_error("Failed to clone repository on host: " + repo);
    } else {
        // Check if repo is shallow (incomplete history)
        std::string shallowFile = repoCacheDir + "/.git/shallow";
        bool isShallow = std::filesystem::exists(shallowFile);
        
        if (isShallow) {
            std::cout << "[SWEBench][VERBOSE] Repo is shallow, fetching full history..." << std::endl;
            std::string fetchCmd = "cd " + repoCacheDir + " && git fetch --unshallow && git fetch --tags origin";
            std::system(fetchCmd.c_str());
        } else {
            // Ensure the commit exists in host cache
            std::string checkCmd = "cd " + repoCacheDir + " && git rev-parse --verify " + baseCommit + " >/dev/null 2>&1";
            if (std::system(checkCmd.c_str()) != 0) {
                std::cout << "[SWEBench][VERBOSE] Fetching missing commit " << baseCommit << " on host..." << std::endl;
                // Fetch everything to be safe if a specific commit fetch fails
                std::string fetchCmd = "cd " + repoCacheDir + " && git fetch origin && git fetch --tags origin";
                std::system(fetchCmd.c_str());
            }
        }
    }

    std::cout << "[SWEBench][VERBOSE] Resetting /work and copying from host cache..." << std::endl;
    host.exec("rm -rf /work/* /work/.* 2>/dev/null || true");
    host.exec("mkdir -p /work");

    if (host.getId() != "localhost") {
        // Use docker cp for robust directory transfer
        std::string transferCmd = "docker cp " + repoCacheDir + "/. " + host.getId() + ":/work/";
        int res = std::system(transferCmd.c_str());
        if (res != 0) throw std::runtime_error("Failed to transfer repository to container using docker cp");
        
        auto gitCheck = host.exec("ls -d /work/.git");
        if (gitCheck.exitCode != 0) {
            std::cout << "[SWEBench][ERROR] .git directory not found in /work after docker cp!" << std::endl;
        }
    } else {
        host.exec("cp -a " + repoCacheDir + "/. /work/");
    }

    std::cout << "[SWEBench][VERBOSE] Checking out " << baseCommit << "..." << std::endl;
    host.exec("git config --global --add safe.directory /work", "/work");
    auto checkoutRes = host.exec("git checkout -f " + baseCommit, "/work");
    if (checkoutRes.exitCode != 0) {
        std::cout << "[SWEBench][DEBUG] Checkout failed. The commit might be missing from the cache." << std::endl;
        // Do not try to fetch in the container if internet is disabled.
        // We already tried to fetch on the host.
        throw std::runtime_error("Git checkout failed in container: " + checkoutRes.stderrData);
    }
    
    auto logRes = host.exec("git log -1 --format=%H", "/work");
    std::cout << "[SWEBench][DEBUG] Current Commit in Container: " << logRes.stdoutData << std::endl;
    if (logRes.stdoutData.find(baseCommit) == std::string::npos && baseCommit.find(logRes.stdoutData.substr(0, 7)) == std::string::npos) {
        std::cout << "[SWEBench][ERROR] Checkout failed to reach target commit!" << std::endl;
    }

    std::cout << "[SWEBench][VERBOSE] Applying test patch..." << std::endl;
    host.writeFile("/work/test.patch", std::vector<uint8_t>(testPatch.begin(), testPatch.end()));
    auto applyRes = host.exec("git apply /work/test.patch", "/work");
    if (applyRes.exitCode != 0) {
        host.exec("patch -p1 < /work/test.patch", "/work");
    }

    // Fix for Python 3.10+ compatibility: collections.Mapping -> collections.abc.Mapping
    // This affects astropy's vendored configobj
    std::cout << "[SWEBench][VERBOSE] Applying Python 3.10+ compatibility patches..." << std::endl;
    host.exec("find . -name '*.py' -exec sed -i 's/collections\\.Mapping/collections.abc.Mapping/g' {} \\; 2>/dev/null || true", "/work");
    host.exec("find . -name '*.py' -exec sed -i 's/collections\\.Iterable/collections.abc.Iterable/g' {} \\; 2>/dev/null || true", "/work");
    host.exec("find . -name '*.py' -exec sed -i 's/collections\\.Callable/collections.abc.Callable/g' {} \\; 2>/dev/null || true", "/work");
    // Additional patches for Python 3.10+ (removed in Python 3.11)
    host.exec("find . -name '*.py' -exec sed -i 's/collections\\.MutableSequence/collections.abc.MutableSequence/g' {} \\; 2>/dev/null || true", "/work");
    host.exec("find . -name '*.py' -exec sed -i 's/collections\\.MutableMapping/collections.abc.MutableMapping/g' {} \\; 2>/dev/null || true", "/work");
    host.exec("find . -name '*.py' -exec sed -i 's/collections\\.Sequence/collections.abc.Sequence/g' {} \\; 2>/dev/null || true", "/work");

    std::cout << "[SWEBench][VERBOSE] Installing environment dependencies with uv..." << std::endl;
    
    // Force clean to ensure no version conflicts
    host.exec("uv pip uninstall --system --break-system-packages numpy setuptools wheel", "/work");

    // Detect if we need legacy setuptools (before removal of dep_util and package_index)
    auto grepDepUtil = host.exec("grep -r \"setuptools.dep_util\" .", "/work");
    auto grepPackageIndex = host.exec("grep -r \"setuptools.package_index\" .", "/work");
    bool needsLegacySetuptools = (grepDepUtil.exitCode == 0 || grepPackageIndex.exitCode == 0);

    std::string depCmd;
    if (repo.find("astropy") != std::string::npos) {
        if (needsLegacySetuptools) {
            std::cout << "[SWEBench][VERBOSE] Detected legacy setuptools requirement (dep_util)" << std::endl;
            depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools<60\" \"wheel\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
        } else {
            depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools>=68\" \"wheel\" \"numpy>=2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
        }
    } else if (repo.find("django") != std::string::npos) {
        depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools<60\" \"wheel\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
    } else {
        depCmd = "uv pip install --system --break-system-packages --no-cache \"setuptools<70\" \"wheel\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
    }


    auto depRes = host.exec(depCmd, "/work");
    if (depRes.exitCode != 0) {
        std::cout << "[SWEBench][ERROR] Dependency Installation Failed:\n" << depRes.stderrData << std::endl;
        throw std::runtime_error("Environment dependency installation failed");
    }

    // Surgical fix for pyproject.toml
    host.exec("sed -i 's/license = \"\\(.*\\)\"/license = {text = \"\\1\"}/g' pyproject.toml 2>/dev/null || true", "/work");
    host.exec("sed -i '/license-files/d' pyproject.toml 2>/dev/null || true", "/work");

    std::cout << "[SWEBench][VERBOSE] Building extension modules..." << std::endl;
    auto buildRes = host.exec("python3 setup.py build_ext --inplace", "/work");
    if (buildRes.exitCode != 0) std::cout << "[SWEBench][DEBUG] Build Ext Warning (non-fatal):\n" << buildRes.stderrData << std::endl;

    std::cout << "[SWEBench][VERBOSE] Installing repository in editable mode..." << std::endl;
    // Use --no-build-isolation to use our pinned versions
    auto installRes = host.exec("uv pip install --system --no-build-isolation -e .", "/work");
    if (installRes.exitCode != 0) {
        std::cout << "[SWEBench][VERBOSE] Editable install not supported by this version, falling back to standard install..." << std::endl;
        host.exec("uv pip install --system --no-build-isolation .", "/work");
    }

    // VERIFY INSTALLATION

    std::string packageName = repo.substr(repo.find('/') + 1);
    // Heuristic: astropy package is 'astropy', django is 'django'
    if (packageName.find("django") != std::string::npos) packageName = "django";
    if (packageName.find("astropy") != std::string::npos) packageName = "astropy";

    auto verifyRes = host.exec("python3 -c \"import " + packageName + "; print('SUCCESS')\"", "/work");
    if (verifyRes.exitCode != 0 || verifyRes.stdoutData.find("SUCCESS") == std::string::npos) {
        std::cout << "[SWEBench][ERROR] Environment Verification Failed. Package not importable:\n" << verifyRes.stderrData << std::endl;
        // Last ditch effort: install from source without editable
        host.exec("python3 setup.py install --user", "/work");
        verifyRes = host.exec("python3 -c \"import " + packageName + "; print('SUCCESS')\"", "/work");
        if (verifyRes.exitCode != 0) throw std::runtime_error("Package verification failed after install");
    }

    std::cout << "\n\033[1;33m[SWEBench] RUNNING BASELINE TESTS (EXPECTED TO FAIL)\033[0m" << std::endl;
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

    auto baselineRes = host.exec(testCmdSS.str(), "/work", testEnv);
    std::cout << "[SWEBench][DEBUG] Baseline Stdout:\n" << baselineRes.stdoutData << std::endl;

    int passed = 0, failed = 0, errors = 0;
    parseTestResults(baselineRes.stdoutData, passed, failed, errors);
    if (passed == 0 && failed == 0 && errors == 0) parseTestResults(baselineRes.stderrData, passed, failed, errors);

    std::cout << "[SWEBench] Baseline Results -> Passed: " << passed << ", Failed: " << failed << ", Errors: " << errors << std::endl;

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

    std::cout << "\n\033[1;34m[SWEBench] SUMMONING AGENT\033[0m" << std::endl;
    std::stringstream prompt;
    prompt << "Task: " << problemStatement << "\n\n"
           << "The repository has been cloned to /work and checked out at the base commit. "
           << "The relevant tests have been added to the codebase.\n"
           << "The following tests are currently FAILING. You must fix the code so they PASS:\n";
    for (const auto& t : failToPass) prompt << "- " << t << "\n";
    prompt << "\nFix the issue in the codebase. When done, provide a summary and end with <done />.";

    agent.reset();
    agent.run(prompt.str(), [](const StreamEvent&) {});

    std::cout << "\n\033[1;32m[SWEBench] RUNNING FINAL EVALUATION\033[0m" << std::endl;
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

    auto finalRes = host.exec(testCmdSS.str(), "/work", {{"PYTHONPATH", "/work"}});
    int passed = 0, failed = 0, errors = 0;
    parseTestResults(finalRes.stdoutData, passed, failed, errors);
    if (passed == 0 && failed == 0 && errors == 0) parseTestResults(finalRes.stderrData, passed, failed, errors);

    std::cout << "[SWEBench] Final Summary -> Passed: " << passed << ", Failed: " << failed << ", Errors: " << errors << std::endl;

    BenchmarkResult result;
    result.taskId = taskId;
    result.passed = (failed == 0 && errors == 0 && passed >= (int)failToPass.size());
    result.output = "Final (Passed=" + std::to_string(passed) + ", Failed=" + std::to_string(failed) + ")";
    return result;
}

void SWEBench::ensureDatasetLoaded() {
    if (datasetLoaded) return;
    std::string home = getenv("HOME") ? getenv("HOME") : "/root";
    std::string cacheFile = home + "/.firmius/cache/swebench_verified.json";

    if (!std::filesystem::exists(cacheFile)) {
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
