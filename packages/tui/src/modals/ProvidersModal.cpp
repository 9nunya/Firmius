#include "modals/ProvidersModal.hpp"

#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/AccountsModal.hpp"
#include "modals/ModalLayout.hpp"
#include "modals/QuotasModal.hpp"
#include "providers/ProviderRegistry.hpp"

#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace firmius::tui {

namespace {

// ---------------------------------------------------------------------------
// Modal state machine
// ---------------------------------------------------------------------------

enum class Mode {
  Browse,
  TemplatePicker,
  EditCustom,
  EditBuiltin,
  Variants,
  ConfirmDelete,
};

// ---------------------------------------------------------------------------
// Provider list row (merged registry + config view)
// ---------------------------------------------------------------------------

struct ProviderRow {
  std::string id;
  bool isCustom = false; // true if backed by cfg.providers
  std::string kind;      // "openai_compatible", "anthropic", "oauth", "apikey"
  bool enabled = true;
};

std::vector<ProviderRow> buildRows() {
  const auto cfg = firmius::core::Harness::instance().getConfig();
  std::map<std::string, ProviderRow> merged;

  // Built-in providers (registry).
  for (const auto &id :
       firmius::provider::ProviderRegistry::instance().listProviderIds()) {
    auto provider =
        firmius::provider::ProviderRegistry::instance().getProvider(id);
    ProviderRow row;
    row.id = id;
    row.isCustom = false;
    if (provider) {
      row.kind = provider->getProviderType() ==
                         firmius::provider::ProviderType::OAuth
                     ? "oauth"
                     : "apikey";
      row.enabled = provider->isConfigured();
    } else {
      row.kind = "?";
      row.enabled = false;
    }
    merged[id] = row;
  }

  // Custom providers from config override / add to merged set.
  for (const auto &[id, profile] : cfg.providers) {
    ProviderRow row;
    row.id = id;
    row.isCustom = true;
    row.kind = profile.kind;
    row.enabled = profile.enabled;
    merged[id] = row;
  }

  std::vector<ProviderRow> out;
  out.reserve(merged.size());
  for (auto &[_, r] : merged) {
    out.push_back(r);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Provider templates for the add wizard
// ---------------------------------------------------------------------------

struct ProviderTemplate {
  std::string label;
  std::string defaultId;
  std::string displayName;
  std::string kind;
  std::string baseUrl;
  std::string modelsEndpoint;
  std::string chatEndpoint;
  std::string messagesEndpoint;
  std::string anthropicVersion;
  std::string apiKeyRef;
  std::string betaHeader;
  std::string reasoningField;
};

const std::vector<ProviderTemplate> &templates() {
  static const std::vector<ProviderTemplate> kTemplates = {
      {"OpenAI", "openai", "OpenAI", "openai_compatible",
       "https://api.openai.com/v1", "/models", "/chat/completions", "",
       "", "openai", "", "reasoning_effort"},
      {"Anthropic", "anthropic", "Anthropic", "anthropic",
       "https://api.anthropic.com", "/v1/models", "", "/v1/messages",
       "2023-06-01", "anthropic", "", ""},
      {"OpenRouter", "openrouter", "OpenRouter", "openai_compatible",
       "https://openrouter.ai/api/v1", "/models", "/chat/completions", "",
       "", "openrouter", "", "reasoning_effort"},
      {"Groq", "groq", "Groq", "openai_compatible",
       "https://api.groq.com/openai/v1", "/models", "/chat/completions", "",
       "", "groq", "", "reasoning_effort"},
      {"DeepSeek", "deepseek", "DeepSeek", "openai_compatible",
       "https://api.deepseek.com/v1", "/models", "/chat/completions", "",
       "", "deepseek", "", "reasoning_effort"},
      {"Cerebras", "cerebras", "Cerebras", "openai_compatible",
       "https://api.cerebras.ai/v1", "/models", "/chat/completions", "",
       "", "cerebras", "", "reasoning_effort"},
      {"Together", "together", "Together AI", "openai_compatible",
       "https://api.together.xyz/v1", "/models", "/chat/completions", "",
       "", "together", "", "reasoning_effort"},
      {"Mistral", "mistral", "Mistral", "openai_compatible",
       "https://api.mistral.ai/v1", "/models", "/chat/completions", "",
       "", "mistral", "", ""},
      {"Custom OpenAI-compatible", "my-openai", "My OpenAI",
       "openai_compatible", "", "/models", "/chat/completions", "", "",
       "", "", "reasoning_effort"},
      {"Custom Anthropic-compatible", "my-anthropic", "My Anthropic",
       "anthropic", "", "/v1/models", "", "/v1/messages", "2023-06-01",
       "", "", ""},
  };
  return kTemplates;
}

// ---------------------------------------------------------------------------
// CycleField: a safe replacement for ftxui::Radiobox used as horizontal toggle.
// (FTXUI v6.1.9 RadioboxBase::Clamp() crashes when several Radioboxes coexist
// inside a deep Container; this minimal component avoids that path entirely.)
// ---------------------------------------------------------------------------

class CycleField : public ftxui::ComponentBase {
public:
  CycleField(std::shared_ptr<std::vector<std::string>> labels,
             std::shared_ptr<int> index)
      : labels_(std::move(labels)), index_(std::move(index)) {}

  ftxui::Element OnRender() override {
    int n = static_cast<int>(labels_->size());
    if (n <= 0) {
      return ftxui::text("(no options)");
    }
    int i = *index_;
    if (i < 0 || i >= n) {
      i = 0;
    }
    auto el = ftxui::text(std::string("◄ ") + (*labels_)[i] + " ►");
    if (Focused()) {
      el = el | ftxui::inverted;
    }
    return el;
  }

  bool OnEvent(ftxui::Event event) override {
    if (!Focused()) {
      return false;
    }
    int n = static_cast<int>(labels_->size());
    if (n <= 0) {
      return false;
    }
    if (event == ftxui::Event::Character(' ') ||
        event == ftxui::Event::Return ||
        event == ftxui::Event::ArrowRight ||
        event == ftxui::Event::Tab) {
      *index_ = ((*index_) + 1) % n;
      return event != ftxui::Event::Tab;
    }
    if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::TabReverse) {
      *index_ = ((*index_) + n - 1) % n;
      return event != ftxui::Event::TabReverse;
    }
    return false;
  }

  bool Focusable() const override { return !labels_->empty(); }

private:
  std::shared_ptr<std::vector<std::string>> labels_;
  std::shared_ptr<int> index_;
};

ftxui::Component MakeCycleField(std::shared_ptr<std::vector<std::string>> labels,
                                std::shared_ptr<int> index) {
  return std::make_shared<CycleField>(std::move(labels), std::move(index));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string normalizeEndpoint(const std::string &value,
                              const std::string &fallback) {
  return value.empty() ? fallback : value;
}

ftxui::Element fieldRow(const std::string &label, const ftxui::Element &value,
                        const Theme &theme) {
  return ftxui::hbox({
      ftxui::text(label) | ftxui::color(theme.base.dim) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 18),
      value | ftxui::xflex,
  });
}

} // namespace

// ---------------------------------------------------------------------------
// Modal entry point
// ---------------------------------------------------------------------------

ftxui::Component ProvidersModal::create(TuiState &state) {
  // Trigger model discovery so variant editor / pull-models can use cache.
  state.runBackgroundTask(
      []() { firmius::core::Harness::instance().listAllModels(); });

  // ------------------------- shared state -------------------------------
  auto rows = std::make_shared<std::vector<ProviderRow>>();
  auto selected = std::make_shared<int>(0);
  auto mode = std::make_shared<Mode>(Mode::Browse);
  auto message = std::make_shared<std::string>();

  auto templateIndex = std::make_shared<int>(0);

  // Custom-provider edit form state
  auto editOriginalId = std::make_shared<std::string>();
  auto editId = std::make_shared<std::string>();
  auto editDisplayName = std::make_shared<std::string>();
  auto editKindLabels = std::make_shared<std::vector<std::string>>(
      std::vector<std::string>{"openai_compatible", "anthropic"});
  auto editKindIndex = std::make_shared<int>(0);
  auto editEnabledLabels = std::make_shared<std::vector<std::string>>(
      std::vector<std::string>{"enabled", "disabled"});
  auto editEnabledIndex = std::make_shared<int>(0);
  auto editStreamUsageLabels = std::make_shared<std::vector<std::string>>(
      std::vector<std::string>{"enabled", "disabled"});
  auto editStreamUsageIndex = std::make_shared<int>(0);
  auto editBaseUrl = std::make_shared<std::string>();
  auto editModelsEndpoint = std::make_shared<std::string>();
  auto editChatEndpoint = std::make_shared<std::string>();
  auto editMessagesEndpoint = std::make_shared<std::string>();
  auto editApiKeyRef = std::make_shared<std::string>();
  auto editReasoningField = std::make_shared<std::string>();
  auto editAnthropicVersion = std::make_shared<std::string>();
  auto editBetaHeader = std::make_shared<std::string>();
  auto editHeaderKey = std::make_shared<std::string>();
  auto editHeaderValue = std::make_shared<std::string>();
  auto editHeaders = std::make_shared<std::map<std::string, std::string>>();
  auto editMaxRetries = std::make_shared<std::string>("5");
  auto editTimeoutSeconds = std::make_shared<std::string>("300");
  auto editConnectTimeoutSeconds = std::make_shared<std::string>("10");
  auto editBaseDelayMs = std::make_shared<std::string>("1000");
  auto editMaxDelayMs = std::make_shared<std::string>("30000");

  // Built-in retry-config edit state
  auto builtinId = std::make_shared<std::string>();
  auto builtinMaxRetries = std::make_shared<std::string>("5");
  auto builtinTimeoutSeconds = std::make_shared<std::string>("300");
  auto builtinConnectTimeoutSeconds = std::make_shared<std::string>("10");
  auto builtinBaseDelayMs = std::make_shared<std::string>("1000");
  auto builtinMaxDelayMs = std::make_shared<std::string>("30000");

  // Variant editor state
  auto variantProviderId = std::make_shared<std::string>();
  auto variantModelIds = std::make_shared<std::vector<std::string>>();
  auto variantModelLabels = std::make_shared<std::vector<std::string>>();
  auto selectedVariantModel = std::make_shared<int>(0);
  auto variantNameInputText = std::make_shared<std::string>("thinking");
  auto variantJsonInputText = std::make_shared<std::string>("{}");
  auto variantDescriptionInputText = std::make_shared<std::string>();

  // Live-pull progress
  auto pulling = std::make_shared<bool>(false);
  auto pullProviderId = std::make_shared<std::string>();
  auto pullStatus = std::make_shared<std::string>();

  // ------------------------- helpers -----------------------------------
  auto refresh = [rows, selected]() {
    *rows = buildRows();
    int total = static_cast<int>(rows->size());
    if (*selected >= total) {
      *selected = total <= 0 ? 0 : total - 1;
    }
    if (*selected < 0) {
      *selected = 0;
    }
  };

  auto selectedRow = [rows, selected]() -> const ProviderRow * {
    if (rows->empty()) {
      return nullptr;
    }
    int i = *selected;
    if (i < 0 || i >= static_cast<int>(rows->size())) {
      return nullptr;
    }
    return &(*rows)[i];
  };

  auto saveConfigAndReload = [message](const shared::UserConfig &cfg,
                                       const std::string &okMessage) {
    auto &h = firmius::core::Harness::instance();
    try {
      h.updateConfig(cfg);
      h.saveConfig();
      firmius::provider::ProviderRegistry::instance().reloadConfigProviders(
          cfg.providers);
      h.invalidateModelCache();
      *message = okMessage;
    } catch (const std::exception &ex) {
      NotificationManager::instance().notifyError("Providers", ex.what(),
                                                  false);
    }
  };

  auto applyFormFromProfile = [=](const shared::ProviderProfileConfig &p) {
    *editDisplayName = p.displayName;
    *editKindIndex = p.kind == "anthropic" ? 1 : 0;
    *editEnabledIndex = p.enabled ? 0 : 1;
    *editBaseUrl = p.baseUrl;
    *editModelsEndpoint = p.modelsEndpoint;
    *editChatEndpoint = p.chatEndpoint;
    *editMessagesEndpoint = p.messagesEndpoint;
    *editApiKeyRef = p.apiKeyRef;
    *editReasoningField = p.reasoningFieldName;
    *editAnthropicVersion = p.anthropicVersion;
    *editBetaHeader = p.betaHeader;
    *editHeaders = p.headers;
    *editStreamUsageIndex = p.defaults.streamUsage ? 0 : 1;
    *editMaxRetries = std::to_string(p.retry.maxRetries);
    *editTimeoutSeconds = std::to_string(p.retry.timeoutSeconds);
    *editConnectTimeoutSeconds = std::to_string(p.retry.connectTimeoutSeconds);
    *editBaseDelayMs = std::to_string(p.retry.baseDelayMs);
    *editMaxDelayMs = std::to_string(p.retry.maxDelayMs);
  };

  auto buildProfileFromForm = [=]() -> shared::ProviderProfileConfig {
    shared::ProviderProfileConfig p;
    p.kind = (*editKindIndex == 1) ? "anthropic" : "openai_compatible";
    p.displayName = *editDisplayName;
    p.enabled = (*editEnabledIndex == 0);
    p.baseUrl = *editBaseUrl;
    p.modelsEndpoint = normalizeEndpoint(*editModelsEndpoint, "/models");
    p.chatEndpoint =
        normalizeEndpoint(*editChatEndpoint, "/chat/completions");
    p.messagesEndpoint =
        normalizeEndpoint(*editMessagesEndpoint, "/v1/messages");
    p.apiKeyRef = *editApiKeyRef;
    p.reasoningFieldName = *editReasoningField;
    p.anthropicVersion =
        editAnthropicVersion->empty() ? "2023-06-01" : *editAnthropicVersion;
    p.betaHeader = *editBetaHeader;
    p.headers = *editHeaders;
    p.defaults.streamUsage = (*editStreamUsageIndex == 0);
    auto parseInt = [](const std::string &s, int fallback) {
      try {
        return s.empty() ? fallback : std::stoi(s);
      } catch (...) {
        return fallback;
      }
    };
    p.retry.maxRetries = parseInt(*editMaxRetries, 5);
    p.retry.timeoutSeconds = parseInt(*editTimeoutSeconds, 300);
    p.retry.connectTimeoutSeconds = parseInt(*editConnectTimeoutSeconds, 10);
    p.retry.baseDelayMs = parseInt(*editBaseDelayMs, 1000);
    p.retry.maxDelayMs = parseInt(*editMaxDelayMs, 30000);
    return p;
  };

  auto applyTemplate = [=](const ProviderTemplate &t) {
    editOriginalId->clear();
    *editId = t.defaultId;
    *editDisplayName = t.displayName;
    *editKindIndex = t.kind == "anthropic" ? 1 : 0;
    *editEnabledIndex = 0;
    *editBaseUrl = t.baseUrl;
    *editModelsEndpoint = t.modelsEndpoint.empty() ? "/models" : t.modelsEndpoint;
    *editChatEndpoint = t.chatEndpoint;
    *editMessagesEndpoint = t.messagesEndpoint;
    *editApiKeyRef = t.apiKeyRef.empty() ? t.defaultId : t.apiKeyRef;
    *editReasoningField = t.reasoningField;
    *editAnthropicVersion = t.anthropicVersion;
    *editBetaHeader = t.betaHeader;
    *editStreamUsageIndex = 0;
    editHeaders->clear();
    *editHeaderKey = "";
    *editHeaderValue = "";
    *editMaxRetries = "5";
    *editTimeoutSeconds = "300";
    *editConnectTimeoutSeconds = "10";
    *editBaseDelayMs = "1000";
    *editMaxDelayMs = "30000";
  };

  auto beginAdd = [=]() {
    *templateIndex = 0;
    *mode = Mode::TemplatePicker;
    *message = "Pick a template to start.";
  };

  auto beginEditCustom = [=](const std::string &id) {
    const auto cfg = firmius::core::Harness::instance().getConfig();
    auto it = cfg.providers.find(id);
    if (it == cfg.providers.end()) {
      return;
    }
    *editOriginalId = id;
    *editId = id;
    applyFormFromProfile(it->second);
    *editHeaderKey = "";
    *editHeaderValue = "";
    *mode = Mode::EditCustom;
    *message = "Editing provider '" + id + "'.";
  };

  auto beginEditBuiltin = [=](const std::string &id) {
    const auto cfg = firmius::core::Harness::instance().getConfig();
    shared::RetryPolicyConfig retry = cfg.providerRetryDefaults;
    auto rit = cfg.providerRetryPolicies.find(id);
    if (rit != cfg.providerRetryPolicies.end()) {
      retry = rit->second;
    }
    *builtinId = id;
    *builtinMaxRetries = std::to_string(retry.maxRetries);
    *builtinTimeoutSeconds = std::to_string(retry.timeoutSeconds);
    *builtinConnectTimeoutSeconds = std::to_string(retry.connectTimeoutSeconds);
    *builtinBaseDelayMs = std::to_string(retry.baseDelayMs);
    *builtinMaxDelayMs = std::to_string(retry.maxDelayMs);
    *mode = Mode::EditBuiltin;
    *message = "Retry policy for built-in '" + id + "'.";
  };

  auto refreshVariantModels = [=]() {
    variantModelIds->clear();
    variantModelLabels->clear();
    if (variantProviderId->empty()) {
      return;
    }
    const auto models =
        firmius::core::Harness::instance().cachedModelsSnapshot();
    for (const auto &model : models) {
      if (model.provider != *variantProviderId) {
        continue;
      }
      variantModelIds->push_back(model.id);
      std::string label = model.id;
      if (model.contextWindow > 0) {
        label += "  (" + std::to_string(model.contextWindow) + " ctx)";
      }
      if (model.supportsReasoning) {
        label += "  thinking";
      }
      variantModelLabels->push_back(label);
    }
    const auto cfg = firmius::core::Harness::instance().getConfig();
    auto it = cfg.providers.find(*variantProviderId);
    if (it != cfg.providers.end()) {
      for (const auto &[modelId, _] : it->second.modelVariants) {
        if (std::find(variantModelIds->begin(), variantModelIds->end(),
                      modelId) == variantModelIds->end()) {
          variantModelIds->push_back(modelId);
          variantModelLabels->push_back(modelId + "  (config only)");
        }
      }
    }
    if (*selectedVariantModel >= static_cast<int>(variantModelIds->size())) {
      *selectedVariantModel = variantModelIds->empty()
                                  ? 0
                                  : static_cast<int>(variantModelIds->size() - 1);
    }
  };

  auto currentVariantModelId = [=]() -> std::string {
    if (variantModelIds->empty() || *selectedVariantModel < 0 ||
        *selectedVariantModel >= static_cast<int>(variantModelIds->size())) {
      return "";
    }
    return (*variantModelIds)[*selectedVariantModel];
  };

  auto openVariants = [=](const std::string &id) {
    if (id.empty()) {
      return;
    }
    *variantProviderId = id;
    *selectedVariantModel = 0;
    refreshVariantModels();
    *variantNameInputText = "thinking";
    *variantJsonInputText = "{}";
    *variantDescriptionInputText = "";
    *mode = Mode::Variants;
    *message = "Variant editor for '" + id + "'.";
  };

  auto upsertVariant = [=]() {
    if (variantProviderId->empty()) {
      return;
    }
    const std::string modelId = currentVariantModelId();
    if (modelId.empty()) {
      *message = "No model selected.";
      return;
    }
    if (variantNameInputText->empty()) {
      *message = "Variant name cannot be empty.";
      return;
    }
    auto cfg = firmius::core::Harness::instance().getConfig();
    auto pit = cfg.providers.find(*variantProviderId);
    if (pit == cfg.providers.end()) {
      // For built-in providers, lazily create a stub override entry so we can
      // attach variants without converting the provider into a custom one.
      shared::ProviderProfileConfig stub;
      stub.kind = "openai_compatible";
      stub.displayName = *variantProviderId;
      stub.enabled = true;
      cfg.providers[*variantProviderId] = stub;
      pit = cfg.providers.find(*variantProviderId);
    }
    auto &modelCfg = pit->second.modelVariants[modelId];
    if (modelCfg.defaultVariant.empty()) {
      modelCfg.defaultVariant = *variantNameInputText;
    }
    shared::ProviderVariantConfig vcfg;
    vcfg.label = *variantNameInputText;
    vcfg.requestJson =
        variantJsonInputText->empty() ? "{}" : *variantJsonInputText;
    vcfg.description = *variantDescriptionInputText;
    modelCfg.variants[*variantNameInputText] = vcfg;
    saveConfigAndReload(cfg, "Saved variant '" + *variantNameInputText +
                                 "' for '" + modelId + "'.");
    refreshVariantModels();
  };

  auto deleteVariant = [=]() {
    if (variantProviderId->empty() || variantNameInputText->empty()) {
      return;
    }
    const std::string modelId = currentVariantModelId();
    if (modelId.empty()) {
      return;
    }
    auto cfg = firmius::core::Harness::instance().getConfig();
    auto pit = cfg.providers.find(*variantProviderId);
    if (pit == cfg.providers.end()) {
      return;
    }
    auto mit = pit->second.modelVariants.find(modelId);
    if (mit == pit->second.modelVariants.end()) {
      return;
    }
    mit->second.variants.erase(*variantNameInputText);
    if (mit->second.defaultVariant == *variantNameInputText) {
      mit->second.defaultVariant.clear();
    }
    if (mit->second.variants.empty()) {
      pit->second.modelVariants.erase(mit);
    }
    saveConfigAndReload(cfg, "Removed variant '" + *variantNameInputText +
                                 "' from '" + modelId + "'.");
    refreshVariantModels();
  };

  auto setDefaultVariant = [=]() {
    if (variantProviderId->empty() || variantNameInputText->empty()) {
      return;
    }
    const std::string modelId = currentVariantModelId();
    if (modelId.empty()) {
      return;
    }
    auto cfg = firmius::core::Harness::instance().getConfig();
    auto pit = cfg.providers.find(*variantProviderId);
    if (pit == cfg.providers.end()) {
      return;
    }
    auto mit = pit->second.modelVariants.find(modelId);
    if (mit == pit->second.modelVariants.end() ||
        mit->second.variants.find(*variantNameInputText) ==
            mit->second.variants.end()) {
      *message = "Save the variant first.";
      return;
    }
    mit->second.defaultVariant = *variantNameInputText;
    saveConfigAndReload(cfg, "Set default variant for '" + modelId + "'.");
  };

  auto deleteSelectedRow = [=]() {
    auto *r = selectedRow();
    if (!r || !r->isCustom) {
      return;
    }
    auto cfg = firmius::core::Harness::instance().getConfig();
    cfg.providers.erase(r->id);
    saveConfigAndReload(cfg, "Deleted custom provider '" + r->id + "'.");
    refresh();
    *mode = Mode::Browse;
  };

  auto pullModelsFor = [=, &state](const std::string &id) {
    if (id.empty()) {
      return;
    }
    *pulling = true;
    *pullProviderId = id;
    *pullStatus = "Pulling models for '" + id + "' ...";
    state.runBackgroundTask([id, pulling, pullStatus]() {
      auto provider =
          firmius::provider::ProviderRegistry::instance().getProvider(id);
      if (!provider) {
        *pullStatus = "Provider '" + id + "' not registered.";
        *pulling = false;
        return;
      }
      try {
        auto models = provider->listModels();
        std::string head;
        for (size_t i = 0; i < models.size() && i < 3; ++i) {
          if (i)
            head += ", ";
          head += models[i].id;
        }
        *pullStatus = "Pulled " + std::to_string(models.size()) +
                      " models from '" + id + "'." +
                      (head.empty() ? "" : "  e.g. " + head);
      } catch (const std::exception &e) {
        *pullStatus =
            std::string("Pull failed for '") + id + "': " + e.what();
      } catch (...) {
        *pullStatus = "Pull failed for '" + id + "': unknown error.";
      }
      *pulling = false;
      firmius::core::Harness::instance().invalidateModelCache();
      firmius::core::Harness::instance().listAllModels();
    });
  };

  refresh();

  // ------------------------- input components ---------------------------
  auto idInput = ftxui::Input(editId.get(), "provider id");
  auto displayInput = ftxui::Input(editDisplayName.get(), "display name");
  auto kindCycle = MakeCycleField(editKindLabels, editKindIndex);
  auto enabledCycle = MakeCycleField(editEnabledLabels, editEnabledIndex);
  auto streamUsageCycle =
      MakeCycleField(editStreamUsageLabels, editStreamUsageIndex);
  auto baseUrlInput = ftxui::Input(editBaseUrl.get(), "https://...");
  auto modelsEndpointInput =
      ftxui::Input(editModelsEndpoint.get(), "/models");
  auto chatEndpointInput =
      ftxui::Input(editChatEndpoint.get(), "/chat/completions");
  auto messagesEndpointInput =
      ftxui::Input(editMessagesEndpoint.get(), "/v1/messages");
  auto apiKeyRefInput = ftxui::Input(editApiKeyRef.get(), "api key ref");
  auto reasoningFieldInput =
      ftxui::Input(editReasoningField.get(), "reasoning_effort");
  auto anthropicVersionInput =
      ftxui::Input(editAnthropicVersion.get(), "2023-06-01");
  auto betaHeaderInput =
      ftxui::Input(editBetaHeader.get(), "anthropic beta header");
  auto headerKeyInput = ftxui::Input(editHeaderKey.get(), "header key");
  auto headerValueInput = ftxui::Input(editHeaderValue.get(), "header value");
  auto maxRetriesInput = ftxui::Input(editMaxRetries.get(), "5");
  auto timeoutSecondsInput =
      ftxui::Input(editTimeoutSeconds.get(), "300");
  auto connectTimeoutInput =
      ftxui::Input(editConnectTimeoutSeconds.get(), "10");
  auto baseDelayMsInput = ftxui::Input(editBaseDelayMs.get(), "1000");
  auto maxDelayMsInput = ftxui::Input(editMaxDelayMs.get(), "30000");

  auto builtinMaxRetriesInput =
      ftxui::Input(builtinMaxRetries.get(), "5");
  auto builtinTimeoutInput =
      ftxui::Input(builtinTimeoutSeconds.get(), "300");
  auto builtinConnectTimeoutInput =
      ftxui::Input(builtinConnectTimeoutSeconds.get(), "10");
  auto builtinBaseDelayInput =
      ftxui::Input(builtinBaseDelayMs.get(), "1000");
  auto builtinMaxDelayInput =
      ftxui::Input(builtinMaxDelayMs.get(), "30000");

  auto variantNameInput =
      ftxui::Input(variantNameInputText.get(), "thinking");
  auto variantJsonInput = ftxui::Input(
      variantJsonInputText.get(), "json patch (e.g. {\"reasoning\":{}})");
  auto variantDescriptionInput =
      ftxui::Input(variantDescriptionInputText.get(), "optional description");

  auto editFormContainer = ftxui::Container::Vertical({
      idInput,
      displayInput,
      kindCycle,
      enabledCycle,
      baseUrlInput,
      modelsEndpointInput,
      chatEndpointInput,
      messagesEndpointInput,
      apiKeyRefInput,
      reasoningFieldInput,
      anthropicVersionInput,
      betaHeaderInput,
      headerKeyInput,
      headerValueInput,
      streamUsageCycle,
      maxRetriesInput,
      timeoutSecondsInput,
      connectTimeoutInput,
      baseDelayMsInput,
      maxDelayMsInput,
  });

  auto builtinFormContainer = ftxui::Container::Vertical({
      builtinMaxRetriesInput,
      builtinTimeoutInput,
      builtinConnectTimeoutInput,
      builtinBaseDelayInput,
      builtinMaxDelayInput,
  });

  auto variantFormContainer = ftxui::Container::Vertical({
      variantNameInput,
      variantJsonInput,
      variantDescriptionInput,
  });

  auto eventContainer = ftxui::Container::Vertical({
      editFormContainer,
      builtinFormContainer,
      variantFormContainer,
  });

  // ------------------------- renderer ----------------------------------
  auto component = ftxui::Renderer(eventContainer, [=] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    ftxui::Elements elements;

    elements.push_back(ftxui::text("Provider Profiles") | ftxui::bold |
                       ftxui::color(theme.modals.title));
    elements.push_back(
        ftxui::text("All providers — built-in and custom — manage configs, "
                    "retry policy, model variants.") |
        ftxui::color(theme.base.dim));
    elements.push_back(ftxui::separatorLight() |
                       ftxui::color(theme.modals.border));

    if (*mode == Mode::Browse || *mode == Mode::ConfirmDelete) {
      if (rows->empty()) {
        elements.push_back(
            ftxui::text("No providers registered.") | ftxui::color(theme.base.dim));
      } else {
        // Cached models count snapshot per provider for nicer rows.
        std::map<std::string, int> modelCounts;
        for (const auto &m :
             firmius::core::Harness::instance().cachedModelsSnapshot()) {
          modelCounts[m.provider]++;
        }
        for (size_t i = 0; i < rows->size(); ++i) {
          const auto &r = (*rows)[i];
          const bool sel = static_cast<int>(i) == *selected;
          std::string typeTag = r.isCustom ? r.kind : ("builtin " + r.kind);
          std::string state = r.enabled ? "on" : "off";
          int mc = modelCounts.count(r.id) ? modelCounts[r.id] : 0;
          std::string line = std::string(sel ? "▶ " : "  ") + r.id + "  [" +
                             typeTag + "]  " + state + "  " +
                             std::to_string(mc) + " models";
          auto el = ftxui::text(line);
          if (sel) {
            el = el | ftxui::color(theme.modals.highlight_fg) |
                 ftxui::bgcolor(theme.modals.highlight_bg);
          } else if (!r.enabled) {
            el = el | ftxui::color(theme.base.dim);
          } else {
            el = el | ftxui::color(theme.modals.fg);
          }
          elements.push_back(el);
        }
      }

      if (*mode == Mode::ConfirmDelete) {
        elements.push_back(ftxui::separatorLight() |
                           ftxui::color(theme.modals.border));
        elements.push_back(
            ftxui::text("Delete custom provider? (y/n)") |
            ftxui::color(theme.modals.highlight_fg));
      }

      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(
          ftxui::text("Enter settings  A add custom  V variants  P pull "
                      "models  K accounts  Q quotas") |
          ftxui::color(theme.base.dim));
      elements.push_back(
          ftxui::text("Space toggle  D delete (custom)  Esc close") |
          ftxui::color(theme.base.dim));
    } else if (*mode == Mode::TemplatePicker) {
      elements.push_back(ftxui::text("Choose a starting template:") |
                         ftxui::color(theme.modals.fg));
      const auto &tpls = templates();
      for (size_t i = 0; i < tpls.size(); ++i) {
        const auto &t = tpls[i];
        const bool sel = static_cast<int>(i) == *templateIndex;
        std::string line = std::string(sel ? "▶ " : "  ") + t.label + "   " +
                           (t.baseUrl.empty() ? "(blank)" : t.baseUrl);
        auto el = ftxui::text(line);
        if (sel) {
          el = el | ftxui::color(theme.modals.highlight_fg) |
               ftxui::bgcolor(theme.modals.highlight_bg);
        } else {
          el = el | ftxui::color(theme.modals.fg);
        }
        elements.push_back(el);
      }
      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(
          ftxui::text("Enter use template  Esc cancel  ↑↓ navigate") |
          ftxui::color(theme.base.dim));
    } else if (*mode == Mode::EditCustom) {
      elements.push_back(fieldRow("ID", idInput->Render(), theme));
      elements.push_back(
          fieldRow("Display", displayInput->Render(), theme));
      elements.push_back(fieldRow("Kind", kindCycle->Render(), theme));
      elements.push_back(
          fieldRow("Enabled", enabledCycle->Render(), theme));
      elements.push_back(
          fieldRow("Base URL", baseUrlInput->Render(), theme));
      elements.push_back(fieldRow("Models endpoint",
                                  modelsEndpointInput->Render(), theme));
      if (*editKindIndex == 0) {
        elements.push_back(fieldRow("Chat endpoint",
                                    chatEndpointInput->Render(), theme));
        elements.push_back(fieldRow("Reasoning field",
                                    reasoningFieldInput->Render(), theme));
      } else {
        elements.push_back(fieldRow("Messages endpoint",
                                    messagesEndpointInput->Render(), theme));
        elements.push_back(fieldRow("Anthropic ver",
                                    anthropicVersionInput->Render(), theme));
        elements.push_back(
            fieldRow("Beta header", betaHeaderInput->Render(), theme));
      }
      elements.push_back(
          fieldRow("API key ref", apiKeyRefInput->Render(), theme));
      elements.push_back(
          fieldRow("Stream usage", streamUsageCycle->Render(), theme));
      elements.push_back(
          fieldRow("Header key", headerKeyInput->Render(), theme));
      elements.push_back(
          fieldRow("Header value", headerValueInput->Render(), theme));

      if (editHeaders->empty()) {
        elements.push_back(ftxui::text("Headers: none") |
                           ftxui::color(theme.base.dim));
      } else {
        elements.push_back(ftxui::text("Headers:") |
                           ftxui::color(theme.base.dim));
        int shown = 0;
        for (const auto &[k, v] : *editHeaders) {
          if (shown++ >= 4) {
            elements.push_back(ftxui::text("  ...") |
                               ftxui::color(theme.base.dim));
            break;
          }
          elements.push_back(ftxui::text("  " + k + ": " + v) |
                             ftxui::color(theme.modals.fg));
        }
      }

      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(ftxui::text("Retry policy") | ftxui::bold |
                         ftxui::color(theme.modals.fg));
      elements.push_back(
          fieldRow("Max retries", maxRetriesInput->Render(), theme));
      elements.push_back(
          fieldRow("Timeout sec", timeoutSecondsInput->Render(), theme));
      elements.push_back(fieldRow("Connect timeout",
                                  connectTimeoutInput->Render(), theme));
      elements.push_back(
          fieldRow("Base delay ms", baseDelayMsInput->Render(), theme));
      elements.push_back(
          fieldRow("Max delay ms", maxDelayMsInput->Render(), theme));

      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(
          ftxui::text("Ctrl+S save  H add header  R remove header  P pull "
                      "models  Esc cancel") |
          ftxui::color(theme.base.dim));
    } else if (*mode == Mode::EditBuiltin) {
      elements.push_back(ftxui::text("Built-in provider: " + *builtinId) |
                         ftxui::bold | ftxui::color(theme.modals.title));
      elements.push_back(
          ftxui::text("Built-in providers ship with hard-coded endpoints. "
                      "You can tune retry policy here.") |
          ftxui::color(theme.base.dim));
      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(
          fieldRow("Max retries", builtinMaxRetriesInput->Render(), theme));
      elements.push_back(
          fieldRow("Timeout sec", builtinTimeoutInput->Render(), theme));
      elements.push_back(fieldRow("Connect timeout",
                                  builtinConnectTimeoutInput->Render(),
                                  theme));
      elements.push_back(
          fieldRow("Base delay ms", builtinBaseDelayInput->Render(), theme));
      elements.push_back(
          fieldRow("Max delay ms", builtinMaxDelayInput->Render(), theme));

      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(
          ftxui::text("Ctrl+S save  P pull models  V variants  K accounts  "
                      "Q quotas  Esc cancel") |
          ftxui::color(theme.base.dim));
    } else if (*mode == Mode::Variants) {
      elements.push_back(
          ftxui::text("Variant editor: " + *variantProviderId) | ftxui::bold |
          ftxui::color(theme.modals.title));
      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      if (variantModelLabels->empty()) {
        elements.push_back(
            ftxui::text("No models discovered yet. Press P to pull.") |
            ftxui::color(theme.base.dim));
      } else {
        elements.push_back(ftxui::text("Models:") |
                           ftxui::color(theme.base.dim));
        int shown = 0;
        for (size_t i = 0; i < variantModelLabels->size(); ++i) {
          if (shown++ >= 8) {
            elements.push_back(ftxui::text("  ... " +
                                           std::to_string(
                                               variantModelLabels->size() - 8) +
                                           " more") |
                               ftxui::color(theme.base.dim));
            break;
          }
          const bool sel = static_cast<int>(i) == *selectedVariantModel;
          elements.push_back(
              ftxui::text((sel ? "▶ " : "  ") + (*variantModelLabels)[i]) |
              ftxui::color(sel ? theme.modals.highlight_fg : theme.modals.fg));
        }

        const std::string modelId = currentVariantModelId();
        const auto cfg = firmius::core::Harness::instance().getConfig();
        auto pit = cfg.providers.find(*variantProviderId);
        if (!modelId.empty() && pit != cfg.providers.end()) {
          auto mit = pit->second.modelVariants.find(modelId);
          elements.push_back(ftxui::separatorLight() |
                             ftxui::color(theme.modals.border));
          elements.push_back(ftxui::text("Configured variants for " + modelId +
                                         ":") |
                             ftxui::color(theme.base.dim));
          if (mit == pit->second.modelVariants.end() ||
              mit->second.variants.empty()) {
            elements.push_back(ftxui::text("  none") |
                               ftxui::color(theme.base.dim));
          } else {
            for (const auto &[name, v] : mit->second.variants) {
              std::string marker =
                  mit->second.defaultVariant == name ? " [default]" : "";
              elements.push_back(ftxui::text("  " + name + marker) |
                                 ftxui::color(theme.modals.fg));
            }
          }
        }
      }
      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(
          fieldRow("Variant name", variantNameInput->Render(), theme));
      elements.push_back(
          fieldRow("Variant JSON", variantJsonInput->Render(), theme));
      elements.push_back(
          fieldRow("Description", variantDescriptionInput->Render(), theme));
      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(
          ftxui::text("A add/update  X delete  S set default  P pull models  "
                      "↑↓ pick model  Esc back") |
          ftxui::color(theme.base.dim));
    }

    if (*pulling) {
      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(ftxui::text("⟳ " + *pullStatus) |
                         ftxui::color(theme.base.dim));
    } else if (!pullStatus->empty()) {
      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(ftxui::text(*pullStatus) |
                         ftxui::color(theme.modals.highlight_fg));
    }

    if (!message->empty()) {
      elements.push_back(ftxui::separatorLight() |
                         ftxui::color(theme.modals.border));
      elements.push_back(ftxui::text(*message) |
                         ftxui::color(theme.modals.highlight_fg));
    }

    return FlatModalPanel(theme, " Providers ",
                          ModalSection(theme,
                                       ftxui::vbox(std::move(elements)) |
                                           ftxui::yframe |
                                           ftxui::vscroll_indicator,
                                       theme.modals.bg),
                          100, 38, theme.modals.title);
  });

