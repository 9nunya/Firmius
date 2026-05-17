#pragma once

#include "tools/ToolPresenterRegistry.hpp"
#include "tools/ProcessPresenter.hpp"
#include "tools/DelegatePresenter.hpp"
#include "tools/EditPresenter.hpp"
#include "tools/ReadPresenter.hpp"
#include "tools/ListPresenter.hpp"
#include "tools/GrepPresenter.hpp"
#include "tools/GlobPresenter.hpp"
#include "tools/ArtifactsPresenter.hpp"
#include "tools/WebPresenter.hpp"
#include "tools/LspPresenter.hpp"
#include "tools/FleetPresenter.hpp"
#include "tools/TodoPresenter.hpp"
#include "tools/ModeSwitchPresenter.hpp"
#include "tools/SkillPresenter.hpp"
#include "tools/McpPresenter.hpp"
#include "tools/GenericPresenter.hpp"

namespace firmius::tui2 {

/// Register all built-in tool presenters. Call once at startup.
inline void registerAllPresenters() {
  auto& registry = ToolPresenterRegistry::instance();

  // Register specialized presenters first (order matters — first match wins)
  registry.registerPresenter(std::make_unique<ProcessPresenter>());
  registry.registerPresenter(std::make_unique<DelegatePresenter>());
  registry.registerPresenter(std::make_unique<EditPresenter>());
  registry.registerPresenter(std::make_unique<ReadPresenter>());
  registry.registerPresenter(std::make_unique<ListPresenter>());
  registry.registerPresenter(std::make_unique<GrepPresenter>());
  registry.registerPresenter(std::make_unique<GlobPresenter>());
  registry.registerPresenter(std::make_unique<ArtifactsPresenter>());
  registry.registerPresenter(std::make_unique<WebPresenter>());
  registry.registerPresenter(std::make_unique<LspPresenter>());
  registry.registerPresenter(std::make_unique<FleetPresenter>());
  registry.registerPresenter(std::make_unique<TodoPresenter>());
  registry.registerPresenter(std::make_unique<ModeSwitchPresenter>());
  registry.registerPresenter(std::make_unique<SkillPresenter>());
  registry.registerPresenter(std::make_unique<McpPresenter>());

  // Generic fallback — must be last
  registry.registerPresenter(std::make_unique<GenericPresenter>());
}

} // namespace firmius::tui2
