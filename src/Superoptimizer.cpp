#include "superopt/Superoptimizer.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/IR/Verifier.h"

#include <chrono>
#include <regex>
#include <future>

namespace superopt {

Superoptimizer::Superoptimizer(const Config& config) : config_(config) {
    costModel_ = std::make_unique<CostModel>();
    enumerator_ = std::make_unique<Enumerator>(config_);
    stochasticEnumerator_ = std::make_unique<StochasticEnumerator>(config_);
    verifier_ = std::make_unique<HybridVerifier>(config_);
    canonicalizer_ = std::make_unique<Canonicalizer>();
    pruning_ = std::make_unique<PruningEngine>(config_, *costModel_);
    oeChecker_ = std::make_unique<ObservationalEquivalence>(config_);
    cache_ = std::make_unique<OptimizationCache>(config_.cacheFilePath);
    globalTimer_.reset();
}

Superoptimizer::~Superoptimizer() = default;

bool Superoptimizer::matchesFilter(const llvm::Function& func) {
    if (config_.functionFilter.empty()) {
        return true;
    }

    // Try simple substring match first (exceptions disabled in LLVM build)
    return func.getName().str().find(config_.functionFilter) !=
           std::string::npos;
}

bool Superoptimizer::shouldStopNow() const {
    if (shouldStop_) {
        return true;
    }
    if (config_.functionTimeoutMs > 0 &&
        globalTimer_.exceeds(config_.functionTimeoutMs)) {
        return true;
    }
    return false;
}

void Superoptimizer::updateStats(const Stats& delta) {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_ += delta;
}

void Superoptimizer::reportProgress(const ProgressInfo& info) {
    if (progressCallback_) {
        progressCallback_(info);
    }
}

bool Superoptimizer::optimize(llvm::Module& module) {
    shouldStop_ = false;
    globalTimer_.reset();

    runPreOptimizations(module);

    for (auto& func : module) {
        if (!func.isDeclaration()) {
            canonicalizer_->canonicalize(func);
        }
    }

    bool changed = false;
    size_t totalFunctions = 0;
    for (auto& func : module) {
        if (!func.isDeclaration() && matchesFilter(func)) {
            totalFunctions++;
        }
    }

    size_t currentFunction = 0;
    for (auto& func : module) {
        if (shouldStopNow()) break;
        if (func.isDeclaration()) continue;
        if (!matchesFilter(func)) continue;

        currentFunction++;

        ProgressInfo progress;
        progress.currentFunction = currentFunction;
        progress.totalFunctions = totalFunctions;
        progress.currentItem = func.getName().str();
        progress.phase = "optimizing";
        progress.elapsedSeconds = globalTimer_.elapsedSeconds();
        reportProgress(progress);

        auto result = optimizeFunction(func);
        if (result.success) {
            changed = true;
            stats_.functionsOptimized++;
        }
        stats_.functionsProcessed++;
    }

    if (changed) {
        runPostOptimizations(module);
    }

    stats_.totalTimeSeconds = globalTimer_.elapsedSeconds();
    return changed;
}

bool Superoptimizer::optimizeParallel(llvm::Module& module) {
    if (!config_.parallel || config_.numThreads <= 1) {
        return optimize(module);
    }

    shouldStop_ = false;
    globalTimer_.reset();

    runPreOptimizations(module);

    for (auto& func : module) {
        if (!func.isDeclaration()) {
            canonicalizer_->canonicalize(func);
        }
    }

    std::vector<llvm::Function*> functions;
    for (auto& func : module) {
        if (!func.isDeclaration() && matchesFilter(func)) {
            functions.push_back(&func);
        }
    }

    std::atomic<bool> changed{false};
    std::atomic<size_t> completedFunctions{0};

    auto worker = [&](size_t startIdx, size_t endIdx) {
        Superoptimizer localOpt(config_);
        for (size_t i = startIdx; i < endIdx && !shouldStopNow(); ++i) {
            auto result = localOpt.optimizeFunction(*functions[i]);
            if (result.success) {
                changed = true;
            }
            completedFunctions++;

            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_ += localOpt.getStats();
            localOpt.resetStats();
        }
    };

    std::vector<std::thread> threads;
    size_t functionsPerThread = (functions.size() + config_.numThreads - 1) / config_.numThreads;

    for (size_t t = 0; t < config_.numThreads; ++t) {
        size_t start = t * functionsPerThread;
        size_t end = std::min(start + functionsPerThread, functions.size());
        if (start < end) {
            threads.emplace_back(worker, start, end);
        }
    }

    for (auto& thread : threads) {
        thread.join();
    }

    stats_.functionsProcessed = functions.size();

    if (changed) {
        runPostOptimizations(module);
    }

    stats_.totalTimeSeconds = globalTimer_.elapsedSeconds();
    return changed;
}

OptimizationResult Superoptimizer::optimizeFunction(llvm::Function& func) {
    OptimizationResult result;
    result.originalCode = IRLoader::getIRString(func);

    Timer funcTimer;
    double originalCost = costModel_->getFunctionCost(func);
    result.originalCost = originalCost;

    bool changed = false;

    if (config_.enableBlockOptimization) {
        for (auto& bb : func) {
            if (shouldStopNow()) break;
            auto bbResult = optimizeBasicBlock(bb);
            if (bbResult.success) {
                changed = true;
                result.candidatesExplored += bbResult.candidatesExplored;
            }
        }
    }

    std::vector<llvm::Instruction*> instructions;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            instructions.push_back(&inst);
        }
    }

    size_t totalInstructions = instructions.size();
    for (size_t i = 0; i < instructions.size(); ++i) {
        auto* inst = instructions[i];
        if (shouldStopNow()) break;

        if (inst->isTerminator()) continue;
        if (llvm::isa<llvm::PHINode>(inst)) continue;
        if (llvm::isa<llvm::AllocaInst>(inst)) continue;
        if (inst->mayHaveSideEffects()) continue;

        auto instResult = processInstruction(*inst, i, totalInstructions);
        if (instResult.success) {
            changed = true;
            result.candidatesExplored += instResult.candidatesExplored;
            result.candidatesVerified += instResult.candidatesVerified;
        }
    }

    result.timeSeconds = funcTimer.elapsedSeconds();
    result.timedOut = funcTimer.exceeds(config_.functionTimeoutMs);

    if (changed) {
        result.success = true;
        result.optimizedCode = IRLoader::getIRString(func);
        result.optimizedCost = costModel_->getFunctionCost(func);
        stats_.totalCostReduction += result.originalCost - result.optimizedCost;
    }

    return result;
}

