#ifndef FIRMIUS_CORE_TURING_SWEBENCHPP_HPP
#define FIRMIUS_CORE_TURING_SWEBENCHPP_HPP

#include "benchmarks/SWEBench.hpp"

namespace firmius::core {

class TuringSWEBenchPP : public SWEBench {
public:
    explicit TuringSWEBenchPP(BenchmarkConfig config);
};

} // namespace firmius::core

#endif