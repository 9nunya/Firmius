#ifndef FIRMIUS_AUDITS_AUDIT_REGISTRY_HPP
#define FIRMIUS_AUDITS_AUDIT_REGISTRY_HPP

#include "IAudit.hpp"
#include <memory>
#include <vector>

namespace firmius::audits {

std::vector<std::unique_ptr<shared::IAudit>> createAudits();

}

#endif
