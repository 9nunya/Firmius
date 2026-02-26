#include "Serialization.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>

namespace firmius::shared {

namespace {

// Enum conversion helpers
std::string roleToString(Role value) {
    switch (value) {
        case Role::System: return "System";
        case Role::User: return "User";
        case Role::Assistant: return "Assistant";
        case Role::ToolResult: return "ToolResult";
    }
    return "Unknown";
}

Role stringToRole(const std::string& str) {
    if (str == "System") return Role::System;
    if (str == "User") return Role::User;
    if (str == "Assistant") return Role::Assistant;
    if (str == "ToolResult") return Role::ToolResult;
    throw std::runtime_error("Unknown Role: " + str);
}

std::string hostTypeToString(HostType value) {
    switch (value) {
        case HostType::Local: return "Local";
        case HostType::Docker: return "Docker";
        case HostType::RemoteSSH: return "RemoteSSH";
    }
    return "Unknown";
}

HostType stringToHostType(const std::string& str) {
    if (str == "Local") return HostType::Local;
    if (str == "Docker") return HostType::Docker;
    if (str == "RemoteSSH") return HostType::RemoteSSH;
    throw std::runtime_error("Unknown HostType: " + str);
}

std::string toolScopeToString(ToolScope value) {
    switch (value) {
        case ToolScope::FilesystemRead: return "FilesystemRead";
        case ToolScope::FilesystemWrite: return "FilesystemWrite";
        case ToolScope::Process: return "Process";
        case ToolScope::Semantic: return "Semantic";
        case ToolScope::Delegation: return "Delegation";
        case ToolScope::Web: return "Web";
        case ToolScope::Git: return "Git";
    }
    return "Unknown";
}

ToolScope stringToToolScope(const std::string& str) {
    if (str == "FilesystemRead") return ToolScope::FilesystemRead;
    if (str == "FilesystemWrite") return ToolScope::FilesystemWrite;
    if (str == "Process") return ToolScope::Process;
    if (str == "Semantic") return ToolScope::Semantic;
    if (str == "Delegation") return ToolScope::Delegation;
    if (str == "Web") return ToolScope::Web;
    if (str == "Git") return ToolScope::Git;
    throw std::runtime_error("Unknown ToolScope: " + str);
}

std::string agentStatusToString(AgentStatus value) {
    switch (value) {
        case AgentStatus::Idle: return "Idle";
        case AgentStatus::Streaming: return "Streaming";
        case AgentStatus::ExecutingTool: return "ExecutingTool";
        case AgentStatus::AwaitingInput: return "AwaitingInput";
        case AgentStatus::Error: return "Error";
    }
    return "Unknown";
}

AgentStatus stringToAgentStatus(const std::string& str) {
    if (str == "Idle") return AgentStatus::Idle;
    if (str == "Streaming") return AgentStatus::Streaming;
    if (str == "ExecutingTool") return AgentStatus::ExecutingTool;
    if (str == "AwaitingInput") return AgentStatus::AwaitingInput;
    if (str == "Error") return AgentStatus::Error;
    throw std::runtime_error("Unknown AgentStatus: " + str);
}

// Struct conversion helpers
rapidjson::Value tokenMetricsToJson(const TokenMetrics& m, rapidjson::Document::AllocatorType& a) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("prompt", m.prompt, a);
    v.AddMember("completion", m.completion, a);
    v.AddMember("reasoning", m.reasoning, a);
    v.AddMember("total", m.total, a);
    return v;
}

TokenMetrics tokenMetricsFromJson(const rapidjson::Value& v) {
    return { v["prompt"].GetUint(), v["completion"].GetUint(), v["reasoning"].GetUint(), v["total"].GetUint() };
}

rapidjson::Value timingMetricsToJson(const TimingMetrics& m, rapidjson::Document::AllocatorType& a) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("startMs", m.startMs, a);
    v.AddMember("firstTokenMs", m.firstTokenMs, a);
    v.AddMember("endMs", m.endMs, a);
    v.AddMember("toolExecutionMs", m.toolExecutionMs, a);
    return v;
}

TimingMetrics timingMetricsFromJson(const rapidjson::Value& v) {
    return { v["startMs"].GetUint64(), v["firstTokenMs"].GetUint64(), v["endMs"].GetUint64(), v["toolExecutionMs"].GetUint64() };
}

rapidjson::Value agentMetricsToJson(const AgentMetrics& m, rapidjson::Document::AllocatorType& a) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("tokens", tokenMetricsToJson(m.tokens, a), a);
    v.AddMember("timing", timingMetricsToJson(m.timing, a), a);
    v.AddMember("estimatedCostUsd", m.estimatedCostUsd, a);
    return v;
}

