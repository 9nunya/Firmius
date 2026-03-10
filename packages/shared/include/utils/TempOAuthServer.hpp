#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace firmius::shared {

// A simple localhost HTTP server designed to intercept an OAuth callback
class TempOAuthServer {
public:
  TempOAuthServer(int port = 51121,
                  std::string callbackPath = "/oauth-callback");
  ~TempOAuthServer();

  // Start the server asynchronously. It will handle exactly one request
  // containing a `code` query parameter and return the specified success HTML.
  bool startAsync(const std::string &successHtml);

  // Stop the server manually
  void stop();

  // Check if the OAuth code has been received
  bool hasReceivedCode() const;

  // Get the OAuth code if received
  std::string getCode() const;

private:
  int port_;
  std::string callbackPath_;
  std::atomic<bool> isRunning_{false};
  std::atomic<bool> codeReceived_{false};
  std::string receivedCode_;

  // Opaque pointer to the server instance to avoid polluting headers
  struct ServerImpl;
  std::unique_ptr<ServerImpl> impl_;
  std::thread serverThread_;
};

} // namespace firmius::shared