  // ------------------------- event routing ------------------------------
  auto handleSaveCustom = [=]() {
    if (editId->empty()) {
      *message = "Provider ID cannot be empty.";
      return;
    }
    auto cfg = firmius::core::Harness::instance().getConfig();
    const std::string originalId = *editOriginalId;
    const std::string newId = *editId;
    if (!originalId.empty() && originalId != newId) {
      cfg.providers.erase(originalId);
    }
    auto profile = buildProfileFromForm();
    auto existingIt = cfg.providers.find(newId);
    if (existingIt != cfg.providers.end()) {
      profile.modelVariants = existingIt->second.modelVariants;
    }
    cfg.providers[newId] = profile;
    saveConfigAndReload(cfg, "Saved provider '" + newId + "'.");
    refresh();
    *mode = Mode::Browse;
  };

  auto handleSaveBuiltin = [=]() {
    if (builtinId->empty()) {
      return;
    }
    auto parseInt = [](const std::string &s, int fallback) {
      try {
        return s.empty() ? fallback : std::stoi(s);
      } catch (...) {
        return fallback;
      }
    };
    auto cfg = firmius::core::Harness::instance().getConfig();
    auto &pol = cfg.providerRetryPolicies[*builtinId];
    pol.maxRetries = parseInt(*builtinMaxRetries, 5);
    pol.timeoutSeconds = parseInt(*builtinTimeoutSeconds, 300);
    pol.connectTimeoutSeconds = parseInt(*builtinConnectTimeoutSeconds, 10);
    pol.baseDelayMs = parseInt(*builtinBaseDelayMs, 1000);
    pol.maxDelayMs = parseInt(*builtinMaxDelayMs, 30000);
    saveConfigAndReload(cfg,
                        "Saved retry policy for built-in '" + *builtinId + "'.");
    *mode = Mode::Browse;
  };

