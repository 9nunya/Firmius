#ifndef FIRMIUS_CORE_RUNTIMEOVERLAY_HPP
#define FIRMIUS_CORE_RUNTIMEOVERLAY_HPP

#include "Context.hpp"
#include "IEnvironment.hpp"

#include <string>

namespace firmius::core::runtime_overlay {

shared::AgentHistory buildRequestHistoryWithRuntimeOverlays(
    const shared::AgentContext& context, shared::IHost& host,
    shared::IWorkspace& workspace);

void reconcileSuccessfulToolResult(shared::AgentContext& context,
                                   shared::IHost& host,
                                   shared::IWorkspace& workspace,
                                   const std::string& toolName,
                                   const std::string& toolArgsJson,
                                   const std::string& resultJson);

void refreshFileWatch(const shared::AgentContext& context,
                      shared::IHost& host,
                      shared::IWorkspace& workspace,
                      const std::string& path);
void reconstructStateFromHistory(shared::AgentContext& context,
                               shared::IHost& host,
                               shared::IWorkspace& workspace);


} // namespace firmius::core::runtime_overlay

#endif
