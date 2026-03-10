#ifndef FIRMIUS_SHARED_IAUDIT_HPP
#define FIRMIUS_SHARED_IAUDIT_HPP

#include <string>
#include <vector>

namespace firmius::shared {

struct AuditResult {
    std::string auditId;
    int exitCode = 0;
    bool passed = true;
    std::string output;
};

class IAudit {
public:
    virtual ~IAudit() = default;
    virtual std::string getId() const = 0;
    virtual std::string getDescription() const = 0;
    virtual AuditResult run(const std::vector<std::string>& args) = 0;
};

}

#endif
