#include "routes/ThreadsRoute.hpp"
#include "drogon/HttpRequest.h"
#include "drogon/HttpResponse.h"

namespace firmius::web {

drogon::HttpResponsePtr ThreadsRoute::handle(const drogon::HttpRequestPtr &) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setBody("bro what");

  return resp;
}

} // namespace firmius::web
