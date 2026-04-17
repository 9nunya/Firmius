#pragma once

#include <string>

namespace firmius::web {

class WebApp {
public:
    WebApp();

    [[nodiscard]] std::string name() const;
    [[nodiscard]] std::string version() const;
    [[nodiscard]] std::string describe() const;
};

} // namespace firmius::web