AgentMetrics agentMetricsFromJson(const rapidjson::Value& v) {
    return { tokenMetricsFromJson(v["tokens"]), timingMetricsFromJson(v["timing"]), v["estimatedCostUsd"].GetDouble() };
}

rapidjson::Value messagePartToJson(const MessagePart& p, rapidjson::Document::AllocatorType& a) {
    rapidjson::Value v(rapidjson::kObjectType);
    if (auto* txt = std::get_if<TextContent>(&p)) {
        v.AddMember("type", "text", a);
        v.AddMember("text", rapidjson::Value(txt->text.c_str(), a), a);
    } else if (auto* thk = std::get_if<ThinkingContent>(&p)) {
        v.AddMember("type", "thinking", a);
        v.AddMember("thinking", rapidjson::Value(thk->thinking.c_str(), a), a);
    } else if (auto* tcc = std::get_if<ToolCallContent>(&p)) {
        v.AddMember("type", "toolCall", a);
        v.AddMember("id", rapidjson::Value(tcc->id.c_str(), a), a);
        v.AddMember("name", rapidjson::Value(tcc->name.c_str(), a), a);
        v.AddMember("args", rapidjson::Value(tcc->args.c_str(), a), a);
    } else if (auto* trc = std::get_if<ToolResultContent>(&p)) {
        v.AddMember("type", "toolResult", a);
        v.AddMember("toolCallId", rapidjson::Value(trc->toolCallId.c_str(), a), a);
        v.AddMember("result", rapidjson::Value(trc->result.c_str(), a), a);
        v.AddMember("success", trc->success, a);
    }
    return v;
}

MessagePart messagePartFromJson(const rapidjson::Value& v) {
    std::string type = v["type"].GetString();
    if (type == "text") return TextContent{ v["text"].GetString() };
    if (type == "thinking") return ThinkingContent{ v["thinking"].GetString() };
    if (type == "toolCall") return ToolCallContent{ v["id"].GetString(), v["name"].GetString(), v["args"].GetString() };
    if (type == "toolResult") return ToolResultContent{ v["toolCallId"].GetString(), v["result"].GetString(), v["success"].GetBool() };
    throw std::runtime_error("Unknown MessagePart type: " + type);
}

rapidjson::Value messageToJson(const Message& m, rapidjson::Document::AllocatorType& a) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("id", rapidjson::Value(m.id.c_str(), a), a);
    v.AddMember("role", rapidjson::Value(roleToString(m.role).c_str(), a), a);
    rapidjson::Value content(rapidjson::kArrayType);
    for (const auto& p : m.content) content.PushBack(messagePartToJson(p, a), a);
    v.AddMember("content", content, a);
    v.AddMember("timestamp", m.timestamp, a);
    if (m.parentId) v.AddMember("parentId", rapidjson::Value(m.parentId->c_str(), a), a);
    else v.AddMember("parentId", rapidjson::Value(rapidjson::kNullType), a);
    return v;
}

Message messageFromJson(const rapidjson::Value& v) {
    Message m;
    m.id = v["id"].GetString();
    m.role = stringToRole(v["role"].GetString());
    for (const auto& p : v["content"].GetArray()) m.content.push_back(messagePartFromJson(p));
    m.timestamp = v["timestamp"].GetUint64();
    if (v["parentId"].IsString()) m.parentId = v["parentId"].GetString();
    return m;
}

} // namespace

