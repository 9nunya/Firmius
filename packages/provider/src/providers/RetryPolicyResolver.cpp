#include "providers/RetryPolicyResolver.hpp"

#include "providers/BackoffConstants.hpp"
#include <algorithm>
#include <random>

namespace firmius::provider {

using namespace firmius::shared;

namespace {

bool hasRetryCurlTag(const std::unordered_set<std::string> &tags,
                     const std::string &tag) {
  return tags.find(tag) != tags.end();
}

} // namespace

RetryPolicyRuntime RetryPolicyResolver::resolve(const std::string &providerId) {
  RetryPolicyRuntime runtime;

  const auto &cfg = shared::ConfigLoader::instance().getConfig();
  runtime.config = cfg.providerRetryDefaults;

  auto userIt = cfg.providerRetryPolicies.find(providerId);
  if (userIt != cfg.providerRetryPolicies.end()) {
    runtime.config = userIt->second;
  }

  auto dynamicIt = cfg.providers.find(providerId);
  if (dynamicIt != cfg.providers.end()) {
    runtime.config = dynamicIt->second.retry;
  }

  for (int code : runtime.config.retryHttpStatuses) {
    runtime.retryHttpStatuses.insert(code);
  }
  for (int code : runtime.config.nonRetryHttpStatuses) {
    runtime.nonRetryHttpStatuses.insert(code);
  }
  for (const auto &tag : runtime.config.retryCurlErrors) {
    runtime.retryCurlErrors.insert(tag);
  }

  return runtime;
}

bool RetryPolicyResolver::isRetriableHttpStatus(const RetryPolicyRuntime &policy,
                                                int httpStatus) {
  return policy.retryHttpStatuses.find(httpStatus) !=
         policy.retryHttpStatuses.end();
}

bool RetryPolicyResolver::isNonRetriableHttpStatus(
    const RetryPolicyRuntime &policy, int httpStatus) {
  return policy.nonRetryHttpStatuses.find(httpStatus) !=
         policy.nonRetryHttpStatuses.end();
}

bool RetryPolicyResolver::isRetriableCurlError(const RetryPolicyRuntime &policy,
                                               CURLcode code) {
  switch (code) {
  case CURLE_OPERATION_TIMEDOUT:
    return hasRetryCurlTag(policy.retryCurlErrors, "timeout");
  case CURLE_COULDNT_CONNECT:
    return hasRetryCurlTag(policy.retryCurlErrors, "connect");
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_RESOLVE_PROXY:
    return hasRetryCurlTag(policy.retryCurlErrors, "dns");
  case CURLE_SEND_ERROR:
    return hasRetryCurlTag(policy.retryCurlErrors, "send");
  case CURLE_RECV_ERROR:
  case CURLE_GOT_NOTHING:
    return hasRetryCurlTag(policy.retryCurlErrors, "recv");
  default:
    return false;
  }
}

int RetryPolicyResolver::computeDelayMs(const RetryPolicyRuntime &policy,
                                        int attempt, int headerDelayMs) {
  const int boundedAttempt = std::max(0, attempt);

  int baseDelayMs = policy.config.baseDelayMs;
  if (policy.config.useSharedBackoffSequence) {
    baseDelayMs =
        shared::BackoffConstants::getBackoffSeconds(boundedAttempt) * 1000;
  }

  const int cappedDelay = std::min(baseDelayMs, policy.config.maxDelayMs);
  int effectiveDelay = cappedDelay;
  if (policy.config.respectRetryAfter) {
    effectiveDelay = std::max(effectiveDelay, headerDelayMs);
  }

  const double jitterMin =
      std::min(policy.config.jitterMin, policy.config.jitterMax);
  const double jitterMax =
      std::max(policy.config.jitterMin, policy.config.jitterMax);

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<> dis(jitterMin, jitterMax);
  const double jitter = dis(gen);

  return static_cast<int>(effectiveDelay * jitter);
}

} // namespace firmius::provider
