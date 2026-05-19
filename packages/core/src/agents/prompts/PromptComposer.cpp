#include "agents/prompts/PromptComposer.hpp"

#include "ConfigLoader.hpp"
#include "agents/HintingLoader.hpp"
#include "agents/modes/Mode.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace firmius::core::prompts {

using firmius::shared::StringUtil;

namespace {

// Read entire file or return empty string if missing.
std::string readFileToString(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return {};
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// Apply {{KEY}} placeholder substitution.
std::string applyPlaceholders(std::string text,
                              const std::map<std::string, std::string> &p) {
  for (const auto &[key, value] : p) {
    std::size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
      text.replace(pos, key.length(), value);
      pos += value.length();
    }
  }
  return text;
}

// Build a stable {{...}} placeholder map. Mirrors legacy composeSystemPrompt
// so that migrated personas still see the same dynamic substitutions.
std::map<std::string, std::string>
buildBasePlaceholders(const PromptInputs &inputs,
                      const std::string &modelFamily,
                      const std::string &activeHintingName,
                      const std::string &purposeList) {
  std::map<std::string, std::string> placeholders;

  if (inputs.persona) {
    placeholders["{{AGENT_NAME}}"] = inputs.persona->name;
    placeholders["{{AGENT_TITLE}}"] = inputs.persona->title;
  } else {
    placeholders["{{AGENT_NAME}}"] = "agent";
    placeholders["{{AGENT_TITLE}}"] = "Agent";
  }

  if (inputs.context) {
    placeholders["{{CWD}}"] = inputs.context->environment.cwd;
  } else {
    placeholders["{{CWD}}"] = "";
  }

  placeholders["{{MODEL_FAMILY}}"] = modelFamily;
  placeholders["{{ACTIVE_HINTING_FILE}}"] = activeHintingName;
  placeholders["{{REGISTERED_PURPOSES}}"] = purposeList;
  placeholders["{{ACTIVE_MODE}}"] =
      inputs.activeModeName.value_or("none");

  // Custom placeholders registered via PurposeLoader::registerPlaceholder
  // are merged on top by the caller; keeping this map base-level only.
  return placeholders;
}

// Discover registered persona names (excluding base/compaction). Used by
// RegisteredPurposesSection and as a placeholder for delegate hints.
std::vector<std::string> listPurposeNames() {
  const std::string promptsDir = PurposeLoader::resolvePromptsDir();
  std::vector<std::string> names;
  if (!std::filesystem::exists(promptsDir)) {
    return names;
  }
  for (const auto &entry : std::filesystem::directory_iterator(promptsDir)) {
    if (entry.path().extension() != ".md") {
      continue;
    }
    const std::string stem = entry.path().stem().string();
    if (stem == "base" || stem == "COMPACTION_PROMPT") {
      continue;
    }
    names.push_back(stem);
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

std::string joinNames(const std::vector<std::string> &names) {
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    out += names[i];
    if (i + 1 < names.size()) {
      out += ", ";
    }
  }
  return out;
}

// Fetch hinting overlay if available; safe wrapper around the legacy loader.
std::optional<firmius::core::HintingOverlay>
loadHintingSafely(const PromptInputs &inputs) {
  if (!inputs.context) {
    return std::nullopt;
  }
  const auto &cfg = inputs.context->config;
  try {
    return HintingLoader::loadForModel(cfg.providerId, cfg.modelId,
                                       cfg.modelVariant);
  } catch (const std::exception &e) {
    std::cerr << "[hinting] Failed to load model hinting overlay: " << e.what()
              << "\n";
  } catch (...) {
    std::cerr << "[hinting] Failed to load model hinting overlay.\n";
  }
  return std::nullopt;
}

std::string detectModelFamily(const PromptInputs &inputs) {
  if (!inputs.context) {
    return "";
  }
  const auto &cfg = inputs.context->config;
  return ModelHintResolver::detectFamily(cfg.providerId, cfg.modelId,
                                         cfg.modelVariant);
}

// Normalise text trailing whitespace and ensure single trailing newline.
std::string trimTrailing(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                           text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  return text;
}

// Most-recently edited files extracted from AgentState. Best-effort.
std::vector<std::string> recentEditedFiles(const PromptInputs &inputs,
                                           std::size_t limit) {
  std::vector<std::string> out;
  if (!inputs.context) {
    return out;
  }
  const auto &edited = inputs.context->state.editedFiles;
  // editedFiles is append-order; take the tail.
  const std::size_t start = edited.size() > limit ? edited.size() - limit : 0;
  for (std::size_t i = start; i < edited.size(); ++i) {
    out.push_back(edited[i]);
  }
  return out;
}

std::vector<std::string> recentReadFiles(const PromptInputs &inputs,
                                         std::size_t limit) {
  std::vector<std::string> out;
  if (!inputs.context) {
    return out;
  }
  const auto &read = inputs.context->state.readFiles;
  const std::size_t start = read.size() > limit ? read.size() - limit : 0;
  for (std::size_t i = start; i < read.size(); ++i) {
    out.push_back(read[i]);
  }
  return out;
}

} // namespace

// ─── PromptComposer ─────────────────────────────────────────────────────────

PromptComposer::PromptComposer() = default;

void PromptComposer::addSection(int priority,
                                std::shared_ptr<IPromptSection> section) {
  entries_.push_back({priority, std::move(section)});
  std::stable_sort(
      entries_.begin(), entries_.end(),
      [](const Entry &a, const Entry &b) { return a.priority < b.priority; });
}

std::string PromptComposer::compose(const PromptInputs &inputs) const {
  std::string out;
  out.reserve(8192);

  for (const auto &entry : entries_) {
    std::string rendered = entry.section->render(inputs);
    rendered = trimTrailing(std::move(rendered));
    if (rendered.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += "\n\n";
    }
    out += rendered;
  }

  if (!out.empty()) {
    out += "\n";
  }
  return out;
}

PromptComposer PromptComposer::buildDefault() {
  PromptComposer composer;

  // Stable layers (cache-friendly, top of prompt).
  composer.addSection(10, std::make_shared<HouseDoctrineSection>());
  composer.addSection(20, std::make_shared<OutputStyleSection>());
  composer.addSection(30, std::make_shared<PersonaVoiceSection>());

  // Profile (Cairn) — slot reserved; renders empty until Day-3 fills it.
  composer.addSection(40, std::make_shared<ProfileSection>());

  // Hinting overlay — keeps legacy model-specific behavior patches working.
  composer.addSection(50, std::make_shared<HintingOverlaySection>());

  // Refreshable per-turn layers.
  composer.addSection(60, std::make_shared<EnvironmentSection>());
  composer.addSection(70, std::make_shared<RegisteredPurposesSection>());
  composer.addSection(80, std::make_shared<ActiveContextSection>());

  // Scoped frames — render only when their subsystem is active.
  composer.addSection(90, std::make_shared<ModeOverlaySection>());
  composer.addSection(100, std::make_shared<ModeFrameSection>());
  composer.addSection(110, std::make_shared<WorkflowFrameSection>());
  composer.addSection(120, std::make_shared<BranchFrameSection>());

  return composer;
}

// ─── Builtin sections ───────────────────────────────────────────────────────

std::string HouseDoctrineSection::render(const PromptInputs &inputs) const {
  const std::string promptsDir = PurposeLoader::resolvePromptsDir();
  std::string body = readFileToString(promptsDir + "base.md");
  if (body.empty()) {
    return {};
  }

  const std::string modelFamily = detectModelFamily(inputs);
  const auto hinting = loadHintingSafely(inputs);
  const std::string hintingName = hinting ? hinting->name : "none";
  const std::string purposeList = joinNames(listPurposeNames());

  auto placeholders =
      buildBasePlaceholders(inputs, modelFamily, hintingName, purposeList);
  // Custom placeholders bolt onto whatever the loader has registered.
  // We re-implement the legacy injection here so that callers using
  // PurposeLoader::registerPlaceholder still see their values.
  // (PurposeLoader::composeSystemPrompt threads them in.)
  body = applyPlaceholders(std::move(body), placeholders);
  return body;
}

std::string OutputStyleSection::render(const PromptInputs &) const {
  // Direct, structured, evidence-anchored. These rules survive every
  // persona swap and shape every response. Kept under 350 tokens.
  return R"(# OUTPUT STYLE

- Direct response. No "Sure!", no "Absolutely!", no apologies before answering. Begin with the substance.
- Markdown for structure: short paragraphs, headings only when they help, bold for the title of every list item.
- Code in fenced blocks with a language tag.
- File references use the @/abs/path:start-end format, e.g. `@/work/foo.cpp:12-20`. For a single line: `@/work/foo.cpp:14`.
- Cite every factual claim about repository state with a file reference. No claim survives without an anchor.
- When uncertain, name the uncertainty and pick the next probe. Never wave hands.
- A tool call may carry a one-line preface ("running tests now…") but never long narrative; keep prose between turns, not between tool blocks within a turn.
- No emoji unless the user uses them first. No "I'm just an AI" disclaimers.

## Anti-patterns
- Discovery Theater — creating a chunk just to "investigate" what a single read would resolve.
- Vibe-Acceptance — trusting a sibling's "I think it works" without running the proof.
- Narrative Bloat — five paragraphs of plan when one tool call would land it.
- Todo Amnesia — issuing a summary while a todo is still `[ ]` or `[*]`.
- Ghost Churn — marking a task Done while a background process is still live.)";
}

