#include "superopt/Canonicalizer.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>

namespace superopt {

Canonicalizer::Canonicalizer() = default;

bool Canonicalizer::canonicalize(llvm::Function& func) {
    bool changed = false;

    // Multiple passes to reach fixed point
    bool localChanged;
    do {
        localChanged = false;

        for (auto& bb : func) {
            localChanged |= canonicalize(bb);
        }

        localChanged |= eliminateDeadCode(func);
        changed |= localChanged;
    } while (localChanged);

    return changed;
}

bool Canonicalizer::canonicalize(llvm::BasicBlock& bb) {
    bool changed = false;

    for (auto& inst : bb) {
        changed |= canonicalize(inst);
    }

    changed |= combineInstructions(bb);

    return changed;
}

bool Canonicalizer::canonicalize(llvm::Instruction& inst) {
    bool changed = false;

    changed |= canonicalizeOperandOrder(inst);

    if (auto* cmp = llvm::dyn_cast<llvm::CmpInst>(&inst)) {
        changed |= canonicalizeCompare(*cmp);
    }

    changed |= simplifyTrivial(inst);
    changed |= applyIdentities(inst);

    return changed;
}

bool Canonicalizer::canonicalizeOperandOrder(llvm::Instruction& inst) {
    // For commutative operations, order operands consistently
    // (e.g., constants on the right, lower-numbered values first)

    if (!inst.isCommutative()) {
        return false;
    }

    if (inst.getNumOperands() < 2) {
        return false;
    }

    llvm::Value* op0 = inst.getOperand(0);
    llvm::Value* op1 = inst.getOperand(1);

    bool shouldSwap = false;

    // Put constants on the right
    if (llvm::isa<llvm::Constant>(op0) && !llvm::isa<llvm::Constant>(op1)) {
        shouldSwap = true;
    }
    // For non-constants, use pointer comparison for consistent ordering
    else if (!llvm::isa<llvm::Constant>(op0) && !llvm::isa<llvm::Constant>(op1)) {
        if (op0 > op1) {
            shouldSwap = true;
        }
    }

    if (shouldSwap) {
        inst.setOperand(0, op1);
        inst.setOperand(1, op0);
        return true;
    }

    return false;
}

bool Canonicalizer::canonicalizeCompare(llvm::CmpInst& cmp) {
    // Normalize comparison predicates
    // e.g., prefer < over > (swap operands if needed)

    llvm::CmpInst::Predicate pred = cmp.getPredicate();
    bool shouldSwap = false;

    switch (pred) {
        case llvm::CmpInst::ICMP_SGT:
        case llvm::CmpInst::ICMP_UGT:
        case llvm::CmpInst::FCMP_OGT:
        case llvm::CmpInst::FCMP_UGT:
            shouldSwap = true;
            break;
        case llvm::CmpInst::ICMP_SGE:
        case llvm::CmpInst::ICMP_UGE:
        case llvm::CmpInst::FCMP_OGE:
        case llvm::CmpInst::FCMP_UGE:
            shouldSwap = true;
            break;
        default:
            break;
    }

    if (shouldSwap) {
        cmp.swapOperands();
        return true;
    }

    return false;
}

bool Canonicalizer::simplifyTrivial(llvm::Instruction& inst) {
    // Use LLVM's instruction simplification
    const llvm::DataLayout& DL = inst.getModule()->getDataLayout();
    llvm::SimplifyQuery SQ(DL);

    if (llvm::Value* simplified = llvm::simplifyInstruction(&inst, SQ)) {
        inst.replaceAllUsesWith(simplified);
        return true;
    }

    return false;
}

bool Canonicalizer::applyIdentities(llvm::Instruction& inst) {
    // Apply algebraic identities

    // x + 0 = x
    if (inst.getOpcode() == llvm::Instruction::Add) {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
            if (c->isZero()) {
                inst.replaceAllUsesWith(inst.getOperand(0));
                return true;
            }
        }
    }

