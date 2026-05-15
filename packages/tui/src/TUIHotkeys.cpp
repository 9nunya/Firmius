#include "TUIHotkeys.hpp"

#include "NotificationManager.hpp"
#include "components/SyntaxHighlighter.hpp"
#include "utils/PlatformPaths.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <set>
#include <sstream>
#include <vector>

namespace firmius::tui {

namespace {

using rapidjson::Value;

constexpr const char *kHotkeysConfigFileName = "hotkeys.json";

std::string trimCopy(std::string text) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  text.erase(text.begin(),
             std::find_if(text.begin(), text.end(),
                          [&](char ch) { return !is_space(ch); }));
  text.erase(std::find_if(text.rbegin(), text.rend(),
                          [&](char ch) { return !is_space(ch); })
                 .base(),
             text.end());
  return text;
}

std::string upperCopy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return text;
}

std::string canonicalizeHotkeyLabel(std::string label) {
  std::string trimmed = trimCopy(std::move(label));
  if (trimmed.empty()) {
    return {};
  }

  std::string normalized;
  normalized.reserve(trimmed.size());
  for (char ch : trimmed) {
    if (ch == '-' || ch == '_') {
      normalized.push_back('+');
    } else if (!std::isspace(static_cast<unsigned char>(ch))) {
      normalized.push_back(ch);
    }
  }

  std::vector<std::string> raw_parts;
  std::stringstream stream(normalized);
  std::string piece;
  while (std::getline(stream, piece, '+')) {
    piece = upperCopy(trimCopy(piece));
    if (!piece.empty()) {
      raw_parts.push_back(std::move(piece));
    }
  }

  const auto canonical_modifier = [](const std::string &part)
      -> std::optional<std::string> {
    if (part == "CTRL" || part == "CONTROL" || part == "CTL")
      return std::string("CTRL");
    if (part == "SHIFT" || part == "SHFT")
      return std::string("SHIFT");
    if (part == "ALT" || part == "OPTION" || part == "OPT")
      return std::string("ALT");
    if (part == "META" || part == "CMD" || part == "COMMAND" ||
        part == "SUPER")
      return std::string("META");
    return std::nullopt;
  };

  std::vector<std::string> modifiers;
  std::string base;
  for (const auto &part : raw_parts) {
    if (auto mod = canonical_modifier(part)) {
      if (std::find(modifiers.begin(), modifiers.end(), *mod) ==
          modifiers.end()) {
        modifiers.push_back(*mod);
      }
      continue;
    }
    if (!base.empty()) {
      return {};
    }
    base = part;
  }

  if (base.empty()) {
    return {};
  }

  if (base.size() == 1 && std::isalpha(static_cast<unsigned char>(base[0]))) {
    base[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
  } else if (base[0] == 'F' && base.size() > 1) {
    bool digits = std::all_of(base.begin() + 1, base.end(), [](unsigned char ch) {
      return std::isdigit(ch) != 0;
    });
    if (!digits) {
      return {};
    }
  } else if (base == "ESC") {
    base = "ESC";
  } else if (base == "ENTER" || base == "RETURN") {
    base = "ENTER";
  } else if (base == "TAB") {
    base = "TAB";
  } else if (base == "BACKSPACE" || base == "BS") {
    base = "BACKSPACE";
  } else if (base == "PGUP" || base == "PAGEUP") {
    base = "PGUP";
  } else if (base == "PGDN" || base == "PAGEDOWN") {
    base = "PGDN";
  } else if (base == "HOME" || base == "END" || base == "UP" ||
             base == "DOWN" || base == "LEFT" || base == "RIGHT") {
  } else {
    return {};
  }

  static const std::vector<std::string> modifier_order = {"CTRL", "ALT",
                                                           "SHIFT", "META"};
  std::vector<std::string> ordered;
  for (const auto &name : modifier_order) {
    if (std::find(modifiers.begin(), modifiers.end(), name) != modifiers.end()) {
      ordered.push_back(name);
    }
  }
  ordered.push_back(base);

  std::ostringstream out;
  for (size_t i = 0; i < ordered.size(); ++i) {
    if (i > 0) {
      out << '+';
    }
    out << ordered[i];
  }
  return out.str();
}

std::string displayLabelFromCanonical(const std::string &canonical) {
  if (canonical.empty()) {
    return {};
  }
  std::stringstream stream(canonical);
  std::string part;
  std::vector<std::string> display_parts;
  while (std::getline(stream, part, '+')) {
    if (part == "CTRL")
      display_parts.push_back("Ctrl");
    else if (part == "ALT")
      display_parts.push_back("Alt");
    else if (part == "SHIFT")
      display_parts.push_back("Shift");
    else if (part == "META")
      display_parts.push_back("Meta");
    else if (part == "PGUP")
      display_parts.push_back("PgUp");
    else if (part == "PGDN")
      display_parts.push_back("PgDn");
    else if (part == "ESC")
      display_parts.push_back("Esc");
    else if (part == "ENTER")
      display_parts.push_back("Enter");
    else if (part == "BACKSPACE")
      display_parts.push_back("Backspace");
    else if (part.size() == 1)
      display_parts.push_back(part);
    else
      display_parts.push_back(part);
  }

  std::ostringstream out;
  for (size_t i = 0; i < display_parts.size(); ++i) {
    if (i > 0) {
      out << '+';
    }
    out << display_parts[i];
  }
  return out.str();
}

std::filesystem::path hotkeysConfigPath() {
  return firmius::shared::PlatformPaths::firmiusHomeDir() / kHotkeysConfigFileName;
}

bool parseJsonObjectFile(const std::filesystem::path &path, rapidjson::Document &doc) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  doc.Parse(content.c_str());
  return !doc.HasParseError() && doc.IsObject();
}

