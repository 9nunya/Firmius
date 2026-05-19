#ifndef FIRMIUS_PROVIDER_ANTIGRAVITYPROTOCOL_HPP
#define FIRMIUS_PROVIDER_ANTIGRAVITYPROTOCOL_HPP

#include "IProvider.hpp"
#include <string>
#include <rapidjson/document.h>

namespace firmius::provider {

/**
 * @brief Handles translation between AgentHistory and Gemini-specific JSON payloads.
 */
class AntigravityProtocol {
public:
    struct RequestContext {
        std::string modelId;
        std::string projectId;
        std::string sessionId;
        std::string requestId;
    };

    /**
     * @brief Translates AgentHistory and options into an Antigravity API request body.
     */
    static std::string prepareRequestBody(const AgentHistory& history, 
                                        const ProviderOptions& opts,
                                        const RequestContext& ctx);

    /**
     * @brief Translates a JSON schema string into Gemini-compatible schema format.
     */
    static rapidjson::Value toGeminiSchema(const std::string& inputSchema, 
                                         rapidjson::Document::AllocatorType& a);

    /**
     * @brief Converts Internal Role enum to Gemini-compatible role string.
     */
    static std::string roleToString(Role r);
};

} // namespace firmius::provider

#endif // FIRMIUS_PROVIDER_ANTIGRAVITYPROTOCOL_HPP
