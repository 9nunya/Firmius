#ifndef FIRMIUS_ROUTER_HPP
#define FIRMIUS_ROUTER_HPP

#include "APIRoute.hpp"
#include <memory>
#include <string>
#include <vector>

namespace firmius::web {

class Router {
public:
  static Router &instance();

  Router(const Router &) = delete;
  Router &operator=(const Router &) = delete;

  void registerRoute(std::unique_ptr<APIRoute> route);
  bool handleAllRoutes();

private:
  Router() = default;
  ~Router() = default;

  std::vector<std::unique_ptr<APIRoute>> routes_;
  bool handled_ = false;
};

} // namespace firmius::web

#endif
