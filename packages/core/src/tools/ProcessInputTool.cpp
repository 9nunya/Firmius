#include "tools/ProcessInputTool.hpp"
#include "IAgent.hpp"
#include "utils/TerminalUtil.hpp"
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace firmius::core {

namespace {
std::string escapeForJson(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 32) {
                    // Escape control characters
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(c));
                    result += oss.str();
                } else {
                    result += c;
                }
        }
    }
    return result;
}
}

shared::ToolResult ProcessInputTool::execute(const ProcessInputInput& input, shared::ToolContext& ctx) {
    try {
        std::string translated = shared::TerminalUtil::translate(input.input);
        
        int linesSent = 0;
        int charsSent = 0;
        
        // Split by \n and send with delay
        size_t last = 0;
        size_t next = 0;
        while ((next = translated.find('\n', last)) != std::string::npos) {
            std::string part = translated.substr(last, next - last + 1);
            ctx.agent.getEnvironment()->getProcessManager().writeToProcess(input.process_id, part);
            linesSent++;
            charsSent += static_cast<int>(part.size());
            
            // Wait for 1000ms but stay interruptible.
            auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000)) {
                if (ctx.cancelRequested()) {
                    return shared::ToolResult::fail("Interrupted");
                }
                if (!ctx.waitFor(std::chrono::milliseconds(10))) {
                    return shared::ToolResult::fail("Interrupted");
                }
            }
            last = next + 1;
        }
        
        // Send remaining part
        if (last < translated.size()) {
            std::string remaining = translated.substr(last);
            ctx.agent.getEnvironment()->getProcessManager().writeToProcess(input.process_id, remaining);
            charsSent += static_cast<int>(remaining.size());
        }

        // Return meaningful feedback
        std::string escapedInput = escapeForJson(translated);
        std::string resultJson = "{\"sent\":\"" + escapedInput + 
            "\",\"chars\":" + std::to_string(charsSent) + 
            ",\"lines\":" + std::to_string(linesSent) + "}";
        return shared::ToolResult::ok(resultJson);
    } catch (const std::exception& e) {
        return shared::ToolResult::fail(e.what());
    }
}

}
