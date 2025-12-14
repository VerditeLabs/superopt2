# LLVM Superoptimizer: Next Steps Plan

## Executive Summary

The current implementation provides a solid foundation with basic enumeration, random testing verification, and instruction-level optimization. The next steps focus on three major areas:

1. **Search Efficiency** - The exponential search space is the primary bottleneck
2. **Verification Robustness** - Random testing can miss edge cases
3. **Optimization Scope** - Currently limited to single instructions

---

## Phase 1: Critical Improvements (High Impact, Moderate Effort)

### 1.1 Constant Synthesis

**Problem**: The enumerator only uses input values as operands. It cannot synthesize constants like `0`, `1`, `-1`, or powers of 2, which are essential for many optimizations.

**Solution**: Extend `SynthesizedSequence` and `Enumerator` to include a constant pool.

```cpp
// In Enumerator.h
struct ConstantPool {
    std::vector<llvm::Constant*> constants;

    static ConstantPool getDefaultPool(llvm::LLVMContext& ctx, llvm::Type* type);
};

// Default pool for i32: {0, 1, -1, 2, -2, ...powers of 2...}
```

**Impact**: Enables strength reduction like `x * 2 → x << 1`, `x / 4 → x >> 2`.

**Files to modify**: `Enumerator.h`, `Enumerator.cpp`

---

### 1.2 Observational Equivalence Classes (OEC)

**Problem**: Many enumerated sequences compute the same function. We waste time verifying redundant candidates.

**Solution**: Use fingerprinting to group sequences into equivalence classes before expensive verification.

```cpp
// In Pruning.h
class ObservationalEquivalence {
public:
    // Compute fingerprint by evaluating on fixed test inputs
    uint64_t computeFingerprint(const SynthesizedSequence& seq,
                                 const std::vector<llvm::Type*>& inputTypes);

    // Only verify one representative per equivalence class
    bool isRepresentative(const SynthesizedSequence& seq);

private:
    // Fixed test vectors for fingerprinting (computed once)
    std::vector<std::vector<uint64_t>> testVectors_;

    // Map from fingerprint to canonical sequence
    std::unordered_map<uint64_t, SynthesizedSequence> canonicalSequences_;
};
```

**Impact**: 10-100x reduction in verification calls.

**Files to modify**: `Pruning.h`, `Pruning.cpp`, `Superoptimizer.cpp`

---

### 1.3 Better Pruning Strategies

**Problem**: The current pruning is minimal. We enumerate many obviously dead ends.

**Solutions**:

1. **Type-Directed Pruning**: Don't generate sequences that can't produce the target type
2. **Cost Bounding**: Prune partial sequences that already exceed the target cost
3. **Pattern Pruning**: Skip known-bad patterns (e.g., `x - x`, consecutive inverses)
4. **Live Value Analysis**: Don't compute values that won't be used

```cpp
// Enhanced pruning in enumeration
bool shouldPruneEarly(const SynthesizedSequence& partial,
                      llvm::Type* targetType,
                      double costBound) {
    // Type check: can any instruction produce targetType?
    if (!canProduceType(partial.availableTypes, targetType)) return true;

    // Cost check: already too expensive?
    if (computeLowerBoundCost(partial) >= costBound) return true;

    // Pattern check: contains known-bad patterns?
    if (containsBadPattern(partial)) return true;

    return false;
}
```

**Impact**: Reduces search space by orders of magnitude.

**Files to modify**: `Pruning.cpp`, `Enumerator.cpp`

---

### 1.4 Incremental/Iterative Deepening Search

**Problem**: With max-instructions=5, search can take forever. But max-instructions=2 misses optimizations.

**Solution**: Use iterative deepening - search with length 1 first, then 2, etc.

```cpp
std::optional<SynthesizedSequence> searchOptimalIterative(
    llvm::Instruction& inst, double currentCost) {

    for (size_t depth = 1; depth <= config_.maxInstructions; ++depth) {
        auto result = searchWithDepth(inst, currentCost, depth);
        if (result) {
            // Found at this depth - likely optimal
            return result;
        }
    }
    return std::nullopt;
}
```

**Impact**: Finds simple optimizations quickly, allows deeper search when needed.

**Files to modify**: `Superoptimizer.cpp`

---

## Phase 2: Verification Improvements (High Impact, Higher Effort)

### 2.1 Undefined Behavior Handling

**Problem**: Division by zero, signed overflow, and other UB can cause false equivalences or crashes.

**Solution**:
1. Filter test inputs to avoid UB-triggering values
2. Track preconditions and only verify within valid domain

