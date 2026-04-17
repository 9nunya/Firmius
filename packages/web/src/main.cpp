#include "web/WebApp.hpp"

#include <iostream>

int main() {
    const firmius::web::WebApp app;
    std::cout << app.describe() << std::endl;
    return 0;
}
