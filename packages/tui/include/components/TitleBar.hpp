#ifndef FIRMIUS_COMPONENTS_TITLE_BAR_HPP
#define FIRMIUS_COMPONENTS_TITLE_BAR_HPP

#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>

namespace firmius::tui {

struct TitleBarModel {
    std::string title;
    std::string thread_id;
};

ftxui::Component TitleBar(const std::shared_ptr<TitleBarModel>& model);

}

#endif
