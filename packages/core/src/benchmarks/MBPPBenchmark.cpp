#include "benchmarks/MBPPBenchmark.hpp"
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace firmius::core {
using namespace firmius::shared;

namespace {
size_t writeToFile(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}
}

MBPPBenchmark::MBPPBenchmark(BenchmarkConfig config) : session(std::move(config)) {}

std::vector<std::string> MBPPBenchmark::listTasks() {
    session.emitLog("MBPP: loading dataset manifest.");
    ensureDatasetLoaded();
    std::vector<std::string> tasks;
    if (dataset.IsArray()) {
        for (const auto& item : dataset.GetArray()) {
            tasks.push_back(std::to_string(item["task_id"].GetInt()));
        }
    }
    return tasks;
}

bool MBPPBenchmark::prepareTask(const std::string&) {
    ensureDatasetLoaded();
    
    // Clean /work before each task
    session.emitLog("MBPP: resetting /work for the selected task.");
    session.getHost().exec("rm -rf /work/* /work/.* 2>/dev/null || true");
    
    return true;
}

BenchmarkResult MBPPBenchmark::runTask(const std::string& taskId) {
    ensureDatasetLoaded();
    
    const rapidjson::Value* task = nullptr;
    for (const auto& item : dataset.GetArray()) {
        if (std::to_string(item["task_id"].GetInt()) == taskId) {
            task = &item;
            break;
        }
    }

    if (!task) throw std::runtime_error("Task not found: " + taskId);

    std::string prompt = (*task)["prompt"].GetString();
    std::string testList;
    for (const auto& t : (*task)["test_list"].GetArray()) {
        testList += t.GetString();
        testList += "\n";
    }
    std::string testImports;
    for (const auto& t : (*task)["test_imports"].GetArray()) {
        testImports += t.GetString();
        testImports += "\n";
    }

    std::stringstream fullPrompt;
    fullPrompt << "Task: " << prompt << "\n\n"
               << "Your code must pass these tests:\n" << testList << "\n"
               << "Write the Python solution to /work/solution.py. Do not include tests in /work/solution.py.\n"
               << "Tests will be run against your /work/solution.py. Use the file_edit tool to write the solution.";
    
    BenchmarkResult result;
    result.taskId = taskId;

    session.emitLog("MBPP: running worker on task " + taskId + ".");
    session.runAgentTask(fullPrompt.str());

    // Evaluate
    session.emitLog("MBPP: evaluating generated solution.");
    std::stringstream ss;
    ss << "import json\n"
       << "import os\n"
       << "import sys\n"
       << "sys.path.append('/work')\n"
       << "os.chdir('/work')\n"
       << "passed = 0\n"
       << "total = 0\n"
       << "error_msg = ''\n"
       << "try:\n"
       << "    from solution import *\n"
       << "    test_lines = \"\"\"" << testList << "\"\"\"\n"
       << "    test_imports = \"\"\"" << testImports << "\"\"\"\n"
       << "    exec(test_imports)\n"
       << "    test_lines = [l.strip() for l in test_lines.split('\\n') if l.strip()]\n"
       << "    total = len(test_lines)\n"
       << "    for t in test_lines:\n"
       << "        try:\n"
       << "            exec(t)\n"
       << "            passed += 1\n"
       << "        except Exception as e:\n"
       << "            error_msg += f'Test failed: {t} -> {e}\\n'\n"
       << "except Exception as e:\n"
       << "    error_msg = f'Import error: {e}'\n"
       << "print(json.dumps({'passed': passed, 'total': total, 'error': error_msg}))\n";

    std::string testScript = ss.str();
    session.getHost().writeFile("/work/run_tests.py", std::vector<uint8_t>(testScript.begin(), testScript.end()));
    auto execRes = session.getHost().exec("python3 /work/run_tests.py", "/work");
    
    rapidjson::Document resDoc;
    resDoc.Parse(execRes.stdoutData.c_str());
    if (!resDoc.HasParseError() && resDoc.IsObject() && resDoc.HasMember("passed")) {
        int passed = resDoc["passed"].GetInt();
        int total = resDoc["total"].GetInt();
        result.passed = (passed == total && total > 0);
        result.output = "Passed: " + std::to_string(passed) + "/" + std::to_string(total);
        if (!result.passed) result.output += "\nErrors:\n" + std::string(resDoc["error"].GetString());
    } else {
        result.passed = false;
        result.output = "Evaluation failed. Output: " + execRes.stdoutData + "\nStderr: " + execRes.stderrData;
    }

    session.emitLog("MBPP: finished evaluation for task " + taskId + " with result " +
                    std::string(result.passed ? "PASS" : "FAIL") + ".");

    return result;
}

void MBPPBenchmark::ensureDatasetLoaded() {
    if (datasetLoaded) return;

    std::string home = getenv("HOME") ? getenv("HOME") : "/root";
    std::string cacheDir = home + "/.firmius/cache/mbpp";
    std::string cacheFile = cacheDir + "/sanitized-mbpp.json";
    
    if (!std::filesystem::exists(cacheFile)) {
        session.emitLog("MBPP: downloading dataset cache from Google Research.");
        std::filesystem::create_directories(cacheDir);
        CURL* curl = curl_easy_init();
        std::ofstream out(cacheFile, std::ios::binary);
        curl_easy_setopt(curl, CURLOPT_URL, "https://raw.githubusercontent.com/google-research/google-research/refs/heads/master/mbpp/sanitized-mbpp.json");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        out.close();
    } else {
        session.emitLog("MBPP: using cached dataset at " + cacheFile + ".");
    }

    std::ifstream in(cacheFile);
    std::stringstream buffer;
    buffer << in.rdbuf();
    dataset.Parse(buffer.str().c_str());
    datasetLoaded = true;
}

}