OptimizationResult Superoptimizer::processInstruction(llvm::Instruction& inst,
                                                       size_t index, size_t total) {
    OptimizationResult result;
    stats_.instructionsProcessed++;

    if (config_.enableCaching) {
        auto cached = cache_->lookup(inst);
        if (cached) {
            stats_.cacheHits++;
            if (cached->hasOptimization) {
                if (applySequence(inst, cached->sequence)) {
                    result.success = true;
                    stats_.instructionsOptimized++;
                }
            }
            return result;
        }
        stats_.cacheMisses++;
    }

    double originalCost = costModel_->getInstructionCost(inst);

    auto optimal = searchOptimal(inst, originalCost);

    if (optimal) {
        if (applySequence(inst, *optimal)) {
            result.success = true;
            stats_.instructionsOptimized++;

            if (config_.enableCaching) {
                double newCost = 0;
                for (const auto& t : optimal->templates) {
                    newCost += costModel_->getDefaultCost(t.opcode);
                }
                cache_->store(inst, *optimal, originalCost, newCost);
            }
        }
    } else if (config_.enableCaching) {
        cache_->storeNoOptimization(inst, originalCost);
    }

    result.candidatesExplored = stats_.candidatesGenerated;
    result.candidatesVerified = stats_.candidatesVerified;
    return result;
}

OptimizationResult Superoptimizer::optimizeBasicBlock(llvm::BasicBlock& bb) {
    OptimizationResult result;
    stats_.blocksProcessed++;

    double originalCost = costModel_->getBasicBlockCost(bb);
    auto optimal = searchOptimalBlock(bb, originalCost);

    if (optimal) {
        if (applySequenceToBlock(bb, *optimal)) {
            result.success = true;
            result.optimizedCost = costModel_->getBasicBlockCost(bb);
            stats_.blocksOptimized++;
        }
    }

    result.candidatesExplored = enumerator_->getCandidatesGenerated();
    return result;
}

