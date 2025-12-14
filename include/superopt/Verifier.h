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
};

/// Counterexample found during verification
struct Counterexample {
    TestInput input;
    llvm::GenericValue expectedOutput;
    llvm::GenericValue actualOutput;
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

    /// Execute a function with given inputs using interpretation
    std::optional<llvm::GenericValue> execute(
        llvm::Function& func,
        const TestInput& input);

private:
    Config config_;
    std::optional<Counterexample> counterexample_;
    std::string lastError_;
    std::mt19937_64 rng_;

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
};

/// Symbolic verifier using abstract interpretation (no SMT dependency)
class SymbolicVerifier {
public:
    explicit SymbolicVerifier(const Config& config);

    /// Verify using abstract interpretation
    VerificationResult verify(const llvm::Function& original,
                             const llvm::Function& candidate);

    /// Perform algebraic simplification check
    bool checkAlgebraicEquivalence(const llvm::Instruction& original,
                                   const SynthesizedSequence& candidate);

private:
    Config config_;

    /// Compute abstract value range for an instruction
    std::pair<int64_t, int64_t> computeRange(const llvm::Instruction& inst);
};

} // namespace superopt

#endif // SUPEROPT_VERIFIER_H
