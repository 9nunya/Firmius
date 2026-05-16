#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui2 {

/// Presenter for Files tool (Read, List, Grep, Glob).
class FilesPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Files"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, int width) const override;
};

} // namespace firmius::tui2
