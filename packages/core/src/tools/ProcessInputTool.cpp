#include "tools/ProcessInputTool.hpp"
#include "IAgent.hpp"
#include "utils/TerminalUtil.hpp"
#include <thread>
#include <chrono>

namespace firmius::core {

shared::ToolResult ProcessInputTool::execute(const ProcessInputInput& input, shared::ToolContext& ctx) {
    try {
        std::string translated = shared::TerminalUtil::translate(input.input);
        
        // Split by \n and send with delay
        size_t last = 0;
        size_t next = 0;
        while ((next = translated.find('\n', last)) != std::string::npos) {
            std::string part = translated.substr(last, next - last + 1);
            ctx.agent.writeToProcess(input.process_id, part);
            
            // Wait for 1000ms but stay interruptible
            auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000)) {
                if (ctx.agent.isInterrupted()) {
                    return shared::ToolResult::fail("Interrupted");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            last = next + 1;
        }
        
        // Send remaining part
        if (last < translated.size()) {
            ctx.agent.writeToProcess(input.process_id, translated.substr(last));
        }

        return shared::ToolResult::ok();
    } catch (const std::exception& e) {
        return shared::ToolResult::fail(e.what());
    }
}

}
