#ifndef FIRMIUS_VIEW_RENDERER_HPP
#define FIRMIUS_VIEW_RENDERER_HPP

#include "drogon/HttpResponse.h"
#include "drogon/HttpTypes.h"
#include "drogon/HttpViewData.h"

namespace firmius::web {

template <typename ViewT>
drogon::HttpResponsePtr renderView(const drogon::HttpViewData &vd) {
  ViewT v;
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setBody(v.genText(vd));
  resp->setContentTypeCode(drogon::CT_TEXT_HTML);
  return resp;
}

} // namespace firmius::web

#endif
