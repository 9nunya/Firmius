#ifndef FIRMIUS_CORE_RUNTIME_OVERLAY_HPP
#define FIRMIUS_CORE_RUNTIME_OVERLAY_HPP

#include "Context.hpp"
#include "IEnvironment.hpp"

#include <string>

namespace firmius::core::runtime_overlay {

shared::AgentHistory buildRequestHistoryWithRuntimeOverlays(
    const shared::AgentContext& context, shared::IHost& host,
    shared::IWorkspace& workspace);

void reconcileSuccessfulToolResult(const shared::AgentContext& context,
                                   shared::IHost& host,
                                   shared::IWorkspace& workspace,
                                   const std::string& toolName,
                                   const std::string& toolArgsJson,
                                   const std::string& resultJson);

void refreshFileWatch(const shared::AgentContext& context,
                      shared::IHost& host,
                      shared::IWorkspace& workspace,
                      const std::string& path);

} // namespace firmius::core::runtime_overlay

#endif
