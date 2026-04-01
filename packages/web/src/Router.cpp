#include "Router.hpp"
#include "drogon/HttpAppFramework.h"
#include <memory>

namespace firmius::web {

Router& Router::instance() {
  static Router i;
  return i;
}

void Router::registerRoute(std::unique_ptr<APIRoute> route) {
  routes_.push_back(std::move(route));
}

bool Router::handleAllRoutes() {
  if (handled_) {
    return false;
  }

  for (auto& route : routes_) {
    auto* rawRoute = route.get();
    drogon::app().registerHandler(
        rawRoute->getRoutePath(),
        [rawRoute](const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
          auto resp = rawRoute->handle(req);
          callback(resp);
        },
        {rawRoute->getMethod()});
  }

  handled_ = true;
  return true;
}

} // namespace firmius::web
