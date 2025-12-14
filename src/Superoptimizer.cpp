#include "superopt/Superoptimizer.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/IR/Verifier.h"

#include <chrono>
#include <regex>

namespace superopt {

Superoptimizer::Superoptimizer(const Config& config) : config_(config) {
    costModel_ = std::make_unique<CostModel>();
    enumerator_ = std::make_unique<Enumerator>(config_);
    verifier_ = std::make_unique<Verifier>(config_);
    canonicalizer_ = std::make_unique<Canonicalizer>();
    pruning_ = std::make_unique<PruningEngine>(config_, *costModel_);
}

Superoptimizer::~Superoptimizer() = default;

bool Superoptimizer::matchesFilter(const llvm::Function& func) {
    if (config_.functionFilter.empty()) {
        return true;
    }

    try {
        std::regex pattern(config_.functionFilter);
        return std::regex_search(func.getName().str(), pattern);
    } catch (const std::regex_error&) {
        // If pattern is invalid, do simple substring match
        return func.getName().str().find(config_.functionFilter) !=
               std::string::npos;
    }
}

bool Superoptimizer::optimize(llvm::Module& module) {
    shouldStop_ = false;
    auto startTime = std::chrono::high_resolution_clock::now();

    // Run pre-optimizations
    runPreOptimizations(module);

    // Canonicalize
    for (auto& func : module) {
        if (!func.isDeclaration()) {
            canonicalizer_->canonicalize(func);
        }
    }

    // Optimize each function
    bool changed = false;
    for (auto& func : module) {
        if (shouldStop_) break;

        if (func.isDeclaration()) continue;
        if (!matchesFilter(func)) continue;

        auto result = optimizeFunction(func);
        if (result.success) {
            changed = true;
            stats_.functionsOptimized++;
        }
        stats_.functionsProcessed++;
    }

    // Run post-optimizations
    if (changed) {
        runPostOptimizations(module);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    stats_.totalTimeSeconds = std::chrono::duration<double>(
        endTime - startTime).count();

    return changed;
}

OptimizationResult Superoptimizer::optimizeFunction(llvm::Function& func) {
    OptimizationResult result;
    result.originalCode = IRLoader::getIRString(func);

    auto startTime = std::chrono::high_resolution_clock::now();

    double originalCost = costModel_->getFunctionCost(func);
    result.originalCost = originalCost;

    bool changed = false;

    // Optimize each basic block
    for (auto& bb : func) {
        if (shouldStop_) break;

        auto bbResult = optimizeBasicBlock(bb);
        if (bbResult.success) {
            changed = true;
            result.candidatesExplored += bbResult.candidatesExplored;
        }
    }

    // Optimize individual instructions
    std::vector<llvm::Instruction*> instructions;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            instructions.push_back(&inst);
        }
    }

