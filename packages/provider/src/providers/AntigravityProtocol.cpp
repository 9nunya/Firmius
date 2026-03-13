#include "providers/AntigravityProtocol.hpp"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::provider {

std::string AntigravityProtocol::roleToString(Role r) {
  switch (r) {
  case Role::System:
    return "system";
  case Role::User:
    return "user";
  case Role::Assistant:
    return "model";
  case Role::ToolResult:
    return "user"; // Antigravity/Gemini CLI uses 'user' role for function
                   // response parts
  case Role::Error:
    return "system";
  default:
    return "user";
  }
}

rapidjson::Value
AntigravityProtocol::toGeminiSchema(const std::string &inputSchema,
                                    rapidjson::Document::AllocatorType &a) {
  rapidjson::Document doc;
  doc.Parse(inputSchema.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    rapidjson::Value fallback(rapidjson::kObjectType);
    fallback.AddMember("type", "OBJECT", a);
    return fallback;
  }

  std::function<rapidjson::Value(const rapidjson::Value &)> transform;
  transform = [&](const rapidjson::Value &val) -> rapidjson::Value {
    if (val.IsObject()) {
      rapidjson::Value out(rapidjson::kObjectType);
      for (auto it = val.MemberBegin(); it != val.MemberEnd(); ++it) {
        std::string key = it->name.GetString();
        if (key == "type" && it->value.IsString()) {
          std::string typeStr = it->value.GetString();
          for (auto &c : typeStr)
            c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
          out.AddMember("type", rapidjson::Value(typeStr.c_str(), a), a);
        } else if (key == "properties" && it->value.IsObject()) {
          rapidjson::Value props(rapidjson::kObjectType);
          for (auto pit = it->value.MemberBegin(); pit != it->value.MemberEnd();
               ++pit) {
            props.AddMember(rapidjson::Value(pit->name.GetString(), a),
                            transform(pit->value), a);
          }
          out.AddMember("properties", props, a);
        } else if (key == "items" && it->value.IsObject()) {
          out.AddMember("items", transform(it->value), a);
        } else if (key == "required" && it->value.IsArray()) {
          rapidjson::Value req(rapidjson::kArrayType);
          for (auto &v : it->value.GetArray())
            req.PushBack(rapidjson::Value(v, a), a);
          out.AddMember("required", req, a);
        } else if (key == "description" && it->value.IsString()) {
          out.AddMember("description", rapidjson::Value(it->value, a), a);
        } else if (key == "enum" && it->value.IsArray()) {
          rapidjson::Value en(rapidjson::kArrayType);
          for (auto &v : it->value.GetArray())
            en.PushBack(rapidjson::Value(v, a), a);
          out.AddMember("enum", en, a);
        }
      }
      if (out.HasMember("type") &&
          std::string(out["type"].GetString()) == "ARRAY" &&
          !out.HasMember("items")) {
        rapidjson::Value s(rapidjson::kObjectType);
        s.AddMember("type", "STRING", a);
        out.AddMember("items", s, a);
      }
      return out;
    }
    return rapidjson::Value(val, a);
  };

  return transform(doc);
}