  return ftxui::CatchEvent(component, [=, &state](
                                          const ftxui::Event &event) mutable {
    // Esc handling.
    if (event == ftxui::Event::Escape) {
      if (*mode == Mode::Browse) {
        state.popModal();
      } else {
        *mode = Mode::Browse;
      }
      return true;
    }

    // ConfirmDelete branch.
    if (*mode == Mode::ConfirmDelete) {
      if (event == ftxui::Event::Character('y') ||
          event == ftxui::Event::Character('Y')) {
        deleteSelectedRow();
        return true;
      }
      if (event == ftxui::Event::Character('n') ||
          event == ftxui::Event::Character('N')) {
        *mode = Mode::Browse;
        return true;
      }
      return false;
    }

    // TemplatePicker branch.
    if (*mode == Mode::TemplatePicker) {
      const auto &tpls = templates();
      if (event == ftxui::Event::ArrowUp && *templateIndex > 0) {
        --(*templateIndex);
        return true;
      }
      if (event == ftxui::Event::ArrowDown &&
          *templateIndex + 1 < static_cast<int>(tpls.size())) {
        ++(*templateIndex);
        return true;
      }
      if (event == ftxui::Event::Return) {
        if (*templateIndex >= 0 &&
            *templateIndex < static_cast<int>(tpls.size())) {
          applyTemplate(tpls[*templateIndex]);
          *mode = Mode::EditCustom;
          *message = "Edit and Ctrl+S to save.";
        }
        return true;
      }
      return false;
    }

    // Browse branch.
    if (*mode == Mode::Browse) {
      if (event == ftxui::Event::ArrowUp && *selected > 0) {
        --(*selected);
        return true;
      }
      if (event == ftxui::Event::ArrowDown &&
          *selected + 1 < static_cast<int>(rows->size())) {
        ++(*selected);
        return true;
      }
      const auto *r = selectedRow();
      if (event == ftxui::Event::Character('a') ||
          event == ftxui::Event::Character('A')) {
        beginAdd();
        return true;
      }
      if (event == ftxui::Event::Return) {
        if (r) {
          if (r->isCustom) {
            beginEditCustom(r->id);
          } else {
            beginEditBuiltin(r->id);
          }
        }
        return true;
      }
      if (event == ftxui::Event::Character('v') ||
          event == ftxui::Event::Character('V')) {
        if (r) {
          openVariants(r->id);
        }
        return true;
      }
      if (event == ftxui::Event::Character('p') ||
          event == ftxui::Event::Character('P')) {
        if (r) {
          pullModelsFor(r->id);
        }
        return true;
      }
      if (event == ftxui::Event::Character('k') ||
          event == ftxui::Event::Character('K')) {
        if (r) {
          auto modal = std::make_shared<AccountsModal>(r->id);
          state.deferUiMutation([&state, modal]() {
            state.openModalDirect(modal->create(state));
          });
        }
        return true;
      }
      if (event == ftxui::Event::Character('q') ||
          event == ftxui::Event::Character('Q')) {
        if (r) {
          auto modal = std::make_shared<QuotasModal>(r->id);
          state.deferUiMutation([&state, modal]() {
            state.openModalDirect(modal->create(state));
          });
        }
        return true;
      }
      if (event == ftxui::Event::Character('d') ||
          event == ftxui::Event::Character('D')) {
        if (r && r->isCustom) {
          *mode = Mode::ConfirmDelete;
        } else {
          *message = "Built-in providers cannot be deleted.";
        }
        return true;
      }
      if (event == ftxui::Event::Character(' ')) {
        if (r && r->isCustom) {
          auto cfg = firmius::core::Harness::instance().getConfig();
          auto it = cfg.providers.find(r->id);
          if (it != cfg.providers.end()) {
            it->second.enabled = !it->second.enabled;
            saveConfigAndReload(cfg, "Provider '" + r->id +
                                         (it->second.enabled ? "' enabled."
                                                              : "' disabled."));
            refresh();
          }
        } else {
          *message = "Built-in providers toggle via account/key state.";
        }
        return true;
      }
      return false;
    }

    // EditCustom branch.
    if (*mode == Mode::EditCustom) {
      // Ctrl+S save (^S = 0x13)
      if (event == ftxui::Event::CtrlS ||
          (event.is_character() && event.character() == std::string(1, '\x13'))) {
        handleSaveCustom();
        return true;
      }
      // P: pull models for current id (if it already exists in registry)
      if (event == ftxui::Event::Character('p') ||
          event == ftxui::Event::Character('P')) {
        if (!editFormContainer->ChildAt(0)->Focused() &&
            !editFormContainer->Focused()) {
          // not focused on text input, treat as command
          pullModelsFor(editId->empty() ? *editOriginalId : *editId);
          return true;
        }
        // otherwise let input consume it
      }
      // H/R header ops only when no input is focused
      if (!editFormContainer->Focused()) {
        if (event == ftxui::Event::Character('h') ||
            event == ftxui::Event::Character('H')) {
          if (!editHeaderKey->empty()) {
            (*editHeaders)[*editHeaderKey] = *editHeaderValue;
            *message = "Header saved: " + *editHeaderKey;
          }
          return true;
        }
        if (event == ftxui::Event::Character('r') ||
            event == ftxui::Event::Character('R')) {
          if (!editHeaderKey->empty()) {
            editHeaders->erase(*editHeaderKey);
            *message = "Header removed: " + *editHeaderKey;
          }
          return true;
        }
      }
      return editFormContainer->OnEvent(event);
    }

    // EditBuiltin branch.
    if (*mode == Mode::EditBuiltin) {
      if (event == ftxui::Event::CtrlS ||
          (event.is_character() && event.character() == std::string(1, '\x13'))) {
        handleSaveBuiltin();
        return true;
      }
      if (!builtinFormContainer->Focused()) {
        if (event == ftxui::Event::Character('p') ||
            event == ftxui::Event::Character('P')) {
          pullModelsFor(*builtinId);
          return true;
        }
        if (event == ftxui::Event::Character('v') ||
            event == ftxui::Event::Character('V')) {
          openVariants(*builtinId);
          return true;
        }
        if (event == ftxui::Event::Character('k') ||
            event == ftxui::Event::Character('K')) {
          auto modal = std::make_shared<AccountsModal>(*builtinId);
          state.deferUiMutation([&state, modal]() {
            state.openModalDirect(modal->create(state));
          });
          return true;
        }
        if (event == ftxui::Event::Character('q') ||
            event == ftxui::Event::Character('Q')) {
          auto modal = std::make_shared<QuotasModal>(*builtinId);
          state.deferUiMutation([&state, modal]() {
            state.openModalDirect(modal->create(state));
          });
          return true;
        }
      }
      return builtinFormContainer->OnEvent(event);
    }

    // Variants branch.
    if (*mode == Mode::Variants) {
      if (!variantFormContainer->Focused()) {
        if (event == ftxui::Event::ArrowUp && *selectedVariantModel > 0) {
          --(*selectedVariantModel);
          return true;
        }
        if (event == ftxui::Event::ArrowDown &&
            *selectedVariantModel + 1 <
                static_cast<int>(variantModelIds->size())) {
          ++(*selectedVariantModel);
          return true;
        }
        if (event == ftxui::Event::Character('a') ||
            event == ftxui::Event::Character('A')) {
          upsertVariant();
          return true;
        }
        if (event == ftxui::Event::Character('x') ||
            event == ftxui::Event::Character('X')) {
          deleteVariant();
          return true;
        }
        if (event == ftxui::Event::Character('s') ||
            event == ftxui::Event::Character('S')) {
          setDefaultVariant();
          return true;
        }
        if (event == ftxui::Event::Character('p') ||
            event == ftxui::Event::Character('P')) {
          pullModelsFor(*variantProviderId);
          return true;
        }
      }
      return variantFormContainer->OnEvent(event);
    }

    return false;
  });
}

} // namespace firmius::tui
