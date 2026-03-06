#ifndef FIRMIUS_COMPONENTS_INPUT_BAR_HPP
#define FIRMIUS_COMPONENTS_INPUT_BAR_HPP

#include <ftxui/component/component_base.hpp>
#include <functional>
#include <memory>
#include <string>

namespace firmius::tui {

struct InputBarModel {
    std::string* buffer = nullptr;
    int* cursor = nullptr;
    std::string placeholder;
};

ftxui::Component InputBar(const std::shared_ptr<InputBarModel>& model,
                          std::function<void(const std::string&)> on_submit);

}

#endif