std::vector<HotkeyBinding> buildBindingsFromLabels(
    const std::map<HotkeyAction, std::string> &labels) {
  std::vector<HotkeyBinding> bindings;
  bindings.reserve(labels.size());
  for (const auto &spec : HotkeyBindingSpecs()) {
    auto it = labels.find(spec.action);
    if (it != labels.end()) {
      bindings.push_back({spec.action, displayLabelFromCanonical(it->second)});
    }
  }
  return bindings;
}


std::map<HotkeyAction, std::string> canonicalLabelsFromBindings(
    const std::vector<HotkeyBinding> &bindings,
    std::vector<std::string> &warnings) {
  std::map<HotkeyAction, std::string> canonical_labels;
  for (const auto &binding : bindings) {
    auto canonical = ParseHotkeyLabel(binding.label);
    if (!canonical.has_value()) {
      warnings.push_back(std::string("Ignoring hotkey '") +
                         HotkeyActionName(binding.action) +
                         "': unrecognized label '" + binding.label + "'.");
      continue;
    }
    canonical_labels[binding.action] = *canonical;
  }
  return canonical_labels;
}

void writeHotkeyConfigDocument(
    const std::map<HotkeyAction, std::string> &canonical_labels,
    rapidjson::Document &doc) {
  doc.SetObject();
  auto &alloc = doc.GetAllocator();

  rapidjson::Value bindings(rapidjson::kObjectType);
  for (const auto &spec : HotkeyBindingSpecs()) {
    auto it = canonical_labels.find(spec.action);
    const std::string label =
        it != canonical_labels.end() ? displayLabelFromCanonical(it->second)
                                     : spec.default_label;
    bindings.AddMember(rapidjson::Value(spec.config_key, alloc).Move(),
                       rapidjson::Value(label.c_str(), alloc).Move(), alloc);
  }
  doc.AddMember("bindings", std::move(bindings), alloc);
}

bool saveHotkeyConfigDocument(const rapidjson::Document &doc) {
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  writer.SetIndent(' ', 2);
  doc.Accept(writer);

  std::ofstream out(hotkeysConfigPath());
  if (!out.is_open()) {
    return false;
  }

  out << buffer.GetString() << '\n';
  return true;
}

HotkeyConfig &liveHotkeyConfig() {
  static HotkeyConfig config = LoadHotkeyConfig();
  return config;
}
std::map<HotkeyAction, std::string> defaultCanonicalBindings() {
  std::map<HotkeyAction, std::string> labels;
  for (const auto &spec : HotkeyBindingSpecs()) {
    if (auto canonical = ParseHotkeyLabel(spec.default_label)) {
      labels.emplace(spec.action, *canonical);
    }
  }
  return labels;
}

bool isCtrlLetterInput(const std::string &raw, char lower, char upper,
                       const std::vector<std::string> &knownSequences) {
  for (const auto &sequence : knownSequences) {
    if (raw == sequence) {
      return true;
    }
  }

  if (raw.size() > 5 && raw.rfind("\x1b[", 0) == 0 && raw.back() == 'u') {
    auto semi = raw.find(';', 2);
    if (semi != std::string::npos) {
      try {
        int codepoint = std::stoi(raw.substr(2, semi - 2));
        int modifier = std::stoi(raw.substr(semi + 1, raw.size() - semi - 2));
        bool is_match = codepoint == lower || codepoint == upper;
        bool has_ctrl = (modifier & 4) != 0;
        if (is_match && has_ctrl) {
          return true;
        }
      } catch (...) {
      }
    }
  }

  return false;
}

