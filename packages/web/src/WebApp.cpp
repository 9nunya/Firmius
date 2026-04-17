#include "web/WebApp.hpp"

namespace firmius::web {

WebApp::WebApp() = default;

std::string WebApp::name() const {
    return "firmius-web";
}

std::string WebApp::version() const {
    return "0.1.0";
}

std::string WebApp::describe() const {
    return name() + " backend skeleton " + version();
}

} // namespace firmius::web
