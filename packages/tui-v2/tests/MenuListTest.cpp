#include "MenuList.hpp"

#include <gtest/gtest.h>

using namespace firmius::tui2;

namespace {

MouseEvent leftClick(int row) {
  MouseEvent event;
  event.type = MouseEvent::Type::Press;
  event.button = MouseEvent::Button::Left;
  event.row = row;
  event.col = 1;
  return event;
}

} // namespace

TEST(MenuListTest, ClickSelectsThenActivatesOnSecondClick) {
  MenuList menu;
  bool selected = false;
  std::string selectedId;

  menu.open();
  menu.setTitle("Menu");
  menu.setItems({{"First", "", "1"}, {"Second", "", "2"}});
  menu.setScreenRow(10);
  menu.setOnSelect([&](const MenuList::Item& item) {
    selected = true;
    selectedId = item.id;
  });

  EXPECT_TRUE(menu.handleMouse(leftClick(14), 10, 1));
  EXPECT_EQ(menu.selectedIndex(), 0);
  EXPECT_FALSE(selected);

  EXPECT_TRUE(menu.handleMouse(leftClick(15), 10, 1));
  EXPECT_EQ(menu.selectedIndex(), 1);
  EXPECT_FALSE(selected);

  EXPECT_TRUE(menu.handleMouse(leftClick(15), 10, 1));
  EXPECT_TRUE(selected);
  EXPECT_EQ(selectedId, "2");
}

