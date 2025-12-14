#ifndef SUPEROPT_VERIFIER_H
#define SUPEROPT_VERIFIER_H

#include "superopt/Common.h"
#include "superopt/Enumerator.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/GenericValue.h"

#include <vector>
#include <random>
#include <optional>
#include <unordered_set>
#include <map>

namespace superopt {

/// Result of verification
enum class VerificationResult {
    Equivalent,         // Sequences are semantically equivalent
    NotEquivalent,      // Found a counterexample
    Unknown,            // Could not determine (timeout, etc.)
    Error               // Verification failed due to error
};

/// A test input for verification
struct TestInput {
    std::vector<llvm::GenericValue> values;
    std::vector<uint64_t> rawValues;  // For quick hashing/comparison

    uint64_t getHash() const;
};

/// Counterexample found during verification
struct Counterexample {
    TestInput input;
    llvm::GenericValue expectedOutput;
    llvm::GenericValue actualOutput;
    std::string description;
};

/// Domain constraints for safe execution
struct DomainConstraints {
    bool avoidDivisionByZero = true;
    bool avoidSignedOverflow = true;
    bool avoidShiftOverflow = true;

    /// Check if input is safe for the given instruction
    bool isSafeInput(const TestInput& input, const llvm::Instruction& inst) const;

    /// Check if input is safe for the given sequence
    bool isSafeInput(const TestInput& input, const SynthesizedSequence& seq) const;
};

/// Verifies semantic equivalence of instruction sequences
class Verifier {
public:
    explicit Verifier(const Config& config);
    ~Verifier();

    /// Verify that two functions are equivalent
    VerificationResult verify(const llvm::Function& original,
                             const llvm::Function& candidate);

    /// Verify that a synthesized sequence is equivalent to original instruction
    VerificationResult verify(const llvm::Instruction& original,
                             const SynthesizedSequence& candidate,
                             llvm::LLVMContext& ctx);

    /// Verify that a synthesized sequence is equivalent to a basic block
    VerificationResult verifyBlock(const llvm::BasicBlock& original,
                                   const SynthesizedSequence& candidate,
                                   llvm::LLVMContext& ctx);

    /// Get the last counterexample found (if any)
    std::optional<Counterexample> getCounterexample() const {
        return counterexample_;
    }

    /// Get the last error message
    const std::string& getLastError() const { return lastError_; }

    /// Generate random test inputs for a function
    std::vector<TestInput> generateTestInputs(const llvm::Function& func,
                                              size_t count);

    /// Generate random test inputs for given types
    std::vector<TestInput> generateTestInputs(
        const std::vector<llvm::Type*>& types,
        size_t count);

    /// Generate safe test inputs (avoiding UB)
    std::vector<TestInput> generateSafeTestInputs(
        const std::vector<llvm::Type*>& types,
        const DomainConstraints& constraints,
        size_t count);

    /// Execute a function with given inputs using interpretation
    std::optional<llvm::GenericValue> execute(
        llvm::Function& func,
        const TestInput& input);

    /// Get statistics
    size_t getTestsExecuted() const { return testsExecuted_; }
    void resetStats() { testsExecuted_ = 0; }

private:
    Config config_;
    std::optional<Counterexample> counterexample_;
    std::string lastError_;
    std::mt19937_64 rng_;
    size_t testsExecuted_ = 0;
    DomainConstraints constraints_;

    /// Create a test harness function for a synthesized sequence
    std::unique_ptr<llvm::Module> createTestHarness(
        const SynthesizedSequence& sequence,
        const std::vector<llvm::Type*>& inputTypes,
        llvm::Type* outputType,
        llvm::LLVMContext& ctx);

    /// Generate a random value of the given type
    llvm::GenericValue generateRandomValue(llvm::Type* type);

    /// Compare two generic values for equality
    bool valuesEqual(const llvm::GenericValue& a,
                     const llvm::GenericValue& b,
                     llvm::Type* type);

    /// Generate edge case values
    std::vector<uint64_t> getEdgeCases(unsigned bitWidth);
};

/// Algebraic verifier for pattern-based proofs
class AlgebraicVerifier {
public:
    explicit AlgebraicVerifier(const Config& config);

    /// Try to prove equivalence algebraically (no execution)
    /// Returns true if proven equivalent, false if cannot prove
    std::optional<bool> proveEquivalent(const llvm::Instruction& original,
                                         const SynthesizedSequence& candidate);

    /// Check if sequence matches a known identity
    bool matchesIdentity(const SynthesizedSequence& seq,
                         const std::vector<llvm::Type*>& inputTypes);

    /// Check if sequence is a strength reduction of original
    bool isStrengthReduction(const llvm::Instruction& original,
                              const SynthesizedSequence& candidate);

    /// Get number of algebraic proofs performed
    size_t getProofCount() const { return proofCount_; }

private:
    Config config_;
    size_t proofCount_ = 0;

