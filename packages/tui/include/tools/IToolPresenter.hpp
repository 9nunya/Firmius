#ifndef FIRMIUS_TUI_ITOOLPRESENTER_HPP
#define FIRMIUS_TUI_ITOOLPRESENTER_HPP

#include <string>
#include <vector>

namespace firmius::tui {

class ToolCallItem;
class AppState;

/// Context passed to tool presenters during rendering.
struct ToolRenderContext {
  const AppState* state = nullptr;
};

/// Interface for tool presenters. Each presenter handles rendering for a tool family.
class IToolPresenter {
public:
  virtual ~IToolPresenter() = default;

  /// Name of this presenter (for debugging).
  virtual std::string name() const = 0;

  /// Whether this presenter can handle the given tool name.
  virtual bool matches(const std::string& toolName) const = 0;

  /// Render the tool call item into ANSI-formatted lines.
  virtual std::vector<std::string> render(const ToolCallItem& item,
                                          const ToolRenderContext& ctx,
                                          int width) const = 0;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_ITOOLPRESENTER_HPP