OptimizationResult Superoptimizer::optimizeInstruction(llvm::Instruction& inst) {
    return processInstruction(inst, 0, 1);
}

std::optional<SynthesizedSequence> Superoptimizer::searchOptimal(
    llvm::Instruction& inst,
    double currentCost) {

    switch (config_.searchStrategy) {
        case SearchStrategy::Exhaustive:
            return searchExhaustive(inst, currentCost);
        case SearchStrategy::IterativeDeepening:
            return searchIterativeDeepening(inst, currentCost);
        case SearchStrategy::Stochastic:
            return searchStochastic(inst, currentCost);
        case SearchStrategy::Hybrid:
            return searchHybrid(inst, currentCost);
        default:
            return searchIterativeDeepening(inst, currentCost);
    }
}

std::optional<SynthesizedSequence> Superoptimizer::searchExhaustive(
    llvm::Instruction& inst,
    double currentCost) {

    std::optional<SynthesizedSequence> best;
    double bestCost = currentCost;

    enumerator_->resetStats();
    pruning_->clearSeen();
    oeChecker_->clear();

    auto& ctx = inst.getContext();

    std::vector<llvm::Type*> inputTypes;
    for (auto& op : inst.operands()) {
        inputTypes.push_back(op->getType());
    }

    ConstantPool pool = getConstantPool(inst);

    enumerator_->enumerateWithConstants(inst.getType(), inputTypes, pool, config_.maxInstructions,
        [&](const SynthesizedSequence& candidate) -> bool {
            if (shouldStopNow()) return false;

            stats_.candidatesGenerated++;

            double candidateCost = 0.0;
            for (const auto& templ : candidate.templates) {
                candidateCost += costModel_->getDefaultCost(templ.opcode);
            }

            if (candidateCost >= bestCost) {
                stats_.candidatesPrunedByCost++;
                return true;
            }

            if (config_.enableOEPruning) {
                if (!oeChecker_->isNewEquivalenceClass(candidate, inputTypes, ctx)) {
                    stats_.candidatesPrunedByOE++;
                    return true;
                }
            }

            auto verifyResult = verifyCandidate(inst, candidate);

            if (verifyResult == VerificationResult::Equivalent) {
                stats_.candidatesVerified++;

                double improvement = costModel_->getImprovementRatio(currentCost, candidateCost);
                if (improvement >= config_.minCostImprovement) {
                    best = candidate;
                    bestCost = candidateCost;

                    if (config_.debug) {
                        llvm::errs() << "Found better sequence: cost "
                                    << candidateCost << " (was " << currentCost << ")\n";
                    }
                }
            }

            return true;
        });

    return best;
}

std::optional<SynthesizedSequence> Superoptimizer::searchIterativeDeepening(
    llvm::Instruction& inst,
    double currentCost) {

    std::optional<SynthesizedSequence> best;
    double bestCost = currentCost;

    auto& ctx = inst.getContext();

    std::vector<llvm::Type*> inputTypes;
    for (auto& op : inst.operands()) {
        inputTypes.push_back(op->getType());
    }

    ConstantPool pool = getConstantPool(inst);

    for (size_t depth = 1; depth <= config_.maxInstructions; ++depth) {
        if (shouldStopNow()) break;

        enumerator_->resetStats();
        pruning_->clearSeen();
        oeChecker_->clear();

        bool foundAtDepth = false;

        enumerator_->enumerateIterativeDeepening(inst.getType(), inputTypes, depth,
            [&](const SynthesizedSequence& candidate) -> bool {
                if (shouldStopNow()) return false;

                stats_.candidatesGenerated++;

                double candidateCost = 0.0;
                for (const auto& templ : candidate.templates) {
                    candidateCost += costModel_->getDefaultCost(templ.opcode);
                }

                if (candidateCost >= bestCost) {
                    stats_.candidatesPrunedByCost++;
                    return true;
                }

                if (config_.enableOEPruning) {
                    if (!oeChecker_->isNewEquivalenceClass(candidate, inputTypes, ctx)) {
                        stats_.candidatesPrunedByOE++;
                        return true;
                    }
                }

                auto verifyResult = verifyCandidate(inst, candidate);

                if (verifyResult == VerificationResult::Equivalent) {
                    stats_.candidatesVerified++;

                    double improvement = costModel_->getImprovementRatio(currentCost, candidateCost);
                    if (improvement >= config_.minCostImprovement) {
                        best = candidate;
                        bestCost = candidateCost;
                        foundAtDepth = true;

                        if (config_.debug) {
                            llvm::errs() << "Found at depth " << depth << ": cost "
                                        << candidateCost << " (was " << currentCost << ")\n";
                        }
                    }
                }

                return true;
            });

        if (foundAtDepth && bestCost < currentCost * 0.5) {
            break;
        }
    }

    return best;
}