    /// Known algebraic identities
    struct Identity {
        unsigned opcode;
        std::function<bool(const SynthesizedSequence&)> matcher;
        std::string name;
    };

    std::vector<Identity> identities_;

    void initIdentities();

    /// Check x op 0 patterns
    bool checkZeroIdentity(const SynthesizedSequence& seq, unsigned opcode);

    /// Check x op 1 patterns
    bool checkOneIdentity(const SynthesizedSequence& seq, unsigned opcode);

    /// Check x op x patterns
    bool checkSelfIdentity(const SynthesizedSequence& seq, unsigned opcode);

    /// Check x op -1 patterns
    bool checkNegOneIdentity(const SynthesizedSequence& seq, unsigned opcode);
};

/// CEGIS (Counterexample-Guided Inductive Synthesis) verifier
class CEGISVerifier {
public:
    explicit CEGISVerifier(const Config& config);

    /// Verify using CEGIS approach
    VerificationResult verify(const llvm::Instruction& original,
                              const SynthesizedSequence& candidate,
                              llvm::LLVMContext& ctx);

    /// Get counterexamples accumulated
    const std::vector<TestInput>& getCounterexamples() const {
        return counterexamples_;
    }

    /// Add external counterexample
    void addCounterexample(const TestInput& ce) {
        counterexamples_.push_back(ce);
    }

    /// Clear counterexamples
    void clearCounterexamples() { counterexamples_.clear(); }

private:
    Config config_;
    Verifier baseVerifier_;
    std::vector<TestInput> counterexamples_;

    /// Try to find a distinguishing input
    std::optional<TestInput> findCounterexample(
        const llvm::Instruction& original,
        const SynthesizedSequence& candidate,
        llvm::LLVMContext& ctx);
};

/// Combined verifier using multiple strategies
class HybridVerifier {
public:
    explicit HybridVerifier(const Config& config);

    /// Verify using the best available strategy
    VerificationResult verify(const llvm::Instruction& original,
                              const SynthesizedSequence& candidate,
                              llvm::LLVMContext& ctx);

    /// Get which strategy was used
    std::string getStrategyUsed() const { return strategyUsed_; }

    /// Get combined statistics
    struct Stats {
        size_t algebraicProofs = 0;
        size_t randomTestingVerifications = 0;
        size_t cegisVerifications = 0;
        size_t smtVerifications = 0;
    };

    Stats getStats() const { return stats_; }

private:
    Config config_;
    AlgebraicVerifier algebraicVerifier_;
    Verifier randomVerifier_;
    CEGISVerifier cegisVerifier_;
    std::string strategyUsed_;
    Stats stats_;
};

/// Observational equivalence for pruning
class ObservationalEquivalence {
public:
    explicit ObservationalEquivalence(const Config& config);

    /// Compute fingerprint for a sequence
    uint64_t computeFingerprint(const SynthesizedSequence& seq,
                                 const std::vector<llvm::Type*>& inputTypes,
                                 llvm::LLVMContext& ctx);

    /// Check if sequence is a new equivalence class representative
    bool isNewEquivalenceClass(const SynthesizedSequence& seq,
                                const std::vector<llvm::Type*>& inputTypes,
                                llvm::LLVMContext& ctx);

    /// Get or generate test vectors for fingerprinting
    const std::vector<std::vector<uint64_t>>& getTestVectors(
        size_t numInputs, size_t bitWidth);

    /// Clear seen fingerprints
    void clear() { seenFingerprints_.clear(); }

    /// Get number of unique equivalence classes
    size_t getNumClasses() const { return seenFingerprints_.size(); }

private:
    Config config_;
    std::unordered_set<uint64_t> seenFingerprints_;
    std::map<std::pair<size_t, size_t>, std::vector<std::vector<uint64_t>>> testVectorCache_;
    std::mt19937_64 rng_;

    /// Evaluate sequence on test inputs
    std::vector<uint64_t> evaluate(const SynthesizedSequence& seq,
                                    const std::vector<std::vector<uint64_t>>& inputs);
};

#ifdef SUPEROPT_USE_Z3
/// SMT-based verifier using Z3
class SMTVerifier {
public:
    explicit SMTVerifier(const Config& config);
    ~SMTVerifier();

    /// Verify using SMT solving
    VerificationResult verify(const llvm::Instruction& original,
                              const SynthesizedSequence& candidate,
                              llvm::LLVMContext& ctx);

    /// Get solver statistics
    struct SolverStats {
        size_t queriesIssued = 0;
        size_t satResults = 0;
        size_t unsatResults = 0;
        size_t unknownResults = 0;
        double totalSolveTimeMs = 0.0;
    };

    SolverStats getStats() const { return stats_; }

private:
    Config config_;
    SolverStats stats_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace superopt

#endif // SUPEROPT_VERIFIER_H
