#ifndef FIRMIUS_THREADS_ROUTE_HPP
#define FIRMIUS_THREADS_ROUTE_HPP

#include "APIRoute.hpp"
#include "drogon/HttpResponse.h"
#include "drogon/HttpTypes.h"

namespace firmius::web {

class ThreadsRoute : public APIRoute {
public:
  ThreadsRoute() : APIRoute("/threads", drogon::Get) {};
  ~ThreadsRoute() override = default;

  drogon::HttpResponsePtr handle(const drogon::HttpRequestPtr &req) override;
};

} // namespace firmius::web

#endif // !FIRMIUS_THREADS_ROUTE_HPP
