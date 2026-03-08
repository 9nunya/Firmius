#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/dom/node.hpp>
#include <iostream>
#include <vector>
#include <string>

namespace ftxui {
// Simple ScrollableBox mock based on the codebase's implementation
Element ScrollableBoxRepro(Element child, int& size, int viewport_width) {
    auto background = child;
    if (viewport_width > 0) {
        background = background | size(WIDTH, EQUAL, viewport_width);
    }
    background->ComputeRequirement();
    size = background->requirement().min_y;
    
    return background 
        | focusPosition(0, size) // Try to focus at the very bottom
        | frame 
        | vscroll_indicator 
        | yflex;
}
}

int main() {
    using namespace ftxui;
    
    // Create a table similar to the one in the screenshot
    std::vector<std::vector<Element>> table_data;
    table_data.push_back({text("Element") | bold, text("Story 1") | bold, text("Story 2") | bold, text("Connection") | bold});
    
    for(int i=0; i<5; ++i) {
        table_data.push_back({
            text("Key " + std::to_string(i)),
            paragraph("This is a long description for story 1 that should wrap correctly within the cell and contribute to the height."),
            paragraph("This is a long description for story 2 that should wrap correctly within the cell and contribute to the height."),
            paragraph("This is the connection between them, which is also quite long and complex.")
        });
    }

    auto table = Table(std::move(table_data));
    table.SelectAll().Border(LIGHT);
    table.SelectAll().SeparatorHorizontal(LIGHT);
    table.SelectAll().SeparatorVertical(LIGHT);

    int size = 0;
    int viewport_width = 265;
    int viewport_height = 62;

    auto document = vbox({
        text("Scroll Repro"),
        separator(),
        ScrollableBoxRepro(table.Render(), size, viewport_width)
    });

    auto screen = Screen::Create(Dimension::Fixed(viewport_width), Dimension::Fixed(viewport_height));
    Render(screen, document);
    
    std::cout << "Calculated Content Height (size_): " << size << std::endl;
    // Print the last few lines of the screen to see if we reached the bottom of the table
    // The table has a bottom border. If we don't see it, scrolling is broken.
    screen.Print();

    return 0;
}
