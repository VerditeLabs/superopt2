#ifndef SUPEROPT_CANONICALIZER_H
#define SUPEROPT_CANONICALIZER_H

#include "superopt/Common.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Local.h"

namespace superopt {

/// Canonicalizes IR to a standard form for comparison and optimization
class Canonicalizer {
public:
    Canonicalizer();

    /// Canonicalize a function in place
    bool canonicalize(llvm::Function& func);

    /// Canonicalize a basic block in place
    bool canonicalize(llvm::BasicBlock& bb);

    /// Canonicalize a single instruction
    bool canonicalize(llvm::Instruction& inst);

    /// Create a canonical copy of a function
    std::unique_ptr<llvm::Module> canonicalCopy(const llvm::Function& func,
                                                 llvm::LLVMContext& ctx);

    /// Check if two instructions are structurally equivalent
    static bool structurallyEquivalent(const llvm::Instruction& a,
                                       const llvm::Instruction& b);

    /// Compute a structural hash of an instruction
    static uint64_t structuralHash(const llvm::Instruction& inst);

    /// Compute a structural hash of a basic block
    static uint64_t structuralHash(const llvm::BasicBlock& bb);

private:
    /// Reorder commutative operands consistently
    bool canonicalizeOperandOrder(llvm::Instruction& inst);

    /// Normalize comparison predicates
    bool canonicalizeCompare(llvm::CmpInst& cmp);

    /// Simplify trivial patterns
    bool simplifyTrivial(llvm::Instruction& inst);

    /// Apply algebraic identities
    bool applyIdentities(llvm::Instruction& inst);

    /// Dead code elimination
    bool eliminateDeadCode(llvm::Function& func);

    /// Combine instructions where possible
    bool combineInstructions(llvm::BasicBlock& bb);
};

/// Hash functor for instruction sequences (for memoization)
struct SequenceHash {
    size_t operator()(const std::vector<uint64_t>& seq) const {
        size_t hash = 0;
        for (auto h : seq) {
            hash ^= h + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

} // namespace superopt

#endif // SUPEROPT_CANONICALIZER_H
