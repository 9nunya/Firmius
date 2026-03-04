#pragma once

#include <ftxui/component/component.hpp>
#include <string>
#include <vector>
#include <memory>
#include "CommandRegistry.hpp"
#include "AppState.hpp"

namespace firmius::tui {

class CommandHelper {
public:
    CommandHelper(const CommandRegistry& registry, const AppState& state);

    ftxui::Element render(const std::string& currentInput);

private:
    std::vector<std::string> getSuggestions(const std::string& input);
    
    const CommandRegistry& registry_;
    const AppState& state_;
};

}
