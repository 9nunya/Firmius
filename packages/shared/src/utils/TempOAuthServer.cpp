#include "utils/TempOAuthServer.hpp"

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#endif

#include <httplib.h>
#include <iostream>

namespace firmius::shared {

struct TempOAuthServer::ServerImpl {
  httplib::Server srv;
};

TempOAuthServer::TempOAuthServer(int port, std::string callbackPath)
    : port_(port), callbackPath_(std::move(callbackPath)),
      impl_(std::make_unique<ServerImpl>()) {}

TempOAuthServer::~TempOAuthServer() { stop(); }

bool TempOAuthServer::hasReceivedCode() const { return codeReceived_.load(); }

std::string TempOAuthServer::getCode() const { return receivedCode_; }

void TempOAuthServer::stop() {
  if (isRunning_.load()) {
    impl_->srv.stop();
    if (serverThread_.joinable()) {
      serverThread_.join();
    }
    isRunning_.store(false);
  }
}

bool TempOAuthServer::startAsync(const std::string &successHtml) {
  if (isRunning_.load())
    return false;

  impl_->srv.Get(
      callbackPath_,
      [this, successHtml](const httplib::Request &req, httplib::Response &res) {
        if (req.has_param("code")) {
          receivedCode_ = req.get_param_value("code");
          codeReceived_.store(true);
          res.set_content(successHtml, "text/html");

          // Auto stop after serving the success page
          std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            impl_->srv.stop();
          }).detach();
        } else {
          res.set_content("Missing code parameter.", "text/plain");
        }
      });

  isRunning_.store(true);
  codeReceived_.store(false);
  receivedCode_.clear();

  serverThread_ = std::thread([this]() {
    // Suppress httplib logging to stdout by default
    impl_->srv.listen("localhost", port_);
    isRunning_.store(false);
  });

  return true;
}

} // namespace firmius::shared
