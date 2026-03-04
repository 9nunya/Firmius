#include "ModalSystem.hpp"
#include "AppState.hpp"
#include "Colors.hpp"
#include "harness/Harness.hpp"
#include "providers/ProviderRegistry.hpp"
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace firmius::tui {

using namespace ftxui;

ModalSystem::ModalSystem(std::shared_ptr<AppState> state) : state_(std::move(state)) {}

Element ModalSystem::Render(Element main_content) const {
    if (activeModal_ == ModalType::None) {
        return main_content;
    }

    Element modal;
    switch (activeModal_) {
        case ModalType::ModelSelector:
            modal = renderModelSelector();
            break;
        case ModalType::ThreadSwitcher:
            modal = renderThreadSwitcher();
            break;
        default:
            modal = emptyElement();
    }

    return dbox({
        main_content | dim,
        vbox({
            filler(),
            hbox({
                filler(),
                modal,
                filler(),
            }),
            filler(),
        }) | bgcolor(Color::RGB(40, 40, 40)) | color(Color::White)
    });
}

bool ModalSystem::HandleEvent(Event event) {
    if (activeModal_ == ModalType::None) {
        return false;
    }

    if (event == Event::Escape) {
        hide();
        return true;
    }

    if (event == Event::ArrowUp) {
        selectedIndex_ = std::max(0, selectedIndex_ - 1);
        return true;
    }

    if (event == Event::ArrowDown) {
        selectedIndex_++;
        return true;
    }

    if (event == Event::Return) {
        hide();
        return true;
    }

    return true; 
}

void ModalSystem::show(ModalType type) {
    activeModal_ = type;
    selectedIndex_ = 0;
}

void ModalSystem::hide() {
    activeModal_ = ModalType::None;
}

bool ModalSystem::isActive() const {
    return activeModal_ != ModalType::None;
}

Element ModalSystem::renderModelSelector() const {
    std::vector<Element> rows;
    auto& registry = firmius::provider::ProviderRegistry::instance();
    auto providerIds = registry.listProviderIds();
    
    int current = 0;
    for (const auto& pid : providerIds) {
        auto provider = registry.getProvider(pid);
        if (!provider) continue;
        
        auto models = provider->listModels();
        for (const auto& model : models) {
            bool isSelected = (current == selectedIndex_);
            
            std::string vision = " ";
            for (const auto& mod : model.modalities) {
                if (mod == "image") vision = "👁 ";
            }

            std::string ctx = std::to_string(model.contextWindow / 1024) + "k";
            
            auto row = hbox({
                text(isSelected ? "> " : "  ") | bold | color(isSelected ? ftxui::Color(ftxui::Color::Cyan) : ftxui::Color(ftxui::Color::Default)),
                text(model.provider) | dim | size(WIDTH, EQUAL, 10),
                text(model.id) | flex,
                text(vision) | color(ftxui::Color(ftxui::Color::Yellow)),
                text(ctx) | color(ftxui::Color(ftxui::Color::Green)) | size(WIDTH, EQUAL, 6),
            });


            if (isSelected) {
                row = row | bgcolor(Color::GrayDark);
            }
            
            rows.push_back(row);
            current++;
        }
    }

    if (rows.empty()) {
        rows.push_back(text("No models available") | center);
    }

    return wrapInDialog(vbox(std::move(rows)) | size(HEIGHT, LESS_THAN, 15), "SWITCH MODEL");
}

Element ModalSystem::renderThreadSwitcher() const {
    auto threads = firmius::core::Harness::instance().listThreads();
    std::vector<Element> rows;
    
    int current = 0;
    for (const auto& thread : threads) {
        bool isSelected = (current == selectedIndex_);
        
        std::time_t t = thread.lastActiveAt / 1000;
        std::tm* tm = std::localtime(&t);
        std::stringstream ss;
        if (tm) {
            ss << std::put_time(tm, "%Y-%m-%d %H:%M");
        }

        auto row = hbox({
            text(isSelected ? "> " : "  ") | bold | color(isSelected ? ftxui::Color(ftxui::Color::Cyan) : ftxui::Color(ftxui::Color::Default)),
            text(thread.title.empty() ? thread.threadId : thread.title) | flex,
            text(ss.str()) | dim | size(WIDTH, EQUAL, 17),
        });

        if (isSelected) {
            row = row | bgcolor(Color::GrayDark);
        }
        
        rows.push_back(row);
        current++;
    }

    if (rows.empty()) {
        rows.push_back(text("No threads found") | center);
    }

    return wrapInDialog(vbox(std::move(rows)) | size(HEIGHT, LESS_THAN, 15), "SWITCH THREAD");
}

Element ModalSystem::wrapInDialog(Element content, const std::string& title) const {
    return vbox({
        hbox({
            text(" " + title + " ") | bold | color(ftxui::Color::Black) | bgcolor(ftxui::Color::Cyan),
            filler(),
        }),
        content,
        text(" [Enter] Select  [Esc] Close ") | center | dim,
    }) | border | size(WIDTH, EQUAL, 60) | bgcolor(Color::Black);
}

}