bool isFunctionKeyInput(const std::string &raw, int fn_number) {
  if (fn_number == 1) {
    return raw == "\x1bOP" || raw == "\x1b[11~";
  }
  return false;
}

bool isQuestionInput(const std::string &raw) { return raw == "?"; }

bool matchesCanonicalBinding(const std::string &canonical, const ftxui::Event &event,
                             const std::string &raw) {
  if (canonical == "F1") {
    return event == ftxui::Event::F1 || isFunctionKeyInput(raw, 1);
  }
  if (canonical == "CTRL+SHIFT+P") {
    return raw == "\x10" || raw == "\x1b[80;6u" || raw == "\x1b[112;6u";
  }
  if (canonical == "CTRL+L") {
    return event == ftxui::Event::Special("\x0C") ||
           isCtrlLetterInput(raw, 'l', 'L',
                             {"\x0C", "\x1b[12;5u", "\x1b[27;5;108~",
                              "\x1b[76;5u"});
  }
  if (canonical == "CTRL+R") {
    return event == ftxui::Event::Special("\x12") ||
           isCtrlLetterInput(raw, 'r', 'R',
                             {"\x12", "\x1b[18;5u", "\x1b[27;5;114~",
                              "\x1b[82;5u"});
  }
  if (canonical == "CTRL+K") {
    return event == ftxui::Event::Special("\x0B") ||
           isCtrlLetterInput(raw, 'k', 'K',
                             {"\x0B", "\x1b[11;5u", "\x1b[27;5;107~",
                              "\x1b[75;5u"});
  }
  if (canonical == "CTRL+Z") {
    return event == ftxui::Event::Special("\x1A") ||
           isCtrlLetterInput(raw, 'z', 'Z',
                             {"\x1A", "\x1b[26;5u", "\x1b[27;5;122~",
                              "\x1b[90;5u"});
  }
  if (canonical == "CTRL+SHIFT+Z") {
    return raw == "\x1b[26;6u" || raw == "\x1b[90;6u";
  }
  if (canonical == "ALT+BACKSPACE") {
    return raw == "\x1b\x7f" || raw == "\x1b\b";
  }
  if (canonical == "CTRL+ALT+Z") {
    return raw == "\x1b[26;7u" || raw == "\x1b[90;7u";
  }
  if (canonical == "CTRL+ALT+SHIFT+Z") {
    return raw == "\x1b[26;8u" || raw == "\x1b[90;8u";
  }
  if (canonical == "SHIFT+?") {
    return isQuestionInput(raw);
  }
  return false;
}

} // namespace

const std::vector<HotkeyBindingSpec> &HotkeyBindingSpecs() {
  static const std::vector<HotkeyBindingSpec> specs = {
      {HotkeyAction::OpenHelp, "open_help", kOpenHelpHotkeyLabel,
       "Input + UI", "Open help from anywhere"},
      {HotkeyAction::OpenCommandPalette, "open_command_palette",
       kOpenCommandPaletteHotkeyLabel, "Input + UI",
       "Open command palette / launcher"},
      {HotkeyAction::ModeCycle, "mode_cycle", kModeCycleHotkeyLabel,
       "Agent Control", "Cycle active mode on the lead/focused agent"},
      {HotkeyAction::PermissionCycle, "permission_cycle",
       kPermissionCycleHotkeyLabel, "Agent Control",
       "Cycle thread permissions"},
      {HotkeyAction::RetryLastRequest, "retry_last_request",
       kRetryLastRequestHotkeyLabel, "Agent Control",
       "Retry/resume the stopped focused agent"},
      {HotkeyAction::VariantCycle, "variant_cycle", kVariantCycleHotkeyLabel,
       "Agent Control", "Cycle model variant on focused agent"},
      {HotkeyAction::TranscriptUndo, "transcript_undo",
       kTranscriptUndoHotkeyLabel, "Input + UI", "Undo last agent turn"},
      {HotkeyAction::TranscriptRedo, "transcript_redo",
       kTranscriptRedoHotkeyLabel, "Input + UI",
       "Redo last transcript undo"},
      {HotkeyAction::TranscriptUndoToUserBoundary,
       "transcript_undo_to_user_boundary",
       kTranscriptUndoToUserBoundaryHotkeyLabel, "Input + UI",
       "Undo to last user message"},
      {HotkeyAction::EditUndo, "edit_undo", kEditUndoHotkeyLabel,
       "Input + UI", "Undo last persisted file edit batch"},
      {HotkeyAction::EditRedo, "edit_redo", kEditRedoHotkeyLabel,
       "Input + UI", "Redo last persisted file edit undo"},
  };
  return specs;
}