    // x * 1 = x
    if (inst.getOpcode() == llvm::Instruction::Mul) {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
            if (c->isOne()) {
                inst.replaceAllUsesWith(inst.getOperand(0));
                return true;
            }
        }
    }

    // x * 0 = 0
    if (inst.getOpcode() == llvm::Instruction::Mul) {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
            if (c->isZero()) {
                inst.replaceAllUsesWith(c);
                return true;
            }
        }
    }

    // x & 0 = 0
    if (inst.getOpcode() == llvm::Instruction::And) {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
            if (c->isZero()) {
                inst.replaceAllUsesWith(c);
                return true;
            }
        }
    }

    // x | 0 = x
    if (inst.getOpcode() == llvm::Instruction::Or) {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
            if (c->isZero()) {
                inst.replaceAllUsesWith(inst.getOperand(0));
                return true;
            }
        }
    }

    // x ^ 0 = x
    if (inst.getOpcode() == llvm::Instruction::Xor) {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
            if (c->isZero()) {
                inst.replaceAllUsesWith(inst.getOperand(0));
                return true;
            }
        }
    }

    // x - x = 0
    if (inst.getOpcode() == llvm::Instruction::Sub) {
        if (inst.getOperand(0) == inst.getOperand(1)) {
            auto* zero = llvm::ConstantInt::get(inst.getType(), 0);
            inst.replaceAllUsesWith(zero);
            return true;
        }
    }

    // x ^ x = 0
    if (inst.getOpcode() == llvm::Instruction::Xor) {
        if (inst.getOperand(0) == inst.getOperand(1)) {
            auto* zero = llvm::ConstantInt::get(inst.getType(), 0);
            inst.replaceAllUsesWith(zero);
            return true;
        }
    }

    // x & x = x
    if (inst.getOpcode() == llvm::Instruction::And) {
        if (inst.getOperand(0) == inst.getOperand(1)) {
            inst.replaceAllUsesWith(inst.getOperand(0));
            return true;
        }
    }

    // x | x = x
    if (inst.getOpcode() == llvm::Instruction::Or) {
        if (inst.getOperand(0) == inst.getOperand(1)) {
            inst.replaceAllUsesWith(inst.getOperand(0));
            return true;
        }
    }

    return false;
}

bool Canonicalizer::eliminateDeadCode(llvm::Function& func) {
    bool changed = false;

    // Remove dead instructions
    std::vector<llvm::Instruction*> toRemove;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.use_empty() && !inst.isTerminator() &&
                !inst.mayHaveSideEffects()) {
                toRemove.push_back(&inst);
            }
        }
    }

    for (auto* inst : toRemove) {
        inst->eraseFromParent();
        changed = true;
    }

    return changed;
}

bool Canonicalizer::combineInstructions(llvm::BasicBlock& bb) {
    // Simple instruction combining patterns
    bool changed = false;

    // Placeholder for more complex combining
    // Full implementation would include:
    // - (x << n) >> n = x & mask
    // - add(add(x, c1), c2) = add(x, c1+c2)
    // etc.

    return changed;
}

std::unique_ptr<llvm::Module> Canonicalizer::canonicalCopy(
    const llvm::Function& func,
    llvm::LLVMContext& ctx) {

    // Clone the function's module
    auto module = llvm::CloneModule(*func.getParent());

    // Find and canonicalize the function
    if (auto* clonedFunc = module->getFunction(func.getName())) {
        canonicalize(*clonedFunc);
    }

    return module;
}

bool Canonicalizer::structurallyEquivalent(const llvm::Instruction& a,
                                            const llvm::Instruction& b) {
    if (a.getOpcode() != b.getOpcode()) return false;
    if (a.getType() != b.getType()) return false;
    if (a.getNumOperands() != b.getNumOperands()) return false;

    for (unsigned i = 0; i < a.getNumOperands(); ++i) {
        if (a.getOperand(i)->getType() != b.getOperand(i)->getType()) {
            return false;
        }
    }

    // For comparisons, check predicate
    if (auto* cmpA = llvm::dyn_cast<llvm::CmpInst>(&a)) {
        auto* cmpB = llvm::dyn_cast<llvm::CmpInst>(&b);
        if (!cmpB || cmpA->getPredicate() != cmpB->getPredicate()) {
            return false;
        }
    }

    return true;
}

uint64_t Canonicalizer::structuralHash(const llvm::Instruction& inst) {
    uint64_t hash = inst.getOpcode();
    hash ^= std::hash<llvm::Type*>{}(inst.getType()) * 31;

    for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
        hash ^= std::hash<llvm::Type*>{}(inst.getOperand(i)->getType()) * (i + 37);
    }

    if (auto* cmp = llvm::dyn_cast<llvm::CmpInst>(&inst)) {
        hash ^= static_cast<uint64_t>(cmp->getPredicate()) * 97;
    }

    return hash;
}

uint64_t Canonicalizer::structuralHash(const llvm::BasicBlock& bb) {
    uint64_t hash = 0;
    size_t pos = 0;

    for (const auto& inst : bb) {
        hash ^= structuralHash(inst) << (pos % 32);
        pos++;
    }

    return hash;
}

} // namespace superopt