rapidjson::Document toJson(const AgentContext& ctx) {
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();

    rapidjson::Value identity(rapidjson::kObjectType);
    identity.AddMember("id", rapidjson::Value(ctx.identity.id.c_str(), a), a);
    identity.AddMember("name", rapidjson::Value(ctx.identity.name.c_str(), a), a);
    identity.AddMember("role", rapidjson::Value(ctx.identity.role.c_str(), a), a);
    identity.AddMember("goal", rapidjson::Value(ctx.identity.goal.c_str(), a), a);
    identity.AddMember("systemPrompt", rapidjson::Value(ctx.identity.systemPrompt.c_str(), a), a);
    d.AddMember("identity", identity, a);

    rapidjson::Value permissions(rapidjson::kObjectType);
    rapidjson::Value scopes(rapidjson::kArrayType);
    for (auto s : ctx.permissions.allowedScopes) scopes.PushBack(rapidjson::Value(toolScopeToString(s).c_str(), a), a);
    permissions.AddMember("allowedScopes", scopes, a);
    rapidjson::Value paths(rapidjson::kArrayType);
    for (const auto& p : ctx.permissions.allowedPaths) paths.PushBack(rapidjson::Value(p.c_str(), a), a);
    permissions.AddMember("allowedPaths", paths, a);
    permissions.AddMember("allowOutsideCwd", ctx.permissions.allowOutsideCwd, a);
    d.AddMember("permissions", permissions, a);

    rapidjson::Value env(rapidjson::kObjectType);
    env.AddMember("type", rapidjson::Value(hostTypeToString(ctx.environment.type).c_str(), a), a);
    env.AddMember("identifier", rapidjson::Value(ctx.environment.identifier.c_str(), a), a);
    env.AddMember("cwd", rapidjson::Value(ctx.environment.cwd.c_str(), a), a);
    rapidjson::Value envVars(rapidjson::kObjectType);
    for (const auto& [k, v] : ctx.environment.envVars) envVars.AddMember(rapidjson::Value(k.c_str(), a), rapidjson::Value(v.c_str(), a), a);
    env.AddMember("envVars", envVars, a);
    d.AddMember("environment", env, a);

    rapidjson::Value history(rapidjson::kObjectType);
    history.AddMember("threadId", rapidjson::Value(ctx.history.threadId.c_str(), a), a);
    rapidjson::Value turns(rapidjson::kArrayType);
    for (const auto& t : ctx.history.turns) {
        rapidjson::Value turn(rapidjson::kObjectType);
        turn.AddMember("turnId", rapidjson::Value(t.turnId.c_str(), a), a);
        rapidjson::Value msgs(rapidjson::kArrayType);
        for (const auto& m : t.messages) msgs.PushBack(messageToJson(m, a), a);
        turn.AddMember("messages", msgs, a);
        turn.AddMember("metrics", agentMetricsToJson(t.metrics, a), a);
        turns.PushBack(turn, a);
    }
    history.AddMember("turns", turns, a);
    d.AddMember("history", history, a);

    rapidjson::Value state(rapidjson::kObjectType);
    state.AddMember("currentStatus", rapidjson::Value(agentStatusToString(ctx.state.currentStatus).c_str(), a), a);
    rapidjson::Value pending(rapidjson::kArrayType);
    for (const auto& p : ctx.state.pendingToolCalls) pending.PushBack(rapidjson::Value(p.c_str(), a), a);
    state.AddMember("pendingToolCalls", pending, a);
    rapidjson::Value procs(rapidjson::kArrayType);
    for (const auto& p : ctx.state.ownedProcesses) procs.PushBack(rapidjson::Value(p.c_str(), a), a);
    state.AddMember("ownedProcesses", procs, a);
    if (ctx.state.fatalError) state.AddMember("fatalError", rapidjson::Value(ctx.state.fatalError->c_str(), a), a);
    else state.AddMember("fatalError", rapidjson::Value(rapidjson::kNullType), a);
    d.AddMember("state", state, a);

    d.AddMember("aggregateMetrics", agentMetricsToJson(ctx.aggregateMetrics, a), a);

    return d;
}

AgentContext fromJson(const rapidjson::Value& v) {
    AgentContext ctx;
    ctx.identity = { v["identity"]["id"].GetString(), v["identity"]["name"].GetString(), v["identity"]["role"].GetString(), v["identity"]["goal"].GetString(), v["identity"]["systemPrompt"].GetString() };
    for (const auto& s : v["permissions"]["allowedScopes"].GetArray()) ctx.permissions.allowedScopes.push_back(stringToToolScope(s.GetString()));
    for (const auto& p : v["permissions"]["allowedPaths"].GetArray()) ctx.permissions.allowedPaths.push_back(p.GetString());
    ctx.permissions.allowOutsideCwd = v["permissions"]["allowOutsideCwd"].GetBool();
    ctx.environment.type = stringToHostType(v["environment"]["type"].GetString());
    ctx.environment.identifier = v["environment"]["identifier"].GetString();
    ctx.environment.cwd = v["environment"]["cwd"].GetString();
    for (auto it = v["environment"]["envVars"].MemberBegin(); it != v["environment"]["envVars"].MemberEnd(); ++it) ctx.environment.envVars[it->name.GetString()] = it->value.GetString();
    ctx.history.threadId = v["history"]["threadId"].GetString();
    for (const auto& t : v["history"]["turns"].GetArray()) {
        AgentTurn turn;
        turn.turnId = t["turnId"].GetString();
        for (const auto& m : t["messages"].GetArray()) turn.messages.push_back(messageFromJson(m));
        turn.metrics = agentMetricsFromJson(t["metrics"]);
        ctx.history.turns.push_back(turn);
    }
    ctx.state.currentStatus = stringToAgentStatus(v["state"]["currentStatus"].GetString());
    for (const auto& p : v["state"]["pendingToolCalls"].GetArray()) ctx.state.pendingToolCalls.push_back(p.GetString());
    for (const auto& p : v["state"]["ownedProcesses"].GetArray()) ctx.state.ownedProcesses.push_back(p.GetString());
    if (v["state"]["fatalError"].IsString()) ctx.state.fatalError = v["state"]["fatalError"].GetString();
    ctx.aggregateMetrics = agentMetricsFromJson(v["aggregateMetrics"]);
    return ctx;
}

