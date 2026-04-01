#include "tools/ToolRegistry.hpp"
#include <iostream>
#include <string>

using namespace firmius::core;
using namespace firmius::shared;

int main() {
    ToolRegistry registry;
    // We cannot easily mock ITool here without more setup, 
    // but we can compile this against firmius_core.
    std::cout << "Test compiled!" << std::endl;
    return 0;
}