std::string PersonaVoiceSection::render(const PromptInputs &inputs) const {
  if (!inputs.persona || inputs.persona->identityPrompt.empty()) {
    return {};
  }

  std::string body = "# AGENT IDENTITY\n";
  body += inputs.persona->identityPrompt;
  return body;
}

std::string ProfileSection::render(const PromptInputs &inputs) const {
  // Day-3 stub. When Cairn lands, this renders the user's high-confidence
  // Hearth + Grove facts as a stable prompt-cache-friendly block.
  // Returning empty keeps the prompt clean until then.
  (void)inputs;
  return {};
}

std::string HintingOverlaySection::render(const PromptInputs &inputs) const {
  const auto hinting = loadHintingSafely(inputs);
  if (!hinting) {
    return {};
  }
  const std::string family = detectModelFamily(inputs);

  std::string out = "# MODEL-SPECIFIC HINTING\n";
  out += "Detected Family: " + family + "\n";
  out += "Hinting File: " + hinting->name + "\n\n";
  out += hinting->body;
  return out;
}

std::string EnvironmentSection::render(const PromptInputs &inputs) const {
  if (!inputs.context) {
    return {};
  }
  const auto &ctx = *inputs.context;

  std::ostringstream ss;
  ss << "# ENVIRONMENT\n";
  ss << "Host: " << ctx.environment.identifier << "\n";
  ss << "CWD: " << ctx.environment.cwd << "\n";
  ss << "Model: " << ctx.config.modelId;
  if (!ctx.config.modelVariant.empty()) {
    ss << " (variant: " << ctx.config.modelVariant << ")";
  }
  ss << "\n";

  const auto recentEdits = recentEditedFiles(inputs, 8);
  if (!recentEdits.empty()) {
    ss << "Recently edited (last " << recentEdits.size() << "): ";
    for (std::size_t i = 0; i < recentEdits.size(); ++i) {
      ss << recentEdits[i];
      if (i + 1 < recentEdits.size()) {
        ss << ", ";
      }
    }
    ss << "\n";
  }

  const auto recentReads = recentReadFiles(inputs, 8);
  if (!recentReads.empty()) {
    ss << "Recently read (last " << recentReads.size() << "): ";
    for (std::size_t i = 0; i < recentReads.size(); ++i) {
      ss << recentReads[i];
      if (i + 1 < recentReads.size()) {
        ss << ", ";
      }
    }
    ss << "\n";
  }

  // Model route categories from config — preserved from legacy compose.
  const auto &cfg = shared::ConfigLoader::instance().getConfig();
  if (!cfg.modelRouterCategories.empty()) {
    std::vector<std::string> categoryNames;
    categoryNames.reserve(cfg.modelRouterCategories.size());
    for (const auto &[name, _] : cfg.modelRouterCategories) {
      categoryNames.push_back(name);
    }
    std::sort(categoryNames.begin(), categoryNames.end());
    ss << "Model Route Categories: " << joinNames(categoryNames) << "\n";
  }
  if (!cfg.defaultRouteCategory.empty()) {
    ss << "Default Route Category: " << cfg.defaultRouteCategory << "\n";
  }

  return ss.str();
}

