#ifndef FIRMIUS_API_ROUTE_HPP
#define FIRMIUS_API_ROUTE_HPP

#include "drogon/HttpRequest.h"
#include "drogon/HttpResponse.h"
#include "drogon/HttpTypes.h"
#include <memory>
#include <string>

namespace firmius::web {

class APIRoute {
public:
  APIRoute(std::string route_, drogon::HttpMethod method_)
      : route(std::move(route_)), method(method_) {}
  virtual ~APIRoute() = default;

  virtual drogon::HttpResponsePtr handle(const drogon::HttpRequestPtr &req) = 0;

  const std::string &getRoutePath() const { return route; }
  drogon::HttpMethod getMethod() const { return method; }

private:
  std::string route;
  drogon::HttpMethod method;
};

} // namespace firmius::web

#endif
