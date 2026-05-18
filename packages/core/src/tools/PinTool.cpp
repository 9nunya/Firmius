#include "tools/PinTool.hpp"

#include "IAgent.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::core {

using namespace firmius::shared;

shared::ToolMetadata PinTool::getMetadata() const {
  return {"pin",
          "Pin or unpin an anchor (free-text fact or specific turn id) so the "
          "working-memory layer keeps it across long sessions. Use this when "
          "you identify a constraint, decision, or fact that must survive "
          "aggressive eviction. Hard pins live forever until explicitly "
          "removed.",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> PinTool::getSchema() const {
  return zObject({
             {"action",
              zString()->describe(
                  "Either 'add' (default) or 'remove'.")},
             {"text",
              zString()->describe(
                  "Free-text anchor to pin or unpin. Optional if "
                  "turn_id is provided.")},
             {"turn_id",
              zString()->describe(
                  "Specific turn ID to pin or unpin. Optional if "
                  "text is provided.")},
         })
      ->required({});
}

shared::ToolResult PinTool::execute(const rapidjson::Value &input,
                                    shared::ToolContext &ctx) {
  try {
    std::string action = "add";
    if (input.HasMember("action") && input["action"].IsString()) {
      action = shared::StringUtil::trim(std::string(input["action"].GetString()));
    }

    std::string text;
    if (input.HasMember("text") && input["text"].IsString()) {
      text = shared::StringUtil::trim(std::string(input["text"].GetString()));
    }
    std::string turnId;
    if (input.HasMember("turn_id") && input["turn_id"].IsString()) {
      turnId = shared::StringUtil::trim(std::string(input["turn_id"].GetString()));
    }
    if (text.empty() && turnId.empty()) {
      throw std::runtime_error(
          "pin requires at least one of 'text' or 'turn_id'");
    }

    auto &state = ctx.agent.getMutableContext().state;

    if (action == "remove") {
      bool removed = false;
      if (!text.empty()) {
        auto it = std::find(state.agentMemoryPins.begin(),
                            state.agentMemoryPins.end(), text);
        if (it != state.agentMemoryPins.end()) {
          state.agentMemoryPins.erase(it);
          removed = true;
        }
      }
      if (!turnId.empty()) {
        auto it = std::find(state.pinnedTurnIds.begin(),
                            state.pinnedTurnIds.end(), turnId);
        if (it != state.pinnedTurnIds.end()) {
          state.pinnedTurnIds.erase(it);
          removed = true;
        }
      }
      rapidjson::Document doc;
      doc.SetObject();
      auto &alloc = doc.GetAllocator();
      const std::string msg =
          removed ? "Pin removed." : "No matching pin found.";
      doc.AddMember(
          "result",
          rapidjson::Value(msg.c_str(),
                           static_cast<rapidjson::SizeType>(msg.size()), alloc),
          alloc);
      doc.AddMember("active_pins",
                    static_cast<int>(state.agentMemoryPins.size()), alloc);
      doc.AddMember("pinned_turns",
                    static_cast<int>(state.pinnedTurnIds.size()), alloc);
      return shared::ToolResult::ok(doc);
    }

    if (action != "add") {
      throw std::runtime_error("pin action must be 'add' or 'remove'");
    }

    if (!text.empty() && std::find(state.agentMemoryPins.begin(),
                                   state.agentMemoryPins.end(),
                                   text) == state.agentMemoryPins.end()) {
      state.agentMemoryPins.push_back(text);
    }
    if (!turnId.empty() && std::find(state.pinnedTurnIds.begin(),
                                     state.pinnedTurnIds.end(),
                                     turnId) == state.pinnedTurnIds.end()) {
      state.pinnedTurnIds.push_back(turnId);
    }
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    std::ostringstream msg;
    msg << "Pin added.";
    if (!text.empty()) {
      msg << " Anchor: \"" << text << "\".";
    }
    if (!turnId.empty()) {
      msg << " Turn: " << turnId << ".";
    }
    msg << " Active pins: " << state.agentMemoryPins.size()
        << ", pinned turns: " << state.pinnedTurnIds.size() << '.';
    const std::string msgStr = msg.str();
    doc.AddMember(
        "result",
        rapidjson::Value(msgStr.c_str(),
                         static_cast<rapidjson::SizeType>(msgStr.size()),
                         alloc),
        alloc);
    doc.AddMember("active_pins",
                  static_cast<int>(state.agentMemoryPins.size()), alloc);
    doc.AddMember("pinned_turns",
                  static_cast<int>(state.pinnedTurnIds.size()), alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