```cpp
struct DomainConstraint {
    // For division: second operand != 0
    // For signed ops: no overflow
    bool isValidInput(const TestInput& input, const llvm::Instruction& inst);
};

// Generate inputs that satisfy constraints
std::vector<TestInput> generateSafeInputs(const llvm::Instruction& inst, size_t count);
```

**Files to modify**: `Verifier.cpp`

---

### 2.2 Algebraic Verification (No Execution)

**Problem**: Some equivalences can be proven algebraically without any execution.

**Solution**: Implement pattern-based algebraic verification for common cases.

```cpp
class AlgebraicVerifier {
public:
    // Try to prove equivalence algebraically
    std::optional<bool> proveEquivalent(const llvm::Instruction& original,
                                         const SynthesizedSequence& candidate);

private:
    // Known identities: x + 0 = x, x * 1 = x, x & -1 = x, etc.
    bool matchesIdentity(const SynthesizedSequence& seq);

    // Strength reduction patterns
    bool matchesStrengthReduction(const llvm::Instruction& original,
                                   const SynthesizedSequence& candidate);
};
```

**Impact**: Instant verification for common patterns.

**Files to modify**: `Verifier.h`, `Verifier.cpp`

---

### 2.3 CEGIS (Counterexample-Guided Inductive Synthesis)

**Problem**: Random testing requires many samples; can still miss corner cases.

**Solution**: Use counterexamples to refine the search.

```cpp
class CEGISVerifier {
public:
    VerificationResult verify(const llvm::Instruction& original,
                              const SynthesizedSequence& candidate) {
        std::vector<TestInput> counterexamples;

        while (true) {
            // Test on known counterexamples first
            if (!passesAllTests(candidate, counterexamples)) {
                return VerificationResult::NotEquivalent;
            }

            // Try to find new counterexample
            auto newCE = findCounterexample(original, candidate);
            if (!newCE) {
                return VerificationResult::Equivalent;  // High confidence
            }

            counterexamples.push_back(*newCE);
        }
    }
};
```

**Impact**: Higher confidence verification with fewer test cases.

**Files to modify**: `Verifier.h`, `Verifier.cpp`

---

## Phase 3: Advanced Features (High Effort)

### 3.1 Basic Block Level Optimization

**Problem**: Current implementation only optimizes single instructions. Multi-instruction patterns are missed.

**Solution**: Extend enumeration to handle basic blocks.

```cpp
// Track live-in values and outputs
struct BlockSpec {
    std::vector<llvm::Value*> inputs;   // Values defined outside
    std::vector<llvm::Value*> outputs;  // Values used outside
    std::vector<llvm::Type*> inputTypes;
    std::vector<llvm::Type*> outputTypes;
};

// Enumerate block replacements
bool enumerateBlockReplacements(const BlockSpec& spec,
                                 size_t maxLength,
                                 CandidateCallback callback);
```

**Key challenges**:
- Handling multiple outputs
- Maintaining data flow between blocks
- PHI node handling

**Files to modify**: `Enumerator.h`, `Enumerator.cpp`, `Superoptimizer.cpp`

---

### 3.2 Stochastic Search (STOKE-style)

**Problem**: Exhaustive enumeration doesn't scale beyond ~4-5 instructions.

**Solution**: Use MCMC (Markov Chain Monte Carlo) to explore the search space.

```cpp
class StochasticSearch {
public:
    std::optional<SynthesizedSequence> search(
        const llvm::Instruction& target,
        size_t maxIterations);

private:
    // Mutation operators
    SynthesizedSequence mutateOpcode(const SynthesizedSequence& seq);
    SynthesizedSequence mutateOperand(const SynthesizedSequence& seq);
    SynthesizedSequence swapInstructions(const SynthesizedSequence& seq);
    SynthesizedSequence insertInstruction(const SynthesizedSequence& seq);
    SynthesizedSequence deleteInstruction(const SynthesizedSequence& seq);

    // Metropolis-Hastings acceptance
    bool shouldAccept(double currentCost, double proposedCost, double temperature);
};
```

**Impact**: Can find longer sequences that exhaustive search can't reach.

**Files to modify**: New file `StochasticSearch.h`, `StochasticSearch.cpp`

---

### 3.3 Target-Specific Cost Models

**Problem**: Current cost model uses generic estimates. Real performance varies by target.

**Solution**: Integrate with LLVM's TargetTransformInfo.

```cpp
class TargetCostModel : public CostModel {
public:
    TargetCostModel(const llvm::TargetMachine& TM);

    double getInstructionCost(const llvm::Instruction& inst) override {
        // Use TTI for accurate cost
        return tti_.getInstructionCost(&inst, TTI::TCK_Latency);
    }

    // Model dependencies and ILP
    double getCriticalPathCost(const std::vector<llvm::Instruction*>& seq);

    // Consider register pressure
    double estimateRegisterPressure(const SynthesizedSequence& seq);

private:
    llvm::TargetTransformInfo tti_;
};
```

