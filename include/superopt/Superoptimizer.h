#ifndef SUPEROPT_SUPEROPTIMIZER_H
#define SUPEROPT_SUPEROPTIMIZER_H

#include "superopt/Common.h"
#include "superopt/IRLoader.h"
#include "superopt/CostModel.h"
#include "superopt/Enumerator.h"
#include "superopt/Verifier.h"
#include "superopt/Canonicalizer.h"
#include "superopt/Pruning.h"
#include "superopt/Cache.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

#include <chrono>
#include <atomic>
#include <thread>
#include <future>
#include <queue>

namespace superopt {

/// Main superoptimizer class
class Superoptimizer {
public:
    explicit Superoptimizer(const Config& config = Config());
    ~Superoptimizer();

    /// Optimize a module
    bool optimize(llvm::Module& module);

    /// Optimize a module in parallel
    bool optimizeParallel(llvm::Module& module);

    /// Optimize a single function
    OptimizationResult optimizeFunction(llvm::Function& func);

    /// Optimize a single basic block
    OptimizationResult optimizeBasicBlock(llvm::BasicBlock& bb);

    /// Optimize a single instruction (find better sequence)
    OptimizationResult optimizeInstruction(llvm::Instruction& inst);

    /// Get statistics
    const Stats& getStats() const { return stats_; }

    /// Reset statistics
    void resetStats() { stats_ = Stats(); }

    /// Set progress callback
    using ProgressCallback = std::function<void(const ProgressInfo&)>;
    void setProgressCallback(ProgressCallback cb) { progressCallback_ = cb; }

    /// Stop optimization (for async cancellation)
    void stop() { shouldStop_ = true; }

    /// Check if optimization was stopped
    bool wasStopped() const { return shouldStop_; }

    /// Get the configuration
    const Config& getConfig() const { return config_; }

    /// Modify the configuration
    Config& getConfig() { return config_; }

    /// Get the cache
    OptimizationCache& getCache() { return *cache_; }

    /// Load cache from file
    bool loadCache(const std::string& path);

    /// Save cache to file
    bool saveCache(const std::string& path);

private:
    Config config_;
    Stats stats_;
    std::unique_ptr<CostModel> costModel_;
    std::unique_ptr<Enumerator> enumerator_;
    std::unique_ptr<StochasticEnumerator> stochasticEnumerator_;
    std::unique_ptr<HybridVerifier> verifier_;
    std::unique_ptr<Canonicalizer> canonicalizer_;
    std::unique_ptr<PruningEngine> pruning_;
    std::unique_ptr<ObservationalEquivalence> oeChecker_;
    std::unique_ptr<OptimizationCache> cache_;
    ProgressCallback progressCallback_;
    std::atomic<bool> shouldStop_{false};
    Timer globalTimer_;
    mutable std::mutex statsMutex_;

    /// Search for optimal sequence using configured strategy
    std::optional<SynthesizedSequence> searchOptimal(
        llvm::Instruction& inst,
        double currentCost);

    /// Search using exhaustive enumeration
    std::optional<SynthesizedSequence> searchExhaustive(
        llvm::Instruction& inst,
        double currentCost);

    /// Search using iterative deepening
    std::optional<SynthesizedSequence> searchIterativeDeepening(
        llvm::Instruction& inst,
        double currentCost);

    /// Search using stochastic methods
    std::optional<SynthesizedSequence> searchStochastic(
        llvm::Instruction& inst,
        double currentCost);

    /// Search using hybrid approach
    std::optional<SynthesizedSequence> searchHybrid(
        llvm::Instruction& inst,
        double currentCost);

    /// Search for optimal sequence for a basic block
    std::optional<SynthesizedSequence> searchOptimalBlock(
        llvm::BasicBlock& bb,
        double currentCost);

    /// Apply a synthesized sequence, replacing original instruction
    bool applySequence(llvm::Instruction& original,
                       const SynthesizedSequence& seq);

    /// Apply a synthesized sequence, replacing basic block contents
    bool applySequenceToBlock(llvm::BasicBlock& bb,
                              const SynthesizedSequence& seq);

    /// Report progress
    void reportProgress(const ProgressInfo& info);

    /// Check if function matches filter
    bool matchesFilter(const llvm::Function& func);

    /// Run LLVM's standard optimizations as a pre-pass
    void runPreOptimizations(llvm::Module& module);

    /// Run LLVM's standard optimizations as a post-pass
    void runPostOptimizations(llvm::Module& module);

    /// Check if we should stop (timeout or manual stop)
    bool shouldStopNow() const;

    /// Update stats thread-safely
    void updateStats(const Stats& delta);

    /// Get constant pool for instruction
    ConstantPool getConstantPool(const llvm::Instruction& inst);

    /// Verify candidate with configured strategy
    VerificationResult verifyCandidate(const llvm::Instruction& original,
                                        const SynthesizedSequence& candidate);

    /// Process instruction for optimization
    OptimizationResult processInstruction(llvm::Instruction& inst,
                                           size_t index, size_t total);
};

/// Parallel worker for superoptimization
class ParallelSuperoptimizer {
public:
    ParallelSuperoptimizer(const Config& config, size_t numThreads);
    ~ParallelSuperoptimizer();

    /// Optimize multiple instructions in parallel
    std::vector<OptimizationResult> optimizeInstructions(
        std::vector<llvm::Instruction*>& instructions);

    /// Optimize multiple functions in parallel
    std::vector<OptimizationResult> optimizeFunctions(
        std::vector<llvm::Function*>& functions);

    /// Stop all workers
    void stop();

    /// Get combined statistics
    Stats getStats() const;

private:
    Config config_;
    size_t numThreads_;
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> taskQueue_;
    std::mutex queueMutex_;
    std::condition_variable condition_;
    std::atomic<bool> shouldStop_{false};
    Stats stats_;
    std::mutex statsMutex_;

    void workerLoop();
};

/// LLVM Pass wrapper for the superoptimizer
class SuperoptimizerPass : public llvm::PassInfoMixin<SuperoptimizerPass> {
public:
    explicit SuperoptimizerPass(const Config& config = Config());

    llvm::PreservedAnalyses run(llvm::Function& F,
                                 llvm::FunctionAnalysisManager& AM);

private:
    Config config_;
};

/// Legacy function pass wrapper
class SuperoptimizerLegacyPass : public llvm::FunctionPass {
public:
    static char ID;

    explicit SuperoptimizerLegacyPass(const Config& config = Config());

    bool runOnFunction(llvm::Function& F) override;

    void getAnalysisUsage(llvm::AnalysisUsage& AU) const override;

private:
    Config config_;
};

} // namespace superopt

#endif // SUPEROPT_SUPEROPTIMIZER_H