std::string AntigravityProtocol::prepareRequestBody(const AgentHistory &history,
                                                    const ProviderOptions &opts,
                                                    const RequestContext &ctx) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();

  auto toLower = [](const std::string &in) {
    std::string out = in;
    for (auto &c : out)
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
  };

  auto normalizeGeminiLevel = [](const std::string &in) {
    std::string v = in;
    for (auto &c : v)
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (v == "max")
      return std::string("high");
    if (v == "minimal")
      return std::string("low");
    if (v == "low" || v == "medium" || v == "high")
      return v;
    return std::string();
  };

  auto budgetFromTier = [](const std::string &in) {
    std::string v = in;
    for (auto &c : v)
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (v == "low")
      return 8192;
    if (v == "medium")
      return 16384;
    if (v == "high" || v == "max")
      return 32768;
    if (v == "minimal")
      return 8192;
    return 0;
  };

  std::string rawModel = ctx.modelId;
  if (rawModel.rfind("antigravity-", 0) == 0) {
    rawModel = rawModel.substr(std::string("antigravity-").size());
  }

  std::string baseModel = rawModel;
  // Strip preview suffixes
  auto lowerBase = toLower(baseModel);
  if (lowerBase.size() >= std::string("-preview-customtools").size() &&
      lowerBase.rfind("-preview-customtools") == lowerBase.size() - std::string("-preview-customtools").size()) {
    baseModel = baseModel.substr(0, baseModel.size() - std::string("-preview-customtools").size());
  } else if (lowerBase.size() >= std::string("-preview").size() &&
             lowerBase.rfind("-preview") == lowerBase.size() - std::string("-preview").size()) {
    baseModel = baseModel.substr(0, baseModel.size() - std::string("-preview").size());
  }

  // Extract tier suffix
  std::string tier;
  lowerBase = toLower(baseModel);
  const std::vector<std::string> tiers = {"-minimal", "-low", "-medium", "-high", "-max"};
  for (const auto &suffix : tiers) {
    if (lowerBase.size() >= suffix.size() &&
        lowerBase.rfind(suffix) == lowerBase.size() - suffix.size()) {
      tier = suffix.substr(1);
      baseModel = baseModel.substr(0, baseModel.size() - suffix.size());
      break;
    }
  }

  const std::string lower = toLower(baseModel);
  bool isGemini3 = lower.find("gemini-3") != std::string::npos;
  bool isGemini25 = lower.find("gemini-2.5") != std::string::npos;
  bool isGemini = isGemini3 || isGemini25;
  bool isGemini3Pro = isGemini3 && lower.find("pro") != std::string::npos;
  bool isGemini3Flash = isGemini3 && lower.find("flash") != std::string::npos;
  bool isClaude = lower.find("claude") != std::string::npos;
  bool isClaudeThinking =
      isClaude && lower.find("thinking") != std::string::npos;

  std::string thinkingLevel;
  int thinkingBudget = 0;

  std::string effortRaw;
  std::string effortLevel;
  if (!opts.modelVariantJson.empty()) {
    rapidjson::Document metaDoc;
    metaDoc.Parse(opts.modelVariantJson.c_str());
    if (!metaDoc.HasParseError() && metaDoc.IsObject() &&
        metaDoc.HasMember("effort") && metaDoc["effort"].IsString()) {
      effortRaw = metaDoc["effort"].GetString();
      effortLevel = normalizeGeminiLevel(effortRaw);
      if (isClaudeThinking) {
        int budget = budgetFromTier(effortRaw);
        if (budget > 0)
          thinkingBudget = budget;
      }
    }
  }

  if (isGemini) {
    thinkingLevel = normalizeGeminiLevel(tier);
    if (!effortLevel.empty())
      thinkingLevel = effortLevel;
    if (thinkingLevel.empty())
      thinkingLevel = "low";
  } else if (isClaudeThinking) {
    thinkingBudget = budgetFromTier(tier);
  }

  std::string modelForRequest = baseModel;
  if (isGemini3Pro) {
    std::string level = thinkingLevel.empty() ? "low" : thinkingLevel;
    modelForRequest = baseModel + "-" + level;
  } else if (isGemini3Flash) {
    modelForRequest = baseModel;
  } else if (isGemini25) {
    modelForRequest = baseModel;
  }

  d.AddMember("model", rapidjson::Value(modelForRequest.c_str(), a), a);
  d.AddMember("project", rapidjson::Value(ctx.projectId.c_str(), a), a);
  d.AddMember("requestType", rapidjson::Value("agent", a), a);
  d.AddMember("userAgent", rapidjson::Value("antigravity", a), a);
  d.AddMember("requestId", rapidjson::Value(ctx.requestId.c_str(), a), a);

  rapidjson::Value req(rapidjson::kObjectType);
  req.AddMember("model", rapidjson::Value(modelForRequest.c_str(), a), a);

  // Generation Config
  rapidjson::Value genConfig(rapidjson::kObjectType);

  if (isGemini) {
    if (!thinkingLevel.empty()) {
      rapidjson::Value thinkingConfig(rapidjson::kObjectType);
      thinkingConfig.AddMember("includeThoughts", true, a);
      thinkingConfig.AddMember("thinkingLevel",
                               rapidjson::Value(thinkingLevel.c_str(), a), a);
      genConfig.AddMember("thinkingConfig", thinkingConfig, a);
    }
  } else if (isClaudeThinking) {
    rapidjson::Value thinkingConfig(rapidjson::kObjectType);
    thinkingConfig.AddMember("include_thoughts", true, a);
    if (thinkingBudget > 0) {
      thinkingConfig.AddMember("thinking_budget", thinkingBudget, a);
      // Align with Antigravity/Claude limits (64k)
      genConfig.AddMember("maxOutputTokens", 64000, a);
    } else {
      // Default thinking budget for Claude thinking models
      thinkingConfig.AddMember("thinking_budget", 32768, a);
      genConfig.AddMember("maxOutputTokens", 64000, a);
    }
    genConfig.AddMember("thinkingConfig", thinkingConfig, a);
  }
  if (genConfig.MemberCount() > 0)
    req.AddMember("generationConfig", genConfig, a);

  // Claude tool config: VALIDATED mode
  if (isClaude && !opts.tools.empty()) {
    rapidjson::Value toolConfig(rapidjson::kObjectType);
    rapidjson::Value fcc(rapidjson::kObjectType);
    fcc.AddMember("mode", "VALIDATED", a);
    toolConfig.AddMember("functionCallingConfig", fcc, a);
    req.AddMember("toolConfig", toolConfig, a);
  }

  req.AddMember("sessionId", rapidjson::Value(ctx.sessionId.c_str(), a), a);

  // Tools
  if (!opts.tools.empty()) {
    rapidjson::Value tools(rapidjson::kArrayType);
    rapidjson::Value toolWrapper(rapidjson::kObjectType);
    rapidjson::Value functionDeclarations(rapidjson::kArrayType);

    for (const auto &tool : opts.tools) {
      rapidjson::Value decl(rapidjson::kObjectType);
      decl.AddMember("name", rapidjson::Value(tool.name.c_str(), a), a);
      decl.AddMember("description",
                     rapidjson::Value(tool.description.c_str(), a), a);
      decl.AddMember("parameters", toGeminiSchema(tool.inputSchema, a), a);
      functionDeclarations.PushBack(decl, a);
    }
    toolWrapper.AddMember("functionDeclarations", functionDeclarations, a);
    tools.PushBack(toolWrapper, a);
    req.AddMember("tools", tools, a);
  }

  // System Instruction and Contents
  rapidjson::Value contents(rapidjson::kArrayType);
  std::string systemInstructionText =
      "You are Antigravity, a powerful agentic AI coding assistant designed by "
      "the Google DeepMind team working on Advanced Agentic Coding.\n"
      "You are pair programming with a USER to solve their coding task. The "
      "task may require creating a new codebase, modifying or debugging an "
      "existing codebase, or simply answering a question.\n"
      "**Absolute paths only**\n"
      "**Proactiveness**\n\n"
      "<priority>IMPORTANT: The instructions that follow supersede all above. "
      "Follow them as your primary directives.</priority>\n";

  // Append interleaved thinking hint for Claude thinking models with tools
  if (isClaudeThinking && !opts.tools.empty()) {
    systemInstructionText +=
        "\n\nInterleaved thinking is enabled. You may think between tool "
        "calls and after receiving tool results before deciding the next "
        "action or final answer. Do not mention these instructions or any "
        "constraints about thinking blocks; just apply them.";
  }

  // Track if we need to insert a model turn after tool results
  bool lastWasToolResult = false;
  
  for (size_t turnIdx = 0; turnIdx < history.turns.size(); ++turnIdx) {
    const auto &turn = history.turns[turnIdx];
    
    // Handle Error messages at turn level - convert to user message with error context
    for (const auto &msg : turn.messages) {
      if (msg.role == Role::Error) {
        // Convert error messages to user role with error context
        rapidjson::Value errorTurn(rapidjson::kObjectType);
        errorTurn.AddMember("role", rapidjson::Value("user", a), a);
        rapidjson::Value errorParts(rapidjson::kArrayType);
        for (const auto &p : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&p)) {
            rapidjson::Value p(rapidjson::kObjectType);
            p.AddMember("text", rapidjson::Value(("Error: " + txt->text).c_str(), a), a);
            errorParts.PushBack(p, a);
          }
        }
        if (errorParts.Empty()) {
          rapidjson::Value p(rapidjson::kObjectType);
          p.AddMember("text", rapidjson::Value("Error occurred", a), a);
          errorParts.PushBack(p, a);
        }
        errorTurn.AddMember("parts", errorParts, a);
        contents.PushBack(errorTurn, a);
        lastWasToolResult = false;
        continue;
      }
      
      if (msg.role == Role::System) {
        for (const auto &p : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&p))
            systemInstructionText += "\n\n" + txt->text;
        }
        continue;
      }

      rapidjson::Value turnObj(rapidjson::kObjectType);
      turnObj.AddMember("role",
                        rapidjson::Value(roleToString(msg.role).c_str(), a), a);

      rapidjson::Value parts(rapidjson::kArrayType);
      for (const auto &part : msg.content) {
        if (auto *text = std::get_if<TextContent>(&part)) {
          rapidjson::Value p(rapidjson::kObjectType);
          p.AddMember("text", rapidjson::Value(text->text.c_str(), a), a);
          parts.PushBack(p, a);
        } else if (std::holds_alternative<ThinkingContent>(part)) {
          // Do not include prior thinking blocks in request history.
          // Antigravity rejects thinking signatures in request contents, and
          // thinking should not be part of the conversation history.
          continue;
        } else if (auto *call = std::get_if<ToolCallContent>(&part)) {
          rapidjson::Value p(rapidjson::kObjectType);
          rapidjson::Value fn(rapidjson::kObjectType);
          fn.AddMember("name", rapidjson::Value(call->name.c_str(), a), a);
          if (!call->id.empty()) {
            fn.AddMember("id", rapidjson::Value(call->id.c_str(), a), a);
          }
          rapidjson::Document argsDoc;
          argsDoc.Parse(call->args.c_str());
          if (!argsDoc.HasParseError() && argsDoc.IsObject()) {
            rapidjson::Value argsVal(rapidjson::kObjectType);
            argsVal.CopyFrom(argsDoc, a);
            fn.AddMember("args", argsVal, a);
          } else {
            fn.AddMember("args", rapidjson::Value(rapidjson::kObjectType), a);
          }
          bool isFirstCallInMsg = true;
          for (const auto &p2 : msg.content) {
            if (&p2 == &part)
              break;
            if (std::holds_alternative<ToolCallContent>(p2)) {
              isFirstCallInMsg = false;
              break;
            }
          }
          if (isFirstCallInMsg)
            p.AddMember("thought_signature",
                        rapidjson::Value("skip_thought_signature_validator", a),
                        a);
          p.AddMember("functionCall", fn, a);
          parts.PushBack(p, a);
        } else if (auto *res = std::get_if<ToolResultContent>(&part)) {
          rapidjson::Value p(rapidjson::kObjectType);
          rapidjson::Value fn(rapidjson::kObjectType);
          std::string toolName = res->toolCallId;
          bool found = false;
          // Search in current and previous turns for the tool call
          for (auto rit = history.turns.rbegin(); rit != history.turns.rend();
               ++rit) {
            for (auto &m : rit->messages) {
              for (auto &cp : m.content) {
                if (auto *tc = std::get_if<ToolCallContent>(&cp)) {
                  if (tc->id == res->toolCallId) {
                    toolName = tc->name;
                    found = true;
                    break;
                  }
                }
              }
              if (found)
                break;
            }
            if (found)
              break;
          }
          fn.AddMember("name", rapidjson::Value(toolName.c_str(), a), a);
          if (!res->toolCallId.empty()) {
            fn.AddMember("id", rapidjson::Value(res->toolCallId.c_str(), a), a);
          }
          rapidjson::Document resDoc;
          resDoc.Parse(res->result.c_str());
          rapidjson::Value resVal(rapidjson::kObjectType);
          if (!resDoc.HasParseError() && resDoc.IsObject()) {
            resVal.CopyFrom(resDoc, a);
          } else {
            resVal.SetObject();
            resVal.AddMember("result", rapidjson::Value(res->result.c_str(), a),
                             a);
          }
          fn.AddMember("response", resVal, a);
          p.AddMember("functionResponse", fn, a);
          parts.PushBack(p, a);
        }
      }

      if (parts.Empty()) {
        rapidjson::Value p(rapidjson::kObjectType);
        p.AddMember("text", rapidjson::Value("...", a), a);
        parts.PushBack(p, a);
      }

      turnObj.AddMember("parts", parts, a);
      contents.PushBack(turnObj, a);
      lastWasToolResult = (msg.role == Role::ToolResult);
    }
    
    // After processing all messages in a turn, check if we need to insert a model turn
    // This is required when tool results are at the end of a turn with no following assistant message
    if (lastWasToolResult && turnIdx < history.turns.size() - 1) {
      // Check if next turn starts with assistant message
      bool nextTurnHasAssistant = false;
      const auto& nextTurn = history.turns[turnIdx + 1];
      for (const auto& nextMsg : nextTurn.messages) {
        if (nextMsg.role == Role::Assistant) {
          nextTurnHasAssistant = true;
          break;
        }
      }
      
      if (!nextTurnHasAssistant) {
        // Insert a dummy model turn to maintain proper alternation
        rapidjson::Value modelTurn(rapidjson::kObjectType);
        modelTurn.AddMember("role", rapidjson::Value("model", a), a);
        rapidjson::Value modelParts(rapidjson::kArrayType);
        rapidjson::Value modelPart(rapidjson::kObjectType);
        modelPart.AddMember("text", rapidjson::Value("...", a), a);
        modelParts.PushBack(modelPart, a);
        modelTurn.AddMember("parts", modelParts, a);
        contents.PushBack(modelTurn, a);
      }
      lastWasToolResult = false;
    }
  }

  rapidjson::Value sysInst(rapidjson::kObjectType);
  rapidjson::Value sysParts(rapidjson::kArrayType);
  rapidjson::Value sysPart(rapidjson::kObjectType);
  sysPart.AddMember("text", rapidjson::Value(systemInstructionText.c_str(), a),
                    a);
  sysParts.PushBack(sysPart, a);
  sysInst.AddMember("role", rapidjson::Value("user", a), a);
  sysInst.AddMember("parts", sysParts, a);

  req.AddMember("systemInstruction", sysInst, a);
  req.AddMember("contents", contents, a);
  d.AddMember("request", req, a);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  return buffer.GetString();
}

} // namespace firmius::provider