**Files to modify**: `CostModel.h`, `CostModel.cpp`, `main.cpp` (add `-target` option)

---

### 3.4 Optional SMT Verification (Z3)

**Problem**: Random testing is probabilistic. Some applications need formal guarantees.

**Solution**: Add optional Z3 integration for formal verification.

```cpp
#ifdef SUPEROPT_USE_Z3
class SMTVerifier {
public:
    VerificationResult verify(const llvm::Instruction& original,
                              const SynthesizedSequence& candidate);

private:
    z3::context ctx_;

    // Translate LLVM IR to Z3 expressions
    z3::expr toZ3(const llvm::Instruction& inst);
    z3::expr toZ3(const SynthesizedSequence& seq);
};
#endif
```

**CMake addition**:
```cmake
option(SUPEROPT_USE_Z3 "Enable Z3 SMT solver for formal verification" OFF)
if(SUPEROPT_USE_Z3)
    find_package(Z3 REQUIRED)
    target_link_libraries(superopt_lib PUBLIC ${Z3_LIBRARIES})
    target_compile_definitions(superopt_lib PUBLIC SUPEROPT_USE_Z3)
endif()
```

**Files to modify**: New files `SMTVerifier.h`, `SMTVerifier.cpp`, `CMakeLists.txt`

---

## Phase 4: Usability & Performance

### 4.1 Parallel Search

**Problem**: Superoptimization is embarrassingly parallel but currently single-threaded.

**Solution**: Parallelize at the function level.

```cpp
bool Superoptimizer::optimizeParallel(llvm::Module& module) {
    std::vector<std::future<OptimizationResult>> futures;

    for (auto& func : module) {
        if (func.isDeclaration()) continue;

        futures.push_back(std::async(std::launch::async, [&, this]() {
            // Each thread gets its own context and cloned function
            return optimizeFunctionThreadSafe(func);
        }));
    }

    // Collect results
    for (auto& f : futures) {
        auto result = f.get();
        // Apply optimizations...
    }
}
```

**Files to modify**: `Superoptimizer.h`, `Superoptimizer.cpp`

---

### 4.2 Caching/Memoization

**Problem**: Same instruction patterns are optimized repeatedly across functions.

**Solution**: Cache optimization results by instruction pattern.

```cpp
class OptimizationCache {
public:
    // Key: structural hash of instruction + operand types
    using CacheKey = uint64_t;

    std::optional<SynthesizedSequence> lookup(const llvm::Instruction& inst);
    void store(const llvm::Instruction& inst, const SynthesizedSequence& result);

    // Persist cache to disk
    void save(const std::string& path);
    void load(const std::string& path);
};
```

**Files to modify**: New file `Cache.h`, `Cache.cpp`, `Superoptimizer.cpp`

---

### 4.3 Progress Reporting & Timeout

**Problem**: Long-running optimizations provide no feedback and can't be bounded.

**Solution**: Add fine-grained progress reporting and per-instruction timeouts.

```cpp
struct ProgressInfo {
    size_t currentFunction;
    size_t totalFunctions;
    size_t currentInstruction;
    size_t totalInstructions;
    size_t candidatesExplored;
    double elapsedSeconds;
    std::string currentItem;
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

// Per-instruction timeout
std::optional<SynthesizedSequence> searchWithTimeout(
    llvm::Instruction& inst,
    std::chrono::milliseconds timeout);
```

**Files to modify**: `Common.h`, `Superoptimizer.h`, `Superoptimizer.cpp`

---

## Implementation Priority

| Priority | Item | Effort | Impact |
|----------|------|--------|--------|
| 1 | Constant Synthesis | Low | High |
| 2 | Observational Equivalence | Medium | High |
| 3 | Better Pruning | Medium | High |
| 4 | Iterative Deepening | Low | Medium |
| 5 | UB Handling | Medium | Medium |
| 6 | Algebraic Verification | Medium | Medium |
| 7 | Parallel Search | Medium | Medium |
| 8 | Basic Block Optimization | High | High |
| 9 | Stochastic Search | High | High |
| 10 | SMT Verification | High | Medium |

---

## Recommended Immediate Actions

1. **Implement constant synthesis** - Biggest bang for buck
2. **Add observational equivalence pruning** - Huge speedup
3. **Implement iterative deepening** - Better UX
4. **Fix UB handling** - Correctness is paramount
5. **Add basic benchmarks** - Measure before optimizing

Would you like me to start implementing any of these phases?
