#include "components/InputBar.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

static void insertText(std::string& text, int& cursor, const std::string& insert) {
    if (cursor < 0) cursor = 0;
    if (cursor > static_cast<int>(text.size())) cursor = static_cast<int>(text.size());
    text.insert(text.begin() + cursor, insert.begin(), insert.end());
    cursor += static_cast<int>(insert.size());
}

ftxui::Component InputBar(const std::shared_ptr<InputBarModel>& model,
                          std::function<void(const std::string&)> on_submit) {
    auto opt = ftxui::InputOption::Default();
    opt.multiline = true;
    if (model) {
        opt.content = model->buffer;
        opt.cursor_position = model->cursor;
        opt.placeholder = model->placeholder;
    }

    auto input = ftxui::Input(opt);
    auto with_keys = ftxui::CatchEvent(input, [model, on_submit](ftxui::Event event) {
        if (!model || !model->buffer || !model->cursor) return false;
        if (event == ftxui::Event::Return) {
            if (!model->buffer->empty()) {
                on_submit(*model->buffer);
                model->buffer->clear();
                *model->cursor = 0;
            }
            return true;
        }
        if (event.input() == "\x1b[13;2u") { // Shift+Enter
            insertText(*model->buffer, *model->cursor, "\n");
            return true;
        }
        return false;
    });

    return ftxui::Renderer(with_keys, [with_keys] {
        return ftxui::vbox({
            with_keys->Render() | ftxui::border,
        });
    });
}

}
