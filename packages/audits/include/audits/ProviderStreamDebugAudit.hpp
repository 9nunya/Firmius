#ifndef FIRMIUS_AUDITS_PROVIDERSTREAMDEBUGAUDIT_HPP
#define FIRMIUS_AUDITS_PROVIDERSTREAMDEBUGAUDIT_HPP

#include "IAudit.hpp"
#include "Context.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

using firmius::shared::AgentHistory;
using firmius::shared::AuditResult;
using firmius::shared::IAudit;

/**
 * @brief Debug audit exposed as provider_full_range that logs EVERY chunk from a provider stream to STDOUT.
 *
 * Usage: firmius_audit --audit provider_full_range <provider_id> [model_id]
 *          [--history-variant=<variant>] [--variant=<model-variant>]
 *        firmius_audit --audit provider_full_range <provider_id> [model_id]
 *          --thread-id=<threadId> [--thread-agent=<agentId>]
 *        firmius_audit --audit provider_full_range <provider_id> [model_id]
 *          [--variant=<model-variant>] --tool-preparing-suite
 * 
 * History variants for testing edge cases:
 * - normal_agentic: Standard conversation with user/assistant messages
 * - agentic_tool_errors: Conversation with tool errors in chat
 * - multiple_tool_results: Multiple tool results in sequence
 * - tool_then_error: Tool result followed by error message
 * - error_then_tool: Error message followed by tool result
 * - thinking_long_tool_call: Should emit thinking plus a long streamed tool call
 * - multi_turn_thinking_preparing: Multi-turn follow-up that should think and prepare a tool call
 * - parallel_tool_preparing: Should think and prepare multiple tool calls in parallel
 */
class ProviderStreamDebugAudit : public IAudit {
public:
    static std::string resolveModelIdArg(const std::vector<std::string>& args);

    std::string getId() const override;
    std::string getDescription() const override;
    AuditResult run(const std::vector<std::string>& args) override;
    
private:
    /**
     * @brief Build different history variants for testing edge cases.
     */
    AgentHistory buildHistoryVariant(const std::string& variant);
    std::vector<std::string> suiteVariants(const std::string& suiteName) const;
};

} // namespace firmius::audits

#endif // FIRMIUS_AUDITS_PROVIDERSTREAMDEBUGAUDIT_HPP