rapidjson::Document toJson(const Message& msg) {
    rapidjson::Document d;
    auto& a = d.GetAllocator();
    d.CopyFrom(messageToJson(msg, a), a);
    return d;
}

Message messageFromJsonValue(const rapidjson::Value& v) { return messageFromJson(v); }

rapidjson::Document toJson(const AgentTurn& turn) {
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    d.AddMember("turnId", rapidjson::Value(turn.turnId.c_str(), a), a);
    rapidjson::Value msgs(rapidjson::kArrayType);
    for (const auto& m : turn.messages) msgs.PushBack(messageToJson(m, a), a);
    d.AddMember("messages", msgs, a);
    d.AddMember("metrics", agentMetricsToJson(turn.metrics, a), a);
    return d;
}

AgentTurn agentTurnFromJsonValue(const rapidjson::Value& v) {
    AgentTurn t;
    t.turnId = v["turnId"].GetString();
    for (const auto& m : v["messages"].GetArray()) t.messages.push_back(messageFromJson(m));
    t.metrics = agentMetricsFromJson(v["metrics"]);
    return t;
}

rapidjson::Document toJson(const StreamEvent& ev) {
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    if (auto* txt = std::get_if<TextChunk>(&ev)) {
        d.AddMember("type", "text", a);
        d.AddMember("delta", rapidjson::Value(txt->delta.c_str(), a), a);
    } else if (auto* thk = std::get_if<ThinkingChunk>(&ev)) {
        d.AddMember("type", "thinking", a);
        d.AddMember("delta", rapidjson::Value(thk->delta.c_str(), a), a);
    } else if (auto* tcc = std::get_if<ToolCallChunk>(&ev)) {
        d.AddMember("type", "toolCall", a);
        d.AddMember("id", rapidjson::Value(tcc->id.c_str(), a), a);
        d.AddMember("index", tcc->index, a);
        d.AddMember("nameDelta", rapidjson::Value(tcc->nameDelta.c_str(), a), a);
        d.AddMember("argsDelta", rapidjson::Value(tcc->argsDelta.c_str(), a), a);
    } else if (auto* met = std::get_if<AgentMetrics>(&ev)) {
        d.AddMember("type", "metrics", a);
        rapidjson::Value m = agentMetricsToJson(*met, a);
        for (auto it = m.MemberBegin(); it != m.MemberEnd(); ++it) d.AddMember(rapidjson::Value(it->name, a), rapidjson::Value(it->value, a), a);
    }
    return d;
}

StreamEvent streamEventFromJsonValue(const rapidjson::Value& v) {
    std::string type = v["type"].GetString();
    if (type == "text") return TextChunk{ v["delta"].GetString() };
    if (type == "thinking") return ThinkingChunk{ v["delta"].GetString() };
    if (type == "toolCall") return ToolCallChunk{ v["id"].GetString(), v["index"].GetUint(), v["nameDelta"].GetString(), v["argsDelta"].GetString() };
    if (type == "metrics") return agentMetricsFromJson(v);
    throw std::runtime_error("Unknown StreamEvent type: " + type);
}

rapidjson::Document toJson(const MessagePart& part) {
    rapidjson::Document d;
    auto& a = d.GetAllocator();
    d.CopyFrom(messagePartToJson(part, a), a);
    return d;
}

MessagePart messagePartFromJsonValue(const rapidjson::Value& v) { return messagePartFromJson(v); }

rapidjson::Document toJson(const AgentMetrics& metrics) {
    rapidjson::Document d;
    auto& a = d.GetAllocator();
    d.CopyFrom(agentMetricsToJson(metrics, a), a);
    return d;
}

AgentMetrics agentMetricsFromJsonValue(const rapidjson::Value& v) { return agentMetricsFromJson(v); }

std::string serializeToString(const AgentContext& ctx) {
    rapidjson::Document d = toJson(ctx);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    return buffer.GetString();
}

AgentContext deserializeFromString(const std::string& json) {
    rapidjson::Document d;
    d.Parse(json.c_str());
    if (d.HasParseError()) throw std::runtime_error("JSON Parse Error");
    return fromJson(d);
}

} // namespace firmius::shared