std::optional<SynthesizedSequence> Superoptimizer::searchStochastic(
    llvm::Instruction& inst,
    double currentCost) {

    auto& ctx = inst.getContext();

    std::vector<llvm::Type*> inputTypes;
    for (auto& op : inst.operands()) {
        inputTypes.push_back(op->getType());
    }

    ConstantPool pool = getConstantPool(inst);

    // Cost function
    auto costFn = [this](const SynthesizedSequence& candidate) -> double {
        double cost = 0.0;
        for (const auto& templ : candidate.templates) {
            cost += costModel_->getDefaultCost(templ.opcode);
        }
        return cost;
    };

    // Verification function
    auto verifyFn = [this, &inst](const SynthesizedSequence& candidate) -> bool {
        auto result = verifyCandidate(inst, candidate);
        return result == VerificationResult::Equivalent;
    };

    auto result = stochasticEnumerator_->search(
        inst.getType(), inputTypes, pool, costFn, verifyFn,
        config_.stochasticIterations);

    if (result && config_.debug) {
        double cost = costFn(*result);
        llvm::errs() << "Stochastic found: cost " << cost
                    << " (was " << currentCost << ")\n";
    }

    return result;
}

std::optional<SynthesizedSequence> Superoptimizer::searchHybrid(
    llvm::Instruction& inst,
    double currentCost) {

    auto result = searchIterativeDeepening(inst, currentCost);

    if (result) {
        double resultCost = 0;
        for (const auto& t : result->templates) {
            resultCost += costModel_->getDefaultCost(t.opcode);
        }
        if (resultCost < currentCost * 0.7) {
            return result;
        }
    }

    auto stochasticResult = searchStochastic(inst, currentCost);

    if (stochasticResult) {
        if (!result) {
            return stochasticResult;
        }

        double resultCost = 0, stochasticCost = 0;
        for (const auto& t : result->templates) {
            resultCost += costModel_->getDefaultCost(t.opcode);
        }
        for (const auto& t : stochasticResult->templates) {
            stochasticCost += costModel_->getDefaultCost(t.opcode);
        }

        return stochasticCost < resultCost ? stochasticResult : result;
    }

    return result;
}

std::optional<SynthesizedSequence> Superoptimizer::searchOptimalBlock(
    llvm::BasicBlock& bb,
    double currentCost) {
    return std::nullopt;
}

VerificationResult Superoptimizer::verifyCandidate(
    const llvm::Instruction& original,
    const SynthesizedSequence& candidate) {

    auto& ctx = const_cast<llvm::Instruction&>(original).getContext();
    return verifier_->verify(original, candidate, ctx);
}

ConstantPool Superoptimizer::getConstantPool(const llvm::Instruction& inst) {
    auto* type = inst.getType();
    auto& ctx = const_cast<llvm::Instruction&>(inst).getContext();

    ConstantPool pool = ConstantPool::getDefaultPool(ctx, type);

    for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(i))) {
            SynthesisConstant sc;
            sc.value = const_cast<llvm::ConstantInt*>(ci);
            sc.intValue = ci->getSExtValue();
            sc.isPowerOfTwo = ci->getValue().isPowerOf2();
            pool.add(sc);
        }
    }

    return pool;
}

bool Superoptimizer::applySequence(llvm::Instruction& original,
                                    const SynthesizedSequence& seq) {
    llvm::IRBuilder<> builder(&original);

    std::vector<llvm::Value*> inputs;
    for (auto& op : original.operands()) {
        inputs.push_back(op);
    }

    llvm::Value* result = seq.apply(builder, inputs);

    if (!result) {
        return false;
    }

    original.replaceAllUsesWith(result);
    return true;
}

