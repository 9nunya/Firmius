#include "benchmarks/AgentBench.hpp"
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

namespace {
size_t writeToFile(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}
}

AgentBench::AgentBench(Agent& a, shared::IHost& h) : agent(a), host(h) {}

std::vector<std::string> AgentBench::listTasks() {
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
    
    int index = std::stoi(taskId);
    if (index < 0 || index >= (int)dataset.Size()) throw std::runtime_error("Task index out of range");

    const auto& task = dataset[index];
    
    // Reset host
    host.exec("rm -rf /work/* /work/.* 2>/dev/null || true");

    // Setup
    if (task.HasMember("create") && task["create"].HasMember("init") && task["create"]["init"].HasMember("code")) {
        std::string initCode = task["create"]["init"]["code"].GetString();
        host.exec(initCode);
    }
    
    return true;
}

BenchmarkResult AgentBench::runTask(const std::string& taskId) {
    ensureDatasetLoaded();
    
    int index = std::stoi(taskId);
    const auto& task = dataset[index];
    std::string description = task["description"].GetString();
    
    BenchmarkResult result;
    result.taskId = taskId;

    std::string fullPrompt = "Task: " + description + "\n\nWhen done, provide your final answer and end with <done />. Do not call tools after you have found the answer.";
    
    agent.reset();
    agent.run(fullPrompt, [](const StreamEvent&) {});

    // Evaluation
    if (task.HasMember("evaluation") && task["evaluation"].HasMember("match")) {
        std::string expected = task["evaluation"]["match"].GetString();
        // Check last message from agent
        const auto& turns = agent.getContext().history.turns;
        if (!turns.empty()) {
            const auto& lastTurn = turns.back();
            if (!lastTurn.messages.empty()) {
                const auto& lastMsg = lastTurn.messages.back();
                std::string actual;
                for (const auto& part : lastMsg.content) {
                    if (auto* txt = std::get_if<TextContent>(&part)) actual += txt->text;
                }
                
                result.passed = (actual.find(expected) != std::string::npos);
                result.output = "Expected: " + expected + "\nActual contains it: " + (result.passed ? "Yes" : "No");
            }
        }
    }

    return result;
}

void AgentBench::ensureDatasetLoaded() {
    if (datasetLoaded) return;

    std::string home = getenv("HOME") ? getenv("HOME") : "/root";
    std::string cacheDir = home + "/.firmius/cache/agentbench";
    std::string cacheFile = cacheDir + "/os_interaction_dev.json";
    
    if (!std::filesystem::exists(cacheFile)) {
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
    }

    std::ifstream in(cacheFile);
    std::stringstream buffer;
    buffer << in.rdbuf();
    dataset.Parse(buffer.str().c_str());
    datasetLoaded = true;
}

}
