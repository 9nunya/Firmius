# PROVIDER PACKAGE

**Purpose**: LLM provider implementations for various APIs.

---

## STRUCTURE

```
packages/provider/
├── include/providers/
│   ├── ProviderRegistry.hpp    # Singleton registry
│   ├── IProvider.hpp           # Interface (in shared/)
│   ├── BaseOpenAIProvider.hpp  # OpenAI-compatible base
│   ├── BaseAPIKeyProvider.hpp  # API key auth base
│   ├── BaseOAuthProvider.hpp   # OAuth auth base
│   └── [15+ Provider].hpp      # Concrete implementations
└── src/providers/
    └── [Implementation].cpp
```

---

## PROVIDER HIERARCHY

```
IProvider (shared/)
├── BaseOpenAIProvider
│   ├── OpenRouterProvider
│   ├── LMStudioProvider
│   ├── QwenProvider
│   ├── ZaiProvider
│   ├── ZenProvider
│   ├── NanoGPTProvider
│   ├── ChutesProvider
│   └── CodexProvider
├── BaseAPIKeyProvider
│   └── [Various API key providers]
└── BaseOAuthProvider
    └── AntigravityProvider (Google/DeepMind protocol)
```

---

## KEY COMPONENTS

### ProviderRegistry
- **File**: `include/providers/ProviderRegistry.hpp`
- **Pattern**: Singleton, thread-safe
- **Methods**: `registerProvider()`, `getProvider(id)`, `listProviderIds()`

### IProvider Interface
- **File**: `packages/shared/include/IProvider.hpp`
- **Key Methods**:
  - `listModels()` → Available models
  - `stream()` → LLM streaming
  - `generateSummary()` → Response summarization
  - `getModelInfo()` → Model metadata

### Base Classes

| Base | Purpose | Use Case |
|------|---------|----------|
| `BaseOpenAIProvider` | OpenAI-compatible REST APIs | Most providers |
| `BaseAPIKeyProvider` | Header-based API key auth | Simple auth |
| `BaseOAuthProvider` | OAuth flow with wizard | Google, Antigravity |

### Available Providers (15+)

| Provider | Protocol | Auth |
|----------|----------|------|
| Antigravity | Native Google/DeepMind | OAuth |
| OpenRouter | OpenAI-compatible | API Key |
| LMStudio | OpenAI-compatible | API Key |
| Qwen | OpenAI-compatible | API Key |
| Zai | OpenAI-compatible | API Key |
| Zen | OpenAI-compatible | API Key |
| NanoGPT | OpenAI-compatible | API Key |
| Chutes | OpenAI-compatible | API Key |
| Codex | OpenAI-compatible | API Key |

---

## ADDING A NEW PROVIDER

1. Create class inheriting from appropriate base
2. Implement `IProvider` interface methods
3. Register in `ProviderRegistry`
4. Add to `Engine::initProviders()`

---

## CONVENTIONS

- Providers stored as `std::shared_ptr<IProvider>`
- Thread-safe access via `ProviderRegistry` mutex
- OAuth providers use `OAuthWizard` for flow
