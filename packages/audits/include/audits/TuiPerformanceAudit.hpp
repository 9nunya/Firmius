#ifndef FIRMIUS_AUDITS_TUI_PERFORMANCE_AUDIT_HPP
#define FIRMIUS_AUDITS_TUI_PERFORMANCE_AUDIT_HPP

#include "IAudit.hpp"
#include "persistence/ThreadManager.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

class TuiPerformanceAudit : public shared::IAudit {
public:
    std::string getId() const override;
    std::string getDescription() const override;
    shared::AuditResult run(const std::vector<std::string>& args) override;
    std::string generateStressThread(int turns, const std::string& tempDir);
};

}

#endif
