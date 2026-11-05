#pragma once
#include "pikaflow/ir.hpp"
#include <cstddef>

namespace pika {
struct OptimizationStats {
    size_t constantsFolded = 0;
    size_t branchesFolded = 0;
    size_t unreachableRemoved = 0;
    size_t functionsSkipped = 0;
    size_t commonExpressionsReused = 0;
    size_t cellLoadsForwarded = 0;
    size_t deadResultsRemoved = 0;
    size_t livenessSkipped = 0;
};

// Returns a verified copy. The reference interpreter can retain the original.
Program optimizeIR(const Program& program, OptimizationStats* stats = nullptr);
} // namespace pika
