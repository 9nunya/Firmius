#include "providers/OpenRouterProvider.hpp"

namespace firmius::provider {

OpenRouterProvider::OpenRouterProvider(const std::string& apiKey)
    : BaseOpenAIProvider("https://openrouter.ai/api/v1", apiKey) {}

std::map<std::string, std::string> OpenRouterProvider::getHeaders() {
    auto h = BaseOpenAIProvider::getHeaders();
    h["HTTP-Referer"] = "https://firmius.ai";
    h["X-Title"] = "Firmius";
    return h;
}

std::string OpenRouterProvider::getReasoningFieldName() const {
    return "reasoning";
}

}