std::string ActiveContextSection::render(const PromptInputs &inputs) const {
  if (!inputs.context) {
    return {};
  }
  const auto &state = inputs.context->state;
  if (state.completedActions.empty() && state.fullyReadFiles.empty()) {
    return {};
  }

  std::ostringstream ss;
  ss << "# ACTIVE CONTEXT\n";
  if (!state.completedActions.empty()) {
    ss << "Completed actions (last 5):\n";
    const std::size_t start =
        state.completedActions.size() > 5 ? state.completedActions.size() - 5
                                          : 0;
    for (std::size_t i = start; i < state.completedActions.size(); ++i) {
      ss << "- " << state.completedActions[i] << "\n";
    }
  }
  if (!state.fullyReadFiles.empty()) {
    ss << "Fully read this session: " << state.fullyReadFiles.size()
       << " file(s).\n";
  }
  return ss.str();
}

std::string ModeOverlaySection::render(const PromptInputs &inputs) const {
  if (!inputs.activeModeName.has_value() ||
      inputs.activeModeName->empty()) {
    return {};
  }

  // Resolve through the in-memory ModeRegistry so persona-scoped sub-modes
  // (forge:apply, ember:capture, …) resolve correctly. The registry already
  // walked prompts/modes/ + prompts/modes/<persona>/ on instance load.
  const std::string personaName =
      inputs.persona ? inputs.persona->name : std::string{};
  const auto *mode = modes::ModeRegistry::instance().resolveForPersona(
      *inputs.activeModeName, personaName);
  if (mode == nullptr || mode->promptOverlay.empty()) {
    return {};
  }

  std::string out = "# MODE OVERLAY (" + mode->qualifiedName() + ")\n";
  out += StringUtil::trim(mode->promptOverlay);

  // If the sub-mode declares a parent system mode, pull that overlay in
  // as additional context. forge:apply with parent_mode: execute means
  // the agent sees both forge's apply guidance and the system execute
  // doctrine in the same prompt slice.
  if (mode->parentMode.has_value() && !mode->parentMode->empty()) {
    if (const auto *parent =
            modes::ModeRegistry::instance().find(*mode->parentMode)) {
      if (!parent->promptOverlay.empty()) {
        out += "\n\n## PARENT MODE OVERLAY (" + parent->qualifiedName() + ")\n";
        out += StringUtil::trim(parent->promptOverlay);
      }
    }
  }
  return out;
}

