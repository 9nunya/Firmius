#include "audits/OAuthWizardAudit.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

namespace firmius::audits {

using namespace firmius::shared;

using namespace firmius::core;
using namespace firmius::provider;

std::string OAuthWizardAudit::getId() const { return "oauth_wizard"; }

std::string OAuthWizardAudit::getDescription() const { return "Run OAuth wizard for a specified provider (usage: --audit oauth_wizard <provider>)"; }

shared::AuditResult OAuthWizardAudit::run(const std::vector<std::string>& args) {
    AuditResult result;
    result.auditId = getId();
    
    if (args.empty()) {
        std::cerr << "Usage: firmius_audit --audit oauth_wizard <provider>" << std::endl;
        std::cerr << "Example: firmius_audit --audit oauth_wizard codex" << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }
    
    std::string providerName = args[0];
    std::cout << "Starting OAuth Wizard Audit for provider: " << providerName << std::endl;
    
    Panic::init();
    EnvLoader::load(".env.local");
    
    // Initialize harness to register all providers
    auto& harness = Harness::instance();
    harness.init();
    
    auto& registry = ProviderRegistry::instance();
    auto provider = registry.getProvider(providerName);
    
    if (!provider) {
        std::cerr << "Provider not found: " << providerName << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }
    
    auto oauthProvider = std::dynamic_pointer_cast<BaseOAuthProvider>(provider);
    if (!oauthProvider) {
        std::cerr << "Provider does not support OAuth: " << providerName << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }
    
    // Check for existing accounts
    auto accounts = oauthProvider->getAccounts();
    if (!accounts.empty()) {
        std::cout << "Note: " << accounts.size() << " existing account(s) found for " << providerName << std::endl;
        std::cout << "Proceeding with adding a new account..." << std::endl;
    }
    
    auto wizard = oauthProvider->beginConnectionWizard();
    if (!wizard) {
        std::cerr << "Failed to create OAuth wizard for provider: " << providerName << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }
    
    // Get and display the initial prompt with URL
    auto prompt = wizard->nextPrompt();
    if (!prompt) {
        std::cerr << "No initial prompt from wizard" << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "OAuth Connection Wizard for: " << providerName << std::endl;
    std::cout << "========================================\n" << std::endl;
    std::cout << prompt->message << std::endl;
    std::cout << "\n----------------------------------------" << std::endl;
    std::cout << "Waiting for authorization callback..." << std::endl;
    std::cout << "Press Ctrl+C to cancel.\n" << std::endl;
    
    // Poll until wizard is complete
    while (!wizard->isComplete()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    std::cout << "\nAuthorization response received. Finalizing exchange..." << std::endl;
    
    std::string errorMessage;
    if (!wizard->finalizeExchange(errorMessage)) {
        std::cerr << "\nOAuth connection failed: " << errorMessage << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }
    
    std::cout << "\n========================================" << std::endl;
    std::cout << wizard->getFinalMessage() << std::endl;
    std::cout << "========================================" << std::endl;

    harness.shutdown();
    result.exitCode = 0;
    result.passed = true;
    return result;
}

}