std::vector<HotkeyBinding> DefaultHotkeyBindings() {
  return buildBindingsFromLabels(defaultCanonicalBindings());
}

std::optional<std::string> ParseHotkeyLabel(std::string label) {
  const std::string canonical = canonicalizeHotkeyLabel(std::move(label));
  if (canonical.empty()) {
    return std::nullopt;
  }
  return canonical;
}

bool SaveDefaultHotkeyConfigIfMissing() {
  const auto path = hotkeysConfigPath();
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return false;
  }
  std::filesystem::create_directories(path.parent_path(), ec);

  rapidjson::Document doc;
  writeHotkeyConfigDocument(defaultCanonicalBindings(), doc);
  return saveHotkeyConfigDocument(doc);
}

HotkeyConfig LoadHotkeyConfig() {
  SaveDefaultHotkeyConfigIfMissing();

  HotkeyConfig config;
  std::map<HotkeyAction, std::string> canonical_labels = defaultCanonicalBindings();
  std::set<std::string> seen_labels;
  rapidjson::Document doc;
  if (parseJsonObjectFile(hotkeysConfigPath(), doc) && doc.HasMember("bindings") &&
      doc["bindings"].IsObject()) {
    const auto &bindings = doc["bindings"];
    for (const auto &spec : HotkeyBindingSpecs()) {
      if (!bindings.HasMember(spec.config_key)) {
        continue;
      }
      const auto &value = bindings[spec.config_key];
      if (!value.IsString()) {
        config.warnings.push_back(std::string("Ignoring hotkey '") +
                                  spec.config_key + "': value must be a string.");
        continue;
      }
      auto canonical = ParseHotkeyLabel(value.GetString());
      if (!canonical.has_value()) {
        config.warnings.push_back(std::string("Ignoring hotkey '") +
                                  spec.config_key + "': unrecognized label '" +
                                  value.GetString() + "'.");
        continue;
      }
      if (!seen_labels.insert(*canonical).second) {
        config.warnings.push_back(std::string("Ignoring hotkey '") +
                                  spec.config_key + "': duplicate binding '" +
                                  displayLabelFromCanonical(*canonical) + "'.");
        continue;
      }
      canonical_labels[spec.action] = *canonical;
    }
  }

  config.bindings = buildBindingsFromLabels(canonical_labels);
  return config;
}

std::vector<HotkeyConflict>
FindHotkeyConflicts(const std::vector<HotkeyBinding> &bindings) {
  std::map<std::string, std::vector<HotkeyAction>> grouped;
  for (const auto &binding : bindings) {
    auto canonical = ParseHotkeyLabel(binding.label);
    if (!canonical.has_value()) {
      continue;
    }
    grouped[*canonical].push_back(binding.action);
  }

  std::vector<HotkeyConflict> conflicts;
  for (const auto &[canonical, actions] : grouped) {
    if (actions.size() > 1) {
      conflicts.push_back({displayLabelFromCanonical(canonical), actions});
    }
  }
  return conflicts;
}

bool SaveHotkeyConfig(const std::vector<HotkeyBinding> &bindings,
                      std::vector<std::string> *warnings) {
  std::vector<std::string> local_warnings;
  auto canonical_labels = canonicalLabelsFromBindings(bindings, local_warnings);
  const auto conflicts = FindHotkeyConflicts(bindings);
  for (const auto &conflict : conflicts) {
    local_warnings.push_back(std::string("Duplicate binding '") + conflict.label +
                             "' is assigned to multiple actions.");
  }
  if (!conflicts.empty()) {
    if (warnings) {
      *warnings = std::move(local_warnings);
    }
    return false;
  }

  rapidjson::Document doc;
  writeHotkeyConfigDocument(canonical_labels, doc);
  const bool saved = saveHotkeyConfigDocument(doc);
  if (!saved) {
    local_warnings.push_back("Failed to write hotkey config file.");
  }
  if (warnings) {
    *warnings = std::move(local_warnings);
  }
  return saved;
}

