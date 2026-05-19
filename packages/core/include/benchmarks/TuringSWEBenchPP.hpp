#ifndef FIRMIUS_CORE_TURINGSWEBENCHPP_HPP
#define FIRMIUS_CORE_TURINGSWEBENCHPP_HPP

#include "benchmarks/SWEBench.hpp"

namespace firmius::core {

class TuringSWEBenchPP : public SWEBench {
public:
    explicit TuringSWEBenchPP(BenchmarkConfig config);
};

} // namespace firmius::core

#endif