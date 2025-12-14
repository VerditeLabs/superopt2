#ifndef SUPEROPT_COMMON_H
#define SUPEROPT_COMMON_H

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace superopt {

/// Configuration for the superoptimizer
struct Config {
    /// Maximum number of instructions to enumerate
    size_t maxInstructions = 5;

    /// Maximum search depth for synthesis
    size_t maxSearchDepth = 10;

    /// Number of random test inputs for verification
    size_t numTestInputs = 100;

    /// Timeout per candidate in milliseconds
    size_t candidateTimeoutMs = 1000;

    /// Enable debug output
    bool debug = false;

    /// Enable parallel search
    bool parallel = false;

    /// Number of threads for parallel search
    size_t numThreads = 4;

    /// Only optimize functions matching this pattern
    std::string functionFilter = "";

    /// Minimum cost improvement to accept optimization
    double minCostImprovement = 0.1;
};

/// Represents the result of a superoptimization attempt
struct OptimizationResult {
    bool success = false;
    std::string originalCode;
    std::string optimizedCode;
    double originalCost = 0.0;
    double optimizedCost = 0.0;
    size_t candidatesExplored = 0;
    double timeSeconds = 0.0;
    std::string message;
};

/// Statistics about the superoptimization process
struct Stats {
    size_t functionsProcessed = 0;
    size_t functionsOptimized = 0;
    size_t candidatesGenerated = 0;
    size_t candidatesVerified = 0;
    size_t candidatesPruned = 0;
    double totalTimeSeconds = 0.0;
    double totalCostReduction = 0.0;
};

} // namespace superopt

#endif // SUPEROPT_COMMON_H
