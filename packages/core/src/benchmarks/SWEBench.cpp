#include "benchmarks/SWEBench.hpp"
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

    std::string cacheBase = "/root/.firmius/cache/repos/";
    std::string repoCacheDir = cacheBase + repo;
    
    host.exec("mkdir -p " + cacheBase);
    
    auto checkRes = host.exec("test -d " + repoCacheDir);
    if (checkRes.exitCode != 0) {
        std::cout << "[SWEBench][VERBOSE] Initial clone into container cache: " << repo << "..." << std::endl;
        host.exec("git clone https://github.com/" + repo + ".git " + repoCacheDir);
    }

    std::cout << "[SWEBench][VERBOSE] Resetting /work and copying from cache..." << std::endl;
    host.exec("rm -rf /work/* /work/.* 2>/dev/null || true");
    host.exec("cp -a " + repoCacheDir + "/. /work/");

    std::cout << "[SWEBench][VERBOSE] Checking out " << baseCommit << "..." << std::endl;
    auto checkoutRes = host.exec("git checkout " + baseCommit, "/work");
    if (checkoutRes.exitCode != 0) {
        host.exec("cd " + repoCacheDir + " && git fetch origin");
        host.exec("cp -a " + repoCacheDir + "/. /work/");
        host.exec("git checkout " + baseCommit, "/work");
    }

    std::cout << "[SWEBench][VERBOSE] Applying test patch..." << std::endl;
    host.writeFile("/work/test.patch", std::vector<uint8_t>(testPatch.begin(), testPatch.end()));
    auto applyRes = host.exec("git apply /work/test.patch", "/work");
    if (applyRes.exitCode != 0) {
        host.exec("patch -p1 < /work/test.patch", "/work");
    }

    std::cout << "[SWEBench][VERBOSE] Installing environment dependencies with pip3..." << std::endl;
    std::string depCmd = "pip3 install --break-system-packages --no-cache-dir \"setuptools<60\" \"numpy<2.0\" cython setuptools_scm pyerfa hypothesis pytest-astropy extension-helpers pytest-mock pyyaml";
    host.exec(depCmd, "/work");
    
    std::cout << "[SWEBench][VERBOSE] Building extension modules..." << std::endl;
    host.exec("python3 setup.py build_ext --inplace", "/work");

    std::cout << "[SWEBench][VERBOSE] Installing repository in editable mode..." << std::endl;
    host.exec("pip3 install --break-system-packages --no-build-isolation -e .", "/work");

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
    
    return (failed > 0 || errors > 0);
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