bool Superoptimizer::applySequenceToBlock(llvm::BasicBlock& bb,
                                           const SynthesizedSequence& seq) {
    return false;
}

void Superoptimizer::runPreOptimizations(llvm::Module& module) {
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

    // Build optimization pipeline
    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O1);
    mpm.run(module, mam);
}

void Superoptimizer::runPostOptimizations(llvm::Module& module) {
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

    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::DCEPass());

    llvm::ModulePassManager mpm;
    mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    mpm.run(module, mam);
}

bool Superoptimizer::loadCache(const std::string& path) {
    return cache_->load(path);
}

bool Superoptimizer::saveCache(const std::string& path) {
    return cache_->save(path);
}

//===----------------------------------------------------------------------===//
// ParallelSuperoptimizer implementation
//===----------------------------------------------------------------------===//

ParallelSuperoptimizer::ParallelSuperoptimizer(const Config& config, size_t numThreads)
    : config_(config), numThreads_(numThreads) {

    for (size_t i = 0; i < numThreads_; ++i) {
        workers_.emplace_back(&ParallelSuperoptimizer::workerLoop, this);
    }
}

ParallelSuperoptimizer::~ParallelSuperoptimizer() {
    stop();
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ParallelSuperoptimizer::workerLoop() {
    while (!shouldStop_) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            condition_.wait(lock, [this] {
                return shouldStop_ || !taskQueue_.empty();
            });

            if (shouldStop_ && taskQueue_.empty()) {
                return;
            }

            task = std::move(taskQueue_.front());
            taskQueue_.pop();
        }

        task();
    }
}

std::vector<OptimizationResult> ParallelSuperoptimizer::optimizeInstructions(
    std::vector<llvm::Instruction*>& instructions) {

    std::vector<OptimizationResult> results(instructions.size());
    std::atomic<size_t> nextIdx{0};

    auto processTask = [&]() {
        Superoptimizer localOpt(config_);

        while (!shouldStop_) {
            size_t idx = nextIdx.fetch_add(1);
            if (idx >= instructions.size()) break;

            results[idx] = localOpt.optimizeInstruction(*instructions[idx]);

            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_ += localOpt.getStats();
            localOpt.resetStats();
        }
    };

    for (size_t i = 0; i < numThreads_; ++i) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(processTask);
    }

    condition_.notify_all();

    while (nextIdx.load() < instructions.size() && !shouldStop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return results;
}

std::vector<OptimizationResult> ParallelSuperoptimizer::optimizeFunctions(
    std::vector<llvm::Function*>& functions) {

    std::vector<OptimizationResult> results(functions.size());
    std::atomic<size_t> nextIdx{0};

    auto processTask = [&]() {
        Superoptimizer localOpt(config_);

        while (!shouldStop_) {
            size_t idx = nextIdx.fetch_add(1);
            if (idx >= functions.size()) break;

            results[idx] = localOpt.optimizeFunction(*functions[idx]);

            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_ += localOpt.getStats();
            localOpt.resetStats();
        }
    };

    for (size_t i = 0; i < numThreads_; ++i) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(processTask);
    }

    condition_.notify_all();

    while (nextIdx.load() < functions.size() && !shouldStop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return results;
}

void ParallelSuperoptimizer::stop() {
    shouldStop_ = true;
}

Stats ParallelSuperoptimizer::getStats() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(statsMutex_));
    return stats_;
}

//===----------------------------------------------------------------------===//
// Pass implementations
//===----------------------------------------------------------------------===//

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

char SuperoptimizerLegacyPass::ID = 0;

SuperoptimizerLegacyPass::SuperoptimizerLegacyPass(const Config& config)
    : llvm::FunctionPass(ID), config_(config) {}

bool SuperoptimizerLegacyPass::runOnFunction(llvm::Function& F) {
    Superoptimizer superopt(config_);
    auto result = superopt.optimizeFunction(F);
    return result.success;
}

void SuperoptimizerLegacyPass::getAnalysisUsage(llvm::AnalysisUsage& AU) const {
}

} // namespace superopt
