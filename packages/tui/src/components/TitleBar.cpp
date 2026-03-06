#include "components/TitleBar.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

ftxui::Component TitleBar(const std::shared_ptr<TitleBarModel>& model) {
    return ftxui::Renderer([model] {
        if (!model) {
            return ftxui::text("");
        }
        return ftxui::hbox(
            ftxui::text(model->title) | ftxui::bold,
            ftxui::filler(),
            ftxui::text(model->thread_id)
        ) | ftxui::border;
    });
}

}
