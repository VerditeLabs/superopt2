#ifndef SUPEROPT_SUPEROPTIMIZER_H
#define SUPEROPT_SUPEROPTIMIZER_H

#include "superopt/Common.h"
#include "superopt/IRLoader.h"
#include "superopt/CostModel.h"
#include "superopt/Enumerator.h"
#include "superopt/Verifier.h"
#include "superopt/Canonicalizer.h"
#include "superopt/Pruning.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

#include <chrono>
#include <atomic>

namespace superopt {

/// Main superoptimizer class
class Superoptimizer {
public:
    explicit Superoptimizer(const Config& config = Config());
    ~Superoptimizer();

    /// Optimize a module
    bool optimize(llvm::Module& module);

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
    using ProgressCallback = std::function<void(const std::string&, double)>;
    void setProgressCallback(ProgressCallback cb) { progressCallback_ = cb; }

    /// Stop optimization (for async cancellation)
    void stop() { shouldStop_ = true; }

    /// Check if optimization was stopped
    bool wasStopped() const { return shouldStop_; }

    /// Get the configuration
    const Config& getConfig() const { return config_; }

    /// Modify the configuration
    Config& getConfig() { return config_; }

private:
    Config config_;
    Stats stats_;
    std::unique_ptr<CostModel> costModel_;
    std::unique_ptr<Enumerator> enumerator_;
    std::unique_ptr<Verifier> verifier_;
    std::unique_ptr<Canonicalizer> canonicalizer_;
    std::unique_ptr<PruningEngine> pruning_;
    ProgressCallback progressCallback_;
    std::atomic<bool> shouldStop_{false};

    /// Search for optimal sequence for an instruction
    std::optional<SynthesizedSequence> searchOptimal(
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
    void reportProgress(const std::string& message, double progress);

    /// Check if function matches filter
    bool matchesFilter(const llvm::Function& func);

    /// Run LLVM's standard optimizations as a pre-pass
    void runPreOptimizations(llvm::Module& module);

    /// Run LLVM's standard optimizations as a post-pass
    void runPostOptimizations(llvm::Module& module);
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