bool ReloadHotkeyConfig(std::vector<std::string> *warnings) {
  liveHotkeyConfig() = LoadHotkeyConfig();
  if (warnings) {
    *warnings = liveHotkeyConfig().warnings;
  }
  return true;
}

std::vector<HotkeyBinding> ResetHotkeyBindingsToDefaults(
    std::vector<std::string> *warnings) {
  auto bindings = DefaultHotkeyBindings();
  SaveHotkeyConfig(bindings, warnings);
  return bindings;
}

const HotkeyBinding *FindHotkeyBinding(HotkeyAction action) {
  const auto &config = liveHotkeyConfig();
  for (const auto &binding : config.bindings) {
    if (binding.action == action) {
      return &binding;
    }
  }
  return nullptr;
}

std::string GetHotkeyLabel(HotkeyAction action) {
  if (const auto *binding = FindHotkeyBinding(action)) {
    return binding->label;
  }
  for (const auto &spec : HotkeyBindingSpecs()) {
    if (spec.action == action) {
      return spec.default_label;
    }
  }
  return {};
}

std::string HotkeyActionName(HotkeyAction action) {
  for (const auto &spec : HotkeyBindingSpecs()) {
    if (spec.action == action) {
      return spec.config_key;
    }
  }
  return "unknown";
}


std::string HotkeyActionDescription(HotkeyAction action) {
  for (const auto &spec : HotkeyBindingSpecs()) {
    if (spec.action == action) {
      return spec.description;
    }
  }
  return "Unknown hotkey action";
}
bool IsHotkeyActionInput(HotkeyAction action, const std::string &raw) {
  if (const auto *binding = FindHotkeyBinding(action)) {
    if (auto canonical = ParseHotkeyLabel(binding->label)) {
      return matchesCanonicalBinding(*canonical, ftxui::Event::Special(raw), raw);
    }
  }
  for (const auto &spec : HotkeyBindingSpecs()) {
    if (spec.action == action) {
      if (auto canonical = ParseHotkeyLabel(spec.default_label)) {
        return matchesCanonicalBinding(*canonical, ftxui::Event::Special(raw), raw);
      }
    }
  }
  return false;
}

bool IsHotkeyActionEvent(HotkeyAction action, const ftxui::Event &event) {
  const std::string raw = event.input();
  if (const auto *binding = FindHotkeyBinding(action)) {
    if (auto canonical = ParseHotkeyLabel(binding->label)) {
      return matchesCanonicalBinding(*canonical, event, raw);
    }
  }
  for (const auto &spec : HotkeyBindingSpecs()) {
    if (spec.action == action) {
      if (auto canonical = ParseHotkeyLabel(spec.default_label)) {
        return matchesCanonicalBinding(*canonical, event, raw);
      }
    }
  }
  return false;
}

bool IsOpenHelpInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::OpenHelp, raw);
}

bool IsOpenHelpEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::OpenHelp, event);
}

bool IsOpenCommandPaletteInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::OpenCommandPalette, raw);
}

bool IsOpenCommandPaletteEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::OpenCommandPalette, event);
}

bool IsModeCycleInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::ModeCycle, raw);
}

bool IsModeCycleEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::ModeCycle, event);
}

bool IsPermissionCycleInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::PermissionCycle, raw);
}

bool IsPermissionCycleEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::PermissionCycle, event);
}

bool IsRetryLastRequestInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::RetryLastRequest, raw);
}

bool IsRetryLastRequestEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::RetryLastRequest, event);
}

bool IsVariantCycleInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::VariantCycle, raw);
}

bool IsVariantCycleEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::VariantCycle, event);
}

bool IsTranscriptUndoInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::TranscriptUndo, raw);
}

bool IsTranscriptUndoEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::TranscriptUndo, event);
}

bool IsTranscriptRedoInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::TranscriptRedo, raw);
}

bool IsTranscriptRedoEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::TranscriptRedo, event);
}

bool IsTranscriptUndoToUserBoundaryInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::TranscriptUndoToUserBoundary, raw);
}

bool IsTranscriptUndoToUserBoundaryEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::TranscriptUndoToUserBoundary, event);
}

bool IsEditUndoInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::EditUndo, raw);
}

bool IsEditUndoEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::EditUndo, event);
}

bool IsEditRedoInput(const std::string &raw) {
  return IsHotkeyActionInput(HotkeyAction::EditRedo, raw);
}

bool IsEditRedoEvent(const ftxui::Event &event) {
  return IsHotkeyActionEvent(HotkeyAction::EditRedo, event);
}

} // namespace firmius::tui
