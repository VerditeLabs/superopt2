#ifndef SUPEROPT_PRUNING_H
#define SUPEROPT_PRUNING_H

#include "superopt/Common.h"
#include "superopt/Enumerator.h"
#include "superopt/CostModel.h"
#include "llvm/IR/Instructions.h"

#include <unordered_set>
#include <unordered_map>

namespace superopt {

/// Prunes the search space during enumeration
class PruningEngine {
public:
    explicit PruningEngine(const Config& config, CostModel& costModel);

    /// Check if a partial sequence should be pruned
    bool shouldPrune(const SynthesizedSequence& partial,
                     double targetCost);

    /// Check if a candidate is redundant (structurally equivalent to seen)
    bool isRedundant(const SynthesizedSequence& candidate);

    /// Add a candidate to the seen set
    void markSeen(const SynthesizedSequence& candidate);

    /// Clear the seen set
    void clearSeen();

    /// Get pruning statistics
    size_t getCandidatesPruned() const { return candidatesPruned_; }
    size_t getRedundantPruned() const { return redundantPruned_; }
    size_t getCostPruned() const { return costPruned_; }

    /// Reset statistics
    void resetStats();

private:
    Config config_;
    CostModel& costModel_;

    // Statistics
    size_t candidatesPruned_ = 0;
    size_t redundantPruned_ = 0;
    size_t costPruned_ = 0;

    // Seen candidates (by structural hash)
    std::unordered_set<uint64_t> seenHashes_;

    /// Compute structural hash of a sequence
    uint64_t computeHash(const SynthesizedSequence& seq);

    /// Cost-based pruning rules
    bool pruneByCost(const SynthesizedSequence& partial, double targetCost);

    /// Pattern-based pruning rules
    bool pruneByPattern(const SynthesizedSequence& partial);

    /// Dead value pruning
    bool pruneDeadValues(const SynthesizedSequence& partial);
};

/// Observational equivalence pruning
class ObservationalPruning {
public:
    explicit ObservationalPruning(const Config& config);

    /// Compute observational signature for a sequence
    /// (output values for a set of test inputs)
    std::vector<uint64_t> computeSignature(
        const SynthesizedSequence& seq,
        const std::vector<std::vector<uint64_t>>& testInputs,
        llvm::LLVMContext& ctx);

    /// Check if two sequences have the same observational signature
    bool sameSignature(const SynthesizedSequence& a,
                       const SynthesizedSequence& b,
                       llvm::LLVMContext& ctx);

    /// Get/generate test inputs
    const std::vector<std::vector<uint64_t>>& getTestInputs(
        size_t numInputs, size_t bitWidth);

private:
    Config config_;
    std::unordered_map<std::pair<size_t, size_t>,
                       std::vector<std::vector<uint64_t>>,
                       std::function<size_t(std::pair<size_t, size_t>)>> testInputCache_;
    std::mt19937_64 rng_;
};

/// Type-based pruning
class TypePruning {
public:
    /// Check if an opcode is valid for given input types
    static bool isValidForTypes(unsigned opcode,
                                const std::vector<llvm::Type*>& inputTypes);

    /// Get valid opcodes for a result type
    static std::vector<unsigned> getValidOpcodes(llvm::Type* resultType);

    /// Check if a sequence can produce the required output type
    static bool canProduceType(const SynthesizedSequence& partial,
                               llvm::Type* targetType);
};

} // namespace superopt

#endif // SUPEROPT_PRUNING_H
