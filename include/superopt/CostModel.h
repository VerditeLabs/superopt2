#ifndef SUPEROPT_COSTMODEL_H
#define SUPEROPT_COSTMODEL_H

#include "superopt/Common.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/TargetTransformInfo.h"

#include <unordered_map>

namespace superopt {

/// Cost model for evaluating instruction sequences
class CostModel {
public:
    /// Cost breakdown for an instruction sequence
    struct CostBreakdown {
        double latency = 0.0;       // Total latency (critical path)
        double throughput = 0.0;    // Reciprocal throughput
        double codeSize = 0.0;      // Code size in bytes
        size_t instructionCount = 0;
        size_t memoryOps = 0;
        size_t branchOps = 0;
        size_t divisionOps = 0;

        /// Combined cost (weighted sum)
        double totalCost(double latencyWeight = 1.0,
                        double throughputWeight = 0.5,
                        double sizeWeight = 0.1) const {
            return latency * latencyWeight +
                   throughput * throughputWeight +
                   codeSize * sizeWeight;
        }
    };

    CostModel();

    /// Set target-specific cost information (optional)
    void setTargetTransformInfo(llvm::TargetTransformInfo* tti);

    /// Compute cost for a single instruction
    double getInstructionCost(const llvm::Instruction& inst);

    /// Compute cost breakdown for a single instruction
    CostBreakdown getInstructionCostBreakdown(const llvm::Instruction& inst);

    /// Compute total cost for a basic block
    double getBasicBlockCost(const llvm::BasicBlock& bb);

    /// Compute cost breakdown for a basic block
    CostBreakdown getBasicBlockCostBreakdown(const llvm::BasicBlock& bb);

    /// Compute total cost for a function
    double getFunctionCost(const llvm::Function& func);

    /// Compute cost breakdown for a function
    CostBreakdown getFunctionCostBreakdown(const llvm::Function& func);

    /// Compute cost for a sequence of instructions
    double getSequenceCost(const std::vector<llvm::Instruction*>& seq);

    /// Check if sequence B is cheaper than sequence A
    bool isCheaper(const std::vector<llvm::Instruction*>& seqA,
                   const std::vector<llvm::Instruction*>& seqB);

    /// Get cost improvement ratio (original - optimized) / original
    double getImprovementRatio(double originalCost, double optimizedCost);

    /// Get default cost for an opcode
    double getDefaultCost(unsigned opcode);

private:
    llvm::TargetTransformInfo* tti_ = nullptr;

    /// Default instruction costs (used when no TTI available)
    std::unordered_map<unsigned, double> defaultCosts_;

    /// Initialize default costs
    void initDefaultCosts();
};

} // namespace superopt

#endif // SUPEROPT_COSTMODEL_H
