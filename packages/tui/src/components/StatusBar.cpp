#include "components/StatusBar.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel>& model) {
    return ftxui::Renderer([model] {
        if (!model) {
            return ftxui::text("");
        }
        return ftxui::hbox(
            ftxui::text(model->status_text) | ftxui::dim
        ) | ftxui::border;
    });
}

}
