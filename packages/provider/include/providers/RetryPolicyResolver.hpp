#ifndef FIRMIUS_PROVIDER_RETRYPOLICYRESOLVER_HPP
#define FIRMIUS_PROVIDER_RETRYPOLICYRESOLVER_HPP

#include "ConfigLoader.hpp"

#include <curl/curl.h>
#include <string>
#include <unordered_set>

namespace firmius::provider {

struct RetryPolicyRuntime {
  shared::RetryPolicyConfig config;
  std::unordered_set<int> retryHttpStatuses;
  std::unordered_set<int> nonRetryHttpStatuses;
  std::unordered_set<std::string> retryCurlErrors;
};

class RetryPolicyResolver {
public:
  static RetryPolicyRuntime resolve(const std::string &providerId);
  static bool isRetriableHttpStatus(const RetryPolicyRuntime &policy,
                                    int httpStatus);
  static bool isNonRetriableHttpStatus(const RetryPolicyRuntime &policy,
                                       int httpStatus);
  static bool isRetriableCurlError(const RetryPolicyRuntime &policy,
                                   CURLcode code);
  static int computeDelayMs(const RetryPolicyRuntime &policy, int attempt,
                            int headerDelayMs);
};

} // namespace firmius::provider

#endif
