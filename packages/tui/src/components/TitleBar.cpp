#include "components/TitleBar.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

ftxui::Component TitleBar(const std::shared_ptr<TitleBarModel>& model) {
    return ftxui::Renderer([model] {
        if (!model) {
            return ftxui::text("");
        }
        return ftxui::vbox({
            ftxui::hbox({
                ftxui::text(" ") | ftxui::bgcolor(ftxui::Color::Cyan),
                ftxui::text(" " + model->title + " ") | ftxui::bold | ftxui::color(ftxui::Color::Cyan),
            }),
            ftxui::separatorLight() | ftxui::color(ftxui::Color::Cyan) | ftxui::dim,
        });
    });
}

}