    for (auto* inst : instructions) {
        if (shouldStop_) break;

        // Skip certain instruction types
        if (inst->isTerminator()) continue;
        if (llvm::isa<llvm::PHINode>(inst)) continue;
        if (llvm::isa<llvm::AllocaInst>(inst)) continue;
        if (inst->mayHaveSideEffects()) continue;

        auto instResult = optimizeInstruction(*inst);
        if (instResult.success) {
            changed = true;
            result.candidatesExplored += instResult.candidatesExplored;
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    result.timeSeconds = std::chrono::duration<double>(
        endTime - startTime).count();

    if (changed) {
        result.success = true;
        result.optimizedCode = IRLoader::getIRString(func);
        result.optimizedCost = costModel_->getFunctionCost(func);
        stats_.totalCostReduction += result.originalCost - result.optimizedCost;
    }

    return result;
}

OptimizationResult Superoptimizer::optimizeBasicBlock(llvm::BasicBlock& bb) {
    OptimizationResult result;

    double originalCost = costModel_->getBasicBlockCost(bb);

    // Search for better sequence
    auto optimal = searchOptimalBlock(bb, originalCost);

    if (optimal) {
        // Found a better sequence
        if (applySequenceToBlock(bb, *optimal)) {
            result.success = true;
            result.optimizedCost = costModel_->getBasicBlockCost(bb);
        }
    }

    result.candidatesExplored = enumerator_->getCandidatesGenerated();
    return result;
}

OptimizationResult Superoptimizer::optimizeInstruction(llvm::Instruction& inst) {
    OptimizationResult result;

    double originalCost = costModel_->getInstructionCost(inst);

    // Search for better sequence
    auto optimal = searchOptimal(inst, originalCost);

    if (optimal) {
        // Found a better sequence
        if (applySequence(inst, *optimal)) {
            result.success = true;
        }
    }

    result.candidatesExplored = enumerator_->getCandidatesGenerated();
    return result;
}

std::optional<SynthesizedSequence> Superoptimizer::searchOptimal(
    llvm::Instruction& inst,
    double currentCost) {

    std::optional<SynthesizedSequence> best;
    double bestCost = currentCost;

    // Reset statistics
    enumerator_->resetStats();
    pruning_->clearSeen();

    auto& ctx = inst.getContext();

    // Enumerate candidates
    enumerator_->enumerateReplacements(inst, config_.maxInstructions,
        [&](const SynthesizedSequence& candidate) -> bool {
            if (shouldStop_) return false;

            stats_.candidatesGenerated++;

            // Estimate cost of candidate
            double candidateCost = 0.0;
            for (const auto& templ : candidate.templates) {
                candidateCost += costModel_->getDefaultCost(templ.opcode);
            }

            // Skip if not cheaper
            if (candidateCost >= bestCost) {
                stats_.candidatesPruned++;
                return true;  // Continue
            }

            // Verify equivalence
            auto verifyResult = verifier_->verify(inst, candidate, ctx);

            if (verifyResult == VerificationResult::Equivalent) {
                stats_.candidatesVerified++;

                // Check improvement threshold
                double improvement = costModel_->getImprovementRatio(
                    currentCost, candidateCost);

                if (improvement >= config_.minCostImprovement) {
                    best = candidate;
                    bestCost = candidateCost;

                    if (config_.debug) {
                        llvm::errs() << "Found better sequence: cost "
                                    << candidateCost << " (was "
                                    << currentCost << ")\n";
                    }
                }
            }

            return true;  // Continue enumeration
        });

    return best;
}

std::optional<SynthesizedSequence> Superoptimizer::searchOptimalBlock(
    llvm::BasicBlock& bb,
    double currentCost) {

    // For now, we optimize instruction-by-instruction
    // Full block optimization would require more complex enumeration
    return std::nullopt;
}

bool Superoptimizer::applySequence(llvm::Instruction& original,
                                    const SynthesizedSequence& seq) {
    // Create IR builder positioned before the original instruction
    llvm::IRBuilder<> builder(&original);

    // Collect operands from original instruction
    std::vector<llvm::Value*> inputs;
    for (auto& op : original.operands()) {
        inputs.push_back(op);
    }

    // Apply the sequence
    llvm::Value* result = seq.apply(builder, inputs);

    if (!result) {
        return false;
    }

    // Replace uses of original with new result
    original.replaceAllUsesWith(result);

    // Don't erase yet - let DCE handle it
    return true;
}

bool Superoptimizer::applySequenceToBlock(llvm::BasicBlock& bb,
                                           const SynthesizedSequence& seq) {
    // More complex - need to replace entire block contents
    // For now, return false (not implemented)
    return false;
}

void Superoptimizer::reportProgress(const std::string& message, double progress) {
    if (progressCallback_) {
        progressCallback_(message, progress);
    }
}

void Superoptimizer::runPreOptimizations(llvm::Module& module) {
    // Run basic LLVM optimizations to clean up the IR first
    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    // Run a lightweight optimization pipeline
    llvm::ModulePassManager mpm = pb.buildO1ModuleOptimization();
    mpm.run(module, mam);
}

void Superoptimizer::runPostOptimizations(llvm::Module& module) {
    // Run cleanup passes after superoptimization
    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    // Run DCE and simplification
    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::DCEPass());

    llvm::ModulePassManager mpm;
    mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    mpm.run(module, mam);
}

// SuperoptimizerPass implementation

SuperoptimizerPass::SuperoptimizerPass(const Config& config)
    : config_(config) {}

llvm::PreservedAnalyses SuperoptimizerPass::run(
    llvm::Function& F,
    llvm::FunctionAnalysisManager& AM) {

    Superoptimizer superopt(config_);
    auto result = superopt.optimizeFunction(F);

    if (result.success) {
        return llvm::PreservedAnalyses::none();
    }
    return llvm::PreservedAnalyses::all();
}

// SuperoptimizerLegacyPass implementation

char SuperoptimizerLegacyPass::ID = 0;

SuperoptimizerLegacyPass::SuperoptimizerLegacyPass(const Config& config)
    : llvm::FunctionPass(ID), config_(config) {}

bool SuperoptimizerLegacyPass::runOnFunction(llvm::Function& F) {
    Superoptimizer superopt(config_);
    auto result = superopt.optimizeFunction(F);
    return result.success;
}

void SuperoptimizerLegacyPass::getAnalysisUsage(llvm::AnalysisUsage& AU) const {
    // We may modify the function
}

} // namespace superopt
