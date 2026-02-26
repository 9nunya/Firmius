#ifndef FIRMIUS_PROVIDER_ZEN_PROVIDER_HPP
#define FIRMIUS_PROVIDER_ZEN_PROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"
#include <string>

namespace firmius::provider {

/**
 * @brief Provider implementation for Zen.
 */
class ZenProvider : public BaseOpenAIProvider {
public:
    /**
     * @brief Constructs a ZenProvider.
     * @param apiKey The Zen API key.
     */
    ZenProvider(const std::string& apiKey);
};

}

#endif
