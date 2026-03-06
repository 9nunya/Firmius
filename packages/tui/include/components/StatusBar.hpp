#ifndef FIRMIUS_COMPONENTS_STATUS_BAR_HPP
#define FIRMIUS_COMPONENTS_STATUS_BAR_HPP

#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>

namespace firmius::tui {

struct StatusBarModel {
    std::string status_text;
};

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel>& model);

}

#endif
