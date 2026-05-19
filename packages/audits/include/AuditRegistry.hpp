#ifndef FIRMIUS_AUDITS_AUDITREGISTRY_HPP
#define FIRMIUS_AUDITS_AUDITREGISTRY_HPP

#include "IAudit.hpp"
#include <memory>
#include <vector>

namespace firmius::audits {

std::vector<std::unique_ptr<shared::IAudit>> createAudits();

}

#endif
