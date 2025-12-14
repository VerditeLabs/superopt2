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
#include <chrono>
#include <atomic>
#include <mutex>

namespace superopt {

/// Search strategy enumeration
enum class SearchStrategy {
    Exhaustive,          // Full enumeration (default)
    IterativeDeepening,  // Depth 1, 2, 3, ...
    Stochastic,          // MCMC-based search
    Hybrid               // Exhaustive for small, stochastic for large
};

/// Verification strategy enumeration
enum class VerificationStrategy {
    RandomTesting,    // Random input testing (default)
    Algebraic,        // Pattern-based algebraic proofs
    CEGIS,            // Counterexample-guided
    SMT,              // Formal SMT verification (requires Z3)
    Hybrid            // Algebraic first, then testing
};

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

    /// Timeout per instruction in milliseconds
    size_t instructionTimeoutMs = 30000;

    /// Timeout per function in milliseconds
    size_t functionTimeoutMs = 300000;

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

    /// Search strategy
    SearchStrategy searchStrategy = SearchStrategy::IterativeDeepening;

    /// Verification strategy
    VerificationStrategy verificationStrategy = VerificationStrategy::Hybrid;

    /// Enable constant synthesis
    bool enableConstantSynthesis = true;

    /// Maximum constants to include in synthesis
    size_t maxConstants = 8;

    /// Enable observational equivalence pruning
    bool enableOEPruning = true;

    /// Number of fingerprint samples for OE pruning
    size_t numFingerprintSamples = 16;

    /// Stochastic search iterations
    size_t stochasticIterations = 10000;

    /// Stochastic search initial temperature
    double stochasticTemperature = 1.0;

    /// Enable basic block optimization
    bool enableBlockOptimization = true;

    /// Enable caching of optimization results
    bool enableCaching = true;

    /// Cache file path (empty = memory only)
    std::string cacheFilePath = "";

    /// Target triple for cost model (empty = host)
    std::string targetTriple = "";

    /// Enable SMT verification (requires Z3)
    bool enableSMT = false;
};

/// Progress information for callbacks
struct ProgressInfo {
    size_t currentFunction = 0;
    size_t totalFunctions = 0;
    size_t currentInstruction = 0;
    size_t totalInstructions = 0;
    size_t candidatesExplored = 0;
    size_t candidatesVerified = 0;
    double elapsedSeconds = 0.0;
    std::string currentItem;
    std::string phase;
};

/// Represents the result of a superoptimization attempt
struct OptimizationResult {
    bool success = false;
    std::string originalCode;
    std::string optimizedCode;
    double originalCost = 0.0;
    double optimizedCost = 0.0;
    size_t candidatesExplored = 0;
    size_t candidatesVerified = 0;
    double timeSeconds = 0.0;
    std::string message;
    bool timedOut = false;
};

/// Statistics about the superoptimization process
struct Stats {
    size_t functionsProcessed = 0;
    size_t functionsOptimized = 0;
    size_t instructionsProcessed = 0;
    size_t instructionsOptimized = 0;
    size_t blocksProcessed = 0;
    size_t blocksOptimized = 0;
    size_t candidatesGenerated = 0;
    size_t candidatesVerified = 0;
    size_t candidatesPruned = 0;
    size_t candidatesPrunedByOE = 0;
    size_t candidatesPrunedByCost = 0;
    size_t candidatesPrunedByType = 0;
    size_t algebraicProofs = 0;
    size_t cacheHits = 0;
    size_t cacheMisses = 0;
    double totalTimeSeconds = 0.0;
    double totalCostReduction = 0.0;

    void reset() { *this = Stats(); }

    Stats& operator+=(const Stats& other) {
        functionsProcessed += other.functionsProcessed;
        functionsOptimized += other.functionsOptimized;
        instructionsProcessed += other.instructionsProcessed;
        instructionsOptimized += other.instructionsOptimized;
        blocksProcessed += other.blocksProcessed;
        blocksOptimized += other.blocksOptimized;
        candidatesGenerated += other.candidatesGenerated;
        candidatesVerified += other.candidatesVerified;
        candidatesPruned += other.candidatesPruned;
        candidatesPrunedByOE += other.candidatesPrunedByOE;
        candidatesPrunedByCost += other.candidatesPrunedByCost;
        candidatesPrunedByType += other.candidatesPrunedByType;
        algebraicProofs += other.algebraicProofs;
        cacheHits += other.cacheHits;
        cacheMisses += other.cacheMisses;
        totalTimeSeconds += other.totalTimeSeconds;
        totalCostReduction += other.totalCostReduction;
        return *this;
    }
};

/// Thread-safe statistics
class AtomicStats {
public:
    std::atomic<size_t> candidatesGenerated{0};
    std::atomic<size_t> candidatesVerified{0};
    std::atomic<size_t> candidatesPruned{0};

    Stats toStats() const {
        Stats s;
        s.candidatesGenerated = candidatesGenerated.load();
        s.candidatesVerified = candidatesVerified.load();
        s.candidatesPruned = candidatesPruned.load();
        return s;
    }
};

/// Timer utility for timeouts
class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}

    double elapsedMs() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }

    double elapsedSeconds() const {
        return elapsedMs() / 1000.0;
    }

    bool exceeds(size_t milliseconds) const {
        return elapsedMs() > static_cast<double>(milliseconds);
    }

    void reset() {
        start_ = std::chrono::high_resolution_clock::now();
    }

private:
    std::chrono::high_resolution_clock::time_point start_;
};

} // namespace superopt

#endif // SUPEROPT_COMMON_H
