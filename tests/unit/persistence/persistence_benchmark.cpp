#include "persistence/ThreadManager.hpp"
#include "persistence/Journaler.hpp"
#include "Context.hpp"
#include "Message.hpp"
#include "Enums.hpp"

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

ThreadMetadata makeMetadata() {
    ThreadMetadata metadata;
    metadata.title = "Benchmark Thread";
    metadata.hostOptions.type = HostType::Local;
    metadata.hostIdentifier = "bench-host";
    metadata.cwd = "/tmp/bench";
    metadata.leadPersona = "lead";
    return metadata;
}

AgentTurn makeTurn(const std::string& id) {
    AgentTurn turn;
    turn.turnId = id;
    turn.stopReason = StopReason::Stop;
    Message msg;
    msg.id = "msg-" + id;
    msg.role = Role::Assistant;
    msg.timestamp = 100;
    msg.content.push_back(TextContent{"hello-" + id});
    turn.messages.push_back(msg);
    turn.metrics.tokens.prompt = 10;
    turn.metrics.tokens.completion = 5;
    turn.metrics.estimatedCostUsd = 0.01;
    return turn;
}

Plan makePlan(const std::string& threadId, int i) {
    Plan plan;
    plan.id = "plan-" + std::to_string(i);
    plan.threadId = threadId;
    plan.title = "Plan " + std::to_string(i);
    plan.objective = "benchmark";
    plan.context = "persistence";
    plan.strategy = "normalize";
    plan.status = PlanStatus::Active;
    plan.notes = "benchmark";

    WorkChunk chunk;
    chunk.id = "chunk-" + std::to_string(i);
    chunk.title = "Chunk";
    chunk.goal = "Goal";
    chunk.context = "Context";
    chunk.constraints = "Constraints";
    chunk.completion = "Completion";
    chunk.status = WorkChunkStatus::Ready;
    chunk.filesToRead = {"a.cpp", "b.cpp"};
    chunk.filesToTouch = {"c.cpp"};
    plan.chunks.push_back(chunk);
    return plan;
}

AgentTodoList makeTodo(const std::string& threadId, const std::string& agentId) {
    AgentTodoList todo;
    todo.threadId = threadId;
    todo.agentId = agentId;
    todo.nextId = 3;
    todo.items.push_back(TodoItem{1, "inspect", TodoStatus::InProgress, "chunk", "plan", 1, 2});
    todo.items.push_back(TodoItem{2, "verify", TodoStatus::Pending, "chunk", "plan", 2, 2});
    return todo;
}

} // namespace

int main() {
    char tempTemplate[] = "/tmp/firmius_persistence_bench_XXXXXX";
    char* result = mkdtemp(tempTemplate);
    if (!result) {
        std::cerr << "mkdtemp failed\n";
        return 1;
    }
    const std::string tempDir = result;
    setenv("HOME", tempDir.c_str(), 1);
    std::filesystem::create_directories(tempDir + "/.firmius/threads");

    ThreadManager tm(tempDir + "/.firmius/threads");
    const std::string threadId = tm.createThread(makeMetadata());
    const std::string agentId = "bench-agent";

    auto start = std::chrono::steady_clock::now();
    {
        Journaler journaler(threadId, agentId);
        for (int i = 0; i < 500; ++i) {
            journaler.appendTurn(makeTurn("turn-" + std::to_string(i)));
        }
    }
    auto afterJournal = std::chrono::steady_clock::now();

    for (int i = 0; i < 100; ++i) {
        tm.writePlan(threadId, makePlan(threadId, i));
    }
    auto afterPlans = std::chrono::steady_clock::now();

    for (int i = 0; i < 100; ++i) {
        tm.writeAgentTodo(threadId, agentId + "-" + std::to_string(i), makeTodo(threadId, agentId + "-" + std::to_string(i)));
    }
    auto afterTodos = std::chrono::steady_clock::now();

    auto history = tm.loadAgentHistory(threadId, agentId);
    auto plans = tm.listPlans(threadId);
    auto todo = tm.getAgentTodo(threadId, agentId + "-42");
    auto afterReads = std::chrono::steady_clock::now();

    auto journalMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterJournal - start).count();
    auto plansMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterPlans - afterJournal).count();
    auto todosMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterTodos - afterPlans).count();
    auto readsMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterReads - afterTodos).count();

    std::cout << "Persistence benchmark results\n";
    std::cout << "  journal_append_500_ms=" << journalMs << "\n";
    std::cout << "  write_plans_100_ms=" << plansMs << "\n";
    std::cout << "  write_todos_100_ms=" << todosMs << "\n";
    std::cout << "  readback_ms=" << readsMs << "\n";
    std::cout << "  history_turns=" << history.turns.size() << "\n";
    std::cout << "  plans_count=" << plans.size() << "\n";
    std::cout << "  todo_items=" << todo.items.size() << "\n";

    std::filesystem::remove_all(tempDir);
    return 0;
}