std::string ModeFrameSection::render(const PromptInputs &inputs) const {
  if (!inputs.activeModeName.has_value() ||
      inputs.activeModeName->empty()) {
    return {};
  }

  const std::string personaName =
      inputs.persona ? inputs.persona->name : std::string{};
  const auto *mode = modes::ModeRegistry::instance().resolveForPersona(
      *inputs.activeModeName, personaName);

  std::ostringstream ss;
  ss << "# MODE FRAME\n";
  if (mode != nullptr) {
    ss << "Active mode: " << mode->qualifiedName();
    if (!mode->title.empty() && mode->title != mode->name) {
      ss << " (" << mode->title << ")";
    }
    ss << "\n";
    if (!mode->shortDescription.empty()) {
      ss << "Stance: " << mode->shortDescription << "\n";
    }
    if (!mode->allowedTransitionsTo.empty()) {
      ss << "Allowed next modes: ";
      for (std::size_t i = 0; i < mode->allowedTransitionsTo.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << mode->allowedTransitionsTo[i];
      }
      ss << "\n";
    }
    if (mode->outputSchema.has_value() && !mode->outputSchema->empty()) {
      ss << "Expected return shape: " << *mode->outputSchema << "\n";
    }
  } else {
    ss << "Active mode: " << *inputs.activeModeName << "\n";
  }
  ss << "To change mode: call mode_switch(name) with a registered mode "
        "(qualified `persona:submode` or bare name). Transitions take effect "
        "on the next turn boundary.\n";
  return ss.str();
}

std::string WorkflowFrameSection::render(const PromptInputs &inputs) const {
  if (!inputs.activeWorkflowName.has_value() ||
      inputs.activeWorkflowName->empty()) {
    return {};
  }
  std::ostringstream ss;
  ss << "# WORKFLOW FRAME\n";
  ss << "Active workflow: " << *inputs.activeWorkflowName << "\n";
  return ss.str();
}

std::string BranchFrameSection::render(const PromptInputs &inputs) const {
  if (!inputs.branchLineage.has_value() || inputs.branchLineage->empty()) {
    return {};
  }
  std::ostringstream ss;
  ss << "# BRANCH FRAME\n";
  ss << "Lineage: " << *inputs.branchLineage << "\n";
  ss << "You are operating as a delegated subagent. Use delegate_return "
        "with the structured payload required by your parent's contract.\n";
  return ss.str();
}

std::string RegisteredPurposesSection::render(const PromptInputs &inputs) const {
  (void)inputs;
  const auto names = listPurposeNames();
  if (names.empty()) {
    return {};
  }
  std::string out =
      "Purposes Registered (use these in summon_subagent): " + joinNames(names);
  return out;
}

} // namespace firmius::core::prompts
