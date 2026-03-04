#include "ToolRenderer.hpp"
#include "Colors.hpp"
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <rapidjson/document.h>
#include <sstream>
#include <vector>
#include <algorithm>

namespace firmius::tui {

using namespace ftxui;

Element ToolRenderer::render(const ToolCallState& state) {
    if (state.isAborted) {
        return renderAborted(state.name);
    }
    if (state.isError) {
        return renderError(state.name, state.result);
    }

    if (state.name == "search_replace") return renderSearchReplace(state.args, state.result);
    if (state.name == "write_file") return renderWriteFile(state.args);
    if (state.name == "bash") return renderBash(state.args, state.result, state.isError);
    if (state.name == "read_file") return renderReadFile(state.args, state.result);
    if (state.name == "grep" || state.name == "glob") return renderGrepGlob(state.name, state.args, state.result);
    if (state.name == "summon_subagent") return renderSummonSubagent(state.args, state.result);
    if (state.name == "subagent_wait") return renderSubagentWait(state.args);

    return vbox({
        text("$ " + state.name) | color(colors::toolName()) | bold,
        text(state.args) | dim,
        separator(),
        text(state.result)
    }) | border;
}

Element ToolRenderer::renderSearchReplace(const std::string& args, const std::string& result) {
    (void)result;
    rapidjson::Document d;
    d.Parse(args.c_str());
    if (d.HasParseError()) return text("Invalid JSON args: " + args) | color(Color::Red);

    std::string oldStr = d.HasMember("oldString") ? d["oldString"].GetString() : "";
    std::string newStr = d.HasMember("newString") ? d["newString"].GetString() : "";
    std::string path = d.HasMember("filePath") ? d["filePath"].GetString() : "unknown";

    auto split = [](const std::string& s) {
        std::vector<std::string> lines;
        std::stringstream ss(s);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(line);
        return lines;
    };

    auto oldLines = split(oldStr);
    auto newLines = split(newStr);

    size_t maxLines = 10;
    bool truncated = oldLines.size() > maxLines || newLines.size() > maxLines;

    Elements oldElements, newElements;
    for (size_t i = 0; i < std::min(oldLines.size(), maxLines); ++i) {
        oldElements.push_back(text("-" + oldLines[i]) | color(colors::diffRemove()));
    }
    for (size_t i = 0; i < std::min(newLines.size(), maxLines); ++i) {
        newElements.push_back(text("+" + newLines[i]) | color(colors::diffAdd()));
    }

    if (truncated) {
        oldElements.push_back(text("...") | dim);
        newElements.push_back(text("...") | dim);
    }

    return vbox({
        text("search_replace: " + path) | color(colors::toolName()) | bold,
        hbox({
            vbox(std::move(oldElements)) | flex,
            separator(),
            vbox(std::move(newElements)) | flex,
        })
    }) | border;
}

Element ToolRenderer::renderWriteFile(const std::string& args) {
    rapidjson::Document d;
    d.Parse(args.c_str());
    if (d.HasParseError()) return text("Invalid JSON args: " + args) | color(Color::Red);

    std::string content = d.HasMember("content") ? d["content"].GetString() : "";
    std::string path = d.HasMember("filePath") ? d["filePath"].GetString() : "unknown";

    std::vector<std::string> lines;
    std::stringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);

    Elements elements;
    elements.push_back(text("write_file: " + path) | color(colors::toolName()) | bold);
    elements.push_back(separator());

    size_t limit = 10;
    for (size_t i = 0; i < std::min(lines.size(), limit); ++i) {
        elements.push_back(text("+" + lines[i]) | color(colors::diffAdd()));
    }
    if (lines.size() > limit) {
        elements.push_back(text("...+" + std::to_string(lines.size() - limit) + " more lines") | dim);
    }

    return vbox(std::move(elements)) | border;
}

Element ToolRenderer::renderBash(const std::string& args, const std::string& result, bool isError) {
    rapidjson::Document d;
    d.Parse(args.c_str());
    std::string cmd = (d.IsObject() && d.HasMember("command")) ? d["command"].GetString() : args;

    return vbox({
        text("$ " + cmd) | color(colors::toolName()) | bold,
        separator(),
        paragraph(result),
        separator(),
        hbox({
            text("Exit Code: ") | dim,
            text(isError ? "x" : "✓") | color(isError ? Color::Red : Color::Green) | bold
        })
    }) | border;
}

Element ToolRenderer::renderReadFile(const std::string& args, const std::string& result) {
    rapidjson::Document d;
    d.Parse(args.c_str());
    std::string path = (d.IsObject() && d.HasMember("filePath")) ? d["filePath"].GetString() : "unknown";

    std::vector<std::string> lines;
    std::stringstream ss(result);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);

    Elements elements;
    elements.push_back(text("read_file: " + path) | color(colors::toolName()) | bold);
    elements.push_back(separator());

    size_t limit = 10;
    for (size_t i = 0; i < std::min(lines.size(), limit); ++i) {
        elements.push_back(text(lines[i]));
    }
    if (lines.size() > limit) {
        elements.push_back(text("...+" + std::to_string(lines.size() - limit) + " more lines") | dim);
    }

    return vbox(std::move(elements)) | border;
}

Element ToolRenderer::renderGrepGlob(const std::string& name, const std::string& args, const std::string& result) {
    (void)args;
    std::vector<std::string> lines;
    std::stringstream ss(result);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);

    size_t matchCount = lines.size();
    
    Elements elements;
    elements.push_back(text(name + " matches: " + std::to_string(matchCount)) | color(colors::toolName()) | bold);
    elements.push_back(separator());

    size_t limit = 5;
    for (size_t i = 0; i < std::min(lines.size(), limit); ++i) {
        elements.push_back(text(lines[i]));
    }
    if (lines.size() > limit) {
        elements.push_back(text("...+" + std::to_string(lines.size() - limit) + " more") | dim);
    }

    return vbox(std::move(elements)) | border;
}

Element ToolRenderer::renderSummonSubagent(const std::string& args, const std::string& result) {
    rapidjson::Document d;
    d.Parse(args.c_str());
    std::string persona = (d.IsObject() && d.HasMember("persona")) ? d["persona"].GetString() : "unknown";
    
    std::string id = result;

    auto t = Table({
        { text("ID"), text("Persona"), text("Status") },
        { text(id), text(persona), text("Spawning...") | color(colors::subagentStatus()) }
    });
    t.SelectAll().Border(LIGHT);
    t.SelectRow(0).Decorate(bold);

    return vbox({
        text("summon_subagent") | color(colors::toolName()) | bold,
        t.Render()
    }) | border;
}

Element ToolRenderer::renderSubagentWait(const std::string& args) {
    rapidjson::Document d;
    d.Parse(args.c_str());
    std::string id = (d.IsObject() && d.HasMember("subagentId")) ? d["subagentId"].GetString() : args;

    return hbox({
        text("Waiting on " + id + "...") | color(colors::subagentStatus()),
        text(" ●") | color(Color::Yellow)
    }) | border;
}

Element ToolRenderer::renderError(const std::string& name, const std::string& result) {
    return vbox({
        text("Tool Error: " + name) | color(colors::toolError()) | bold,
        separator(),
        paragraph(result)
    }) | border | color(colors::toolError());
}

Element ToolRenderer::renderAborted(const std::string& name) {
    return hbox({
        text("⚠ ") | color(colors::interrupted()),
        text("Tool Aborted: " + name) | color(colors::interrupted())
    }) | border | color(colors::interrupted());
}

} // namespace firmius::tui
