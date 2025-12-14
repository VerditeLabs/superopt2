#include "superopt/CostModel.h"

namespace superopt {

CostModel::CostModel() {
    initDefaultCosts();
}

void CostModel::initDefaultCosts() {
    // Default instruction costs (latency-like, higher = more expensive)
    // These are rough estimates; real costs vary by target

    // Arithmetic
    defaultCosts_[llvm::Instruction::Add] = 1.0;
    defaultCosts_[llvm::Instruction::Sub] = 1.0;
    defaultCosts_[llvm::Instruction::Mul] = 3.0;
    defaultCosts_[llvm::Instruction::UDiv] = 20.0;
    defaultCosts_[llvm::Instruction::SDiv] = 20.0;
    defaultCosts_[llvm::Instruction::URem] = 20.0;
    defaultCosts_[llvm::Instruction::SRem] = 20.0;
    defaultCosts_[llvm::Instruction::FAdd] = 3.0;
    defaultCosts_[llvm::Instruction::FSub] = 3.0;
    defaultCosts_[llvm::Instruction::FMul] = 5.0;
    defaultCosts_[llvm::Instruction::FDiv] = 15.0;
    defaultCosts_[llvm::Instruction::FRem] = 20.0;
    defaultCosts_[llvm::Instruction::FNeg] = 1.0;

    // Logical
    defaultCosts_[llvm::Instruction::And] = 1.0;
    defaultCosts_[llvm::Instruction::Or] = 1.0;
    defaultCosts_[llvm::Instruction::Xor] = 1.0;

    // Shifts
    defaultCosts_[llvm::Instruction::Shl] = 1.0;
    defaultCosts_[llvm::Instruction::LShr] = 1.0;
    defaultCosts_[llvm::Instruction::AShr] = 1.0;

    // Comparisons
    defaultCosts_[llvm::Instruction::ICmp] = 1.0;
    defaultCosts_[llvm::Instruction::FCmp] = 2.0;

    // Select
    defaultCosts_[llvm::Instruction::Select] = 1.0;

    // Casts
    defaultCosts_[llvm::Instruction::Trunc] = 0.5;
    defaultCosts_[llvm::Instruction::ZExt] = 0.5;
    defaultCosts_[llvm::Instruction::SExt] = 0.5;
    defaultCosts_[llvm::Instruction::FPToUI] = 3.0;
    defaultCosts_[llvm::Instruction::FPToSI] = 3.0;
    defaultCosts_[llvm::Instruction::UIToFP] = 3.0;
    defaultCosts_[llvm::Instruction::SIToFP] = 3.0;
    defaultCosts_[llvm::Instruction::FPTrunc] = 2.0;
    defaultCosts_[llvm::Instruction::FPExt] = 2.0;
    defaultCosts_[llvm::Instruction::PtrToInt] = 0.5;
    defaultCosts_[llvm::Instruction::IntToPtr] = 0.5;
    defaultCosts_[llvm::Instruction::BitCast] = 0.0;

    // Memory
    defaultCosts_[llvm::Instruction::Load] = 4.0;
    defaultCosts_[llvm::Instruction::Store] = 4.0;
    defaultCosts_[llvm::Instruction::GetElementPtr] = 0.5;
    defaultCosts_[llvm::Instruction::Alloca] = 1.0;

    // Control flow
    defaultCosts_[llvm::Instruction::Br] = 1.0;
    defaultCosts_[llvm::Instruction::Switch] = 2.0;
    defaultCosts_[llvm::Instruction::Ret] = 1.0;
    defaultCosts_[llvm::Instruction::Call] = 5.0;
    defaultCosts_[llvm::Instruction::Invoke] = 5.0;

    // Misc
    defaultCosts_[llvm::Instruction::PHI] = 0.0;
    defaultCosts_[llvm::Instruction::ExtractElement] = 1.0;
    defaultCosts_[llvm::Instruction::InsertElement] = 1.0;
    defaultCosts_[llvm::Instruction::ShuffleVector] = 2.0;
    defaultCosts_[llvm::Instruction::ExtractValue] = 0.5;
    defaultCosts_[llvm::Instruction::InsertValue] = 0.5;
}

void CostModel::setTargetTransformInfo(llvm::TargetTransformInfo* tti) {
    tti_ = tti;
}

double CostModel::getDefaultCost(unsigned opcode) {
    auto it = defaultCosts_.find(opcode);
    if (it != defaultCosts_.end()) {
        return it->second;
    }
    return 1.0;  // Default cost for unknown instructions
}

double CostModel::getInstructionCost(const llvm::Instruction& inst) {
    if (tti_) {
        // Use LLVM's target-specific cost model
        auto cost = tti_->getInstructionCost(
            &inst, llvm::TargetTransformInfo::TCK_Latency);
        if (cost.isValid()) {
            return static_cast<double>(*cost.getValue());
        }
    }

    return getDefaultCost(inst.getOpcode());
}

CostModel::CostBreakdown CostModel::getInstructionCostBreakdown(
    const llvm::Instruction& inst) {
    CostBreakdown breakdown;

    breakdown.instructionCount = 1;
    breakdown.latency = getInstructionCost(inst);
    breakdown.throughput = breakdown.latency;  // Simplified

    // Count memory operations
    if (inst.mayReadOrWriteMemory()) {
        breakdown.memoryOps = 1;
    }

    // Count branches
    if (llvm::isa<llvm::BranchInst>(inst) ||
        llvm::isa<llvm::SwitchInst>(inst)) {
        breakdown.branchOps = 1;
    }

    // Count divisions (expensive)
    if (inst.getOpcode() == llvm::Instruction::UDiv ||
        inst.getOpcode() == llvm::Instruction::SDiv ||
        inst.getOpcode() == llvm::Instruction::URem ||
        inst.getOpcode() == llvm::Instruction::SRem ||
        inst.getOpcode() == llvm::Instruction::FDiv ||
        inst.getOpcode() == llvm::Instruction::FRem) {
        breakdown.divisionOps = 1;
    }

    // Estimate code size (very rough)
    breakdown.codeSize = 4.0;  // Assume 4 bytes per instruction

    return breakdown;
}

double CostModel::getBasicBlockCost(const llvm::BasicBlock& bb) {
    double cost = 0.0;
    for (const auto& inst : bb) {
        cost += getInstructionCost(inst);
    }
    return cost;
}

CostModel::CostBreakdown CostModel::getBasicBlockCostBreakdown(
    const llvm::BasicBlock& bb) {
    CostBreakdown breakdown;

    for (const auto& inst : bb) {
        auto instBreakdown = getInstructionCostBreakdown(inst);
        breakdown.latency += instBreakdown.latency;
        breakdown.throughput += instBreakdown.throughput;
        breakdown.codeSize += instBreakdown.codeSize;
        breakdown.instructionCount += instBreakdown.instructionCount;
        breakdown.memoryOps += instBreakdown.memoryOps;
        breakdown.branchOps += instBreakdown.branchOps;
        breakdown.divisionOps += instBreakdown.divisionOps;
    }

    return breakdown;
}

double CostModel::getFunctionCost(const llvm::Function& func) {
    double cost = 0.0;
    for (const auto& bb : func) {
        cost += getBasicBlockCost(bb);
    }
    return cost;
}

CostModel::CostBreakdown CostModel::getFunctionCostBreakdown(
    const llvm::Function& func) {
    CostBreakdown breakdown;

    for (const auto& bb : func) {
        auto bbBreakdown = getBasicBlockCostBreakdown(bb);
        breakdown.latency += bbBreakdown.latency;
        breakdown.throughput += bbBreakdown.throughput;
        breakdown.codeSize += bbBreakdown.codeSize;
        breakdown.instructionCount += bbBreakdown.instructionCount;
        breakdown.memoryOps += bbBreakdown.memoryOps;
        breakdown.branchOps += bbBreakdown.branchOps;
        breakdown.divisionOps += bbBreakdown.divisionOps;
    }

    return breakdown;
}

double CostModel::getSequenceCost(const std::vector<llvm::Instruction*>& seq) {
    double cost = 0.0;
    for (const auto* inst : seq) {
        cost += getInstructionCost(*inst);
    }
    return cost;
}

bool CostModel::isCheaper(const std::vector<llvm::Instruction*>& seqA,
                          const std::vector<llvm::Instruction*>& seqB) {
    return getSequenceCost(seqB) < getSequenceCost(seqA);
}

double CostModel::getImprovementRatio(double originalCost, double optimizedCost) {
    if (originalCost <= 0.0) return 0.0;
    return (originalCost - optimizedCost) / originalCost;
}

} // namespace superopt
