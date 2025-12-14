#include "superopt/Pruning.h"
#include "superopt/Canonicalizer.h"

namespace superopt {

// PruningEngine implementation

PruningEngine::PruningEngine(const Config& config, CostModel& costModel)
    : config_(config), costModel_(costModel) {}

bool PruningEngine::shouldPrune(const SynthesizedSequence& partial,
                                 double targetCost) {
    candidatesPruned_++;

    // Cost-based pruning
    if (pruneByCost(partial, targetCost)) {
        costPruned_++;
        return true;
    }

    // Pattern-based pruning
    if (pruneByPattern(partial)) {
        candidatesPruned_++;
        return true;
    }

    // Dead value pruning
    if (pruneDeadValues(partial)) {
        candidatesPruned_++;
        return true;
    }

    // Redundancy pruning
    if (isRedundant(partial)) {
        redundantPruned_++;
        return true;
    }

    return false;
}

bool PruningEngine::isRedundant(const SynthesizedSequence& candidate) {
    uint64_t hash = computeHash(candidate);
    return seenHashes_.find(hash) != seenHashes_.end();
}

void PruningEngine::markSeen(const SynthesizedSequence& candidate) {
    uint64_t hash = computeHash(candidate);
    seenHashes_.insert(hash);
}

void PruningEngine::clearSeen() {
    seenHashes_.clear();
}

void PruningEngine::resetStats() {
    candidatesPruned_ = 0;
    redundantPruned_ = 0;
    costPruned_ = 0;
}

uint64_t PruningEngine::computeHash(const SynthesizedSequence& seq) {
    uint64_t hash = seq.templates.size();

    for (size_t i = 0; i < seq.templates.size(); ++i) {
        const auto& templ = seq.templates[i];
        hash ^= static_cast<uint64_t>(templ.opcode) << (i % 32);

        for (size_t j = 0; j < templ.operandIndices.size(); ++j) {
            hash ^= static_cast<uint64_t>(templ.operandIndices[j] + 1000) *
                    (j + 1) * 37;
        }

        hash ^= static_cast<uint64_t>(templ.predicate) * 97;
    }

    hash ^= static_cast<uint64_t>(seq.outputIndex + 1000) * 113;

    return hash;
}

bool PruningEngine::pruneByCost(const SynthesizedSequence& partial,
                                 double targetCost) {
    // Compute lower bound on cost of partial sequence
    double lowerBound = 0.0;

    for (const auto& templ : partial.templates) {
        // Use default costs from cost model
        switch (templ.opcode) {
            case llvm::Instruction::Add:
            case llvm::Instruction::Sub:
            case llvm::Instruction::And:
            case llvm::Instruction::Or:
            case llvm::Instruction::Xor:
            case llvm::Instruction::Shl:
            case llvm::Instruction::LShr:
            case llvm::Instruction::AShr:
            case llvm::Instruction::ICmp:
                lowerBound += 1.0;
                break;
            case llvm::Instruction::Mul:
                lowerBound += 3.0;
                break;
            case llvm::Instruction::UDiv:
            case llvm::Instruction::SDiv:
            case llvm::Instruction::URem:
            case llvm::Instruction::SRem:
                lowerBound += 20.0;
                break;
            default:
                lowerBound += 1.0;
        }
    }

    // If we're already more expensive than target, prune
    return lowerBound > targetCost;
}

bool PruningEngine::pruneByPattern(const SynthesizedSequence& partial) {
    if (partial.templates.empty()) return false;

    // Prune obviously redundant patterns

    // Pattern: x op x where op is not useful
    // (x + x = 2x is fine, but x - x = 0 should be folded)

    for (size_t i = 0; i < partial.templates.size(); ++i) {
        const auto& templ = partial.templates[i];

        if (templ.operandIndices.size() >= 2 &&
            templ.operandIndices[0] == templ.operandIndices[1]) {

            // x - x, x xor x should be constant-folded
            if (templ.opcode == llvm::Instruction::Sub ||
                templ.opcode == llvm::Instruction::Xor) {
                return true;
            }

            // x / x, x % x - these should be 1 or 0
            if (templ.opcode == llvm::Instruction::UDiv ||
                templ.opcode == llvm::Instruction::SDiv ||
                templ.opcode == llvm::Instruction::URem ||
                templ.opcode == llvm::Instruction::SRem) {
                return true;
            }
        }
    }

    // Prune consecutive inverse operations
    // e.g., (x + c) - c, (x << n) >> n (when not useful)
    for (size_t i = 1; i < partial.templates.size(); ++i) {
        const auto& prev = partial.templates[i - 1];
        const auto& curr = partial.templates[i];

        // Check if current uses previous result as first operand
        int prevResultIdx = static_cast<int>(i - 1 +
            partial.templates.size() - partial.templates.size());  // Simplified

        // Add + Sub with same operand (if second operand is the same)
        if (prev.opcode == llvm::Instruction::Add &&
            curr.opcode == llvm::Instruction::Sub) {
            if (prev.operandIndices.size() >= 2 &&
                curr.operandIndices.size() >= 2 &&
                prev.operandIndices[1] == curr.operandIndices[1]) {
                return true;  // This cancels out
            }
        }
    }

    return false;
}

bool PruningEngine::pruneDeadValues(const SynthesizedSequence& partial) {
    if (partial.templates.empty()) return false;
    if (partial.outputIndex < 0) return false;

    // Find which values are actually used
    std::vector<bool> used(partial.templates.size() + 10, false);

    // Output value is used
    if (static_cast<size_t>(partial.outputIndex) < used.size()) {
        used[partial.outputIndex] = true;
    }

    // Propagate backwards
    for (int i = static_cast<int>(partial.templates.size()) - 1; i >= 0; --i) {
        size_t resultIdx = i;  // Result of instruction i

        if (resultIdx < used.size() && used[resultIdx]) {
            // Mark operands as used
            for (int opIdx : partial.templates[i].operandIndices) {
                if (opIdx >= 0 && static_cast<size_t>(opIdx) < used.size()) {
                    used[opIdx] = true;
                }
            }
        }
    }

    // If any instruction result is not used, we could prune
    // (but we've already generated it, so this is for future optimization)
    return false;
}

// ObservationalPruning implementation

ObservationalPruning::ObservationalPruning(const Config& config)
    : config_(config), rng_(std::random_device{}()),
      testInputCache_([](std::pair<size_t, size_t> p) {
          return std::hash<size_t>{}(p.first) ^
                 (std::hash<size_t>{}(p.second) << 1);
      }) {}

std::vector<uint64_t> ObservationalPruning::computeSignature(
    const SynthesizedSequence& seq,
    const std::vector<std::vector<uint64_t>>& testInputs,
    llvm::LLVMContext& ctx) {

    std::vector<uint64_t> signature;
    signature.reserve(testInputs.size());

    // For each test input, compute the output
    for (const auto& inputs : testInputs) {
        // Simple interpreter for the sequence
        std::vector<uint64_t> values = inputs;

        for (const auto& templ : seq.templates) {
            uint64_t result = 0;

            // Get operands
            std::vector<uint64_t> ops;
            for (int idx : templ.operandIndices) {
                if (idx >= 0 && static_cast<size_t>(idx) < values.size()) {
                    ops.push_back(values[idx]);
                }
            }

            if (ops.size() < 2) {
                values.push_back(0);
                continue;
            }

            // Compute result
            switch (templ.opcode) {
                case llvm::Instruction::Add:
                    result = ops[0] + ops[1];
                    break;
                case llvm::Instruction::Sub:
                    result = ops[0] - ops[1];
                    break;
                case llvm::Instruction::Mul:
                    result = ops[0] * ops[1];
                    break;
                case llvm::Instruction::UDiv:
                    result = ops[1] != 0 ? ops[0] / ops[1] : 0;
                    break;
                case llvm::Instruction::SDiv:
                    result = ops[1] != 0 ?
                        static_cast<uint64_t>(
                            static_cast<int64_t>(ops[0]) /
                            static_cast<int64_t>(ops[1])) : 0;
                    break;
                case llvm::Instruction::And:
                    result = ops[0] & ops[1];
                    break;
                case llvm::Instruction::Or:
                    result = ops[0] | ops[1];
                    break;
                case llvm::Instruction::Xor:
                    result = ops[0] ^ ops[1];
                    break;
                case llvm::Instruction::Shl:
                    result = ops[0] << (ops[1] & 63);
                    break;
                case llvm::Instruction::LShr:
                    result = ops[0] >> (ops[1] & 63);
                    break;
                case llvm::Instruction::AShr:
                    result = static_cast<uint64_t>(
                        static_cast<int64_t>(ops[0]) >> (ops[1] & 63));
                    break;
                case llvm::Instruction::ICmp: {
                    bool cmpResult = false;
                    switch (templ.predicate) {
                        case llvm::CmpInst::ICMP_EQ:
                            cmpResult = ops[0] == ops[1]; break;
                        case llvm::CmpInst::ICMP_NE:
                            cmpResult = ops[0] != ops[1]; break;
                        case llvm::CmpInst::ICMP_ULT:
                            cmpResult = ops[0] < ops[1]; break;
                        case llvm::CmpInst::ICMP_ULE:
                            cmpResult = ops[0] <= ops[1]; break;
                        case llvm::CmpInst::ICMP_UGT:
                            cmpResult = ops[0] > ops[1]; break;
                        case llvm::CmpInst::ICMP_UGE:
                            cmpResult = ops[0] >= ops[1]; break;
                        case llvm::CmpInst::ICMP_SLT:
                            cmpResult = static_cast<int64_t>(ops[0]) <
                                       static_cast<int64_t>(ops[1]); break;
                        case llvm::CmpInst::ICMP_SLE:
                            cmpResult = static_cast<int64_t>(ops[0]) <=
                                       static_cast<int64_t>(ops[1]); break;
                        case llvm::CmpInst::ICMP_SGT:
                            cmpResult = static_cast<int64_t>(ops[0]) >
                                       static_cast<int64_t>(ops[1]); break;
                        case llvm::CmpInst::ICMP_SGE:
                            cmpResult = static_cast<int64_t>(ops[0]) >=
                                       static_cast<int64_t>(ops[1]); break;
                        default:
                            break;
                    }
                    result = cmpResult ? 1 : 0;
                    break;
                }
                default:
                    break;
            }

            values.push_back(result);
        }

        // Get output value
        uint64_t output = 0;
        if (seq.outputIndex >= 0 &&
            static_cast<size_t>(seq.outputIndex) < values.size()) {
            output = values[seq.outputIndex];
        }

        signature.push_back(output);
    }

    return signature;
}

bool ObservationalPruning::sameSignature(const SynthesizedSequence& a,
                                          const SynthesizedSequence& b,
                                          llvm::LLVMContext& ctx) {
    // Generate test inputs (assuming 64-bit integers, 2 inputs)
    auto& inputs = getTestInputs(2, 64);

    auto sigA = computeSignature(a, inputs, ctx);
    auto sigB = computeSignature(b, inputs, ctx);

    return sigA == sigB;
}

const std::vector<std::vector<uint64_t>>& ObservationalPruning::getTestInputs(
    size_t numInputs, size_t bitWidth) {

    auto key = std::make_pair(numInputs, bitWidth);
    auto it = testInputCache_.find(key);

    if (it != testInputCache_.end()) {
        return it->second;
    }

    // Generate test inputs
    std::vector<std::vector<uint64_t>> inputs;

    // Add edge cases
    std::vector<uint64_t> edgeCases = {
        0, 1, 2, 3,
        static_cast<uint64_t>(-1),
        static_cast<uint64_t>(-2),
        (1ULL << (bitWidth - 1)) - 1,  // Max signed
        1ULL << (bitWidth - 1),         // Min signed
        (1ULL << bitWidth) - 1,         // Max unsigned (for 64-bit, same as -1)
    };

    // All combinations of edge cases
    for (uint64_t a : edgeCases) {
        for (uint64_t b : edgeCases) {
            std::vector<uint64_t> input;
            input.push_back(a);
            if (numInputs > 1) input.push_back(b);
            for (size_t i = 2; i < numInputs; ++i) {
                input.push_back(0);
            }
            inputs.push_back(input);
        }
    }

    // Add some random inputs
    std::uniform_int_distribution<uint64_t> dist;
    for (size_t i = 0; i < 20; ++i) {
        std::vector<uint64_t> input;
        for (size_t j = 0; j < numInputs; ++j) {
            input.push_back(dist(rng_));
        }
        inputs.push_back(input);
    }

    auto [insertIt, inserted] = testInputCache_.emplace(key, std::move(inputs));
    return insertIt->second;
}

// TypePruning implementation

bool TypePruning::isValidForTypes(unsigned opcode,
                                   const std::vector<llvm::Type*>& inputTypes) {
    if (inputTypes.empty()) return false;

    switch (opcode) {
        case llvm::Instruction::Add:
        case llvm::Instruction::Sub:
        case llvm::Instruction::Mul:
        case llvm::Instruction::UDiv:
        case llvm::Instruction::SDiv:
        case llvm::Instruction::URem:
        case llvm::Instruction::SRem:
        case llvm::Instruction::And:
        case llvm::Instruction::Or:
        case llvm::Instruction::Xor:
        case llvm::Instruction::Shl:
        case llvm::Instruction::LShr:
        case llvm::Instruction::AShr:
            return inputTypes[0]->isIntegerTy();

        case llvm::Instruction::FAdd:
        case llvm::Instruction::FSub:
        case llvm::Instruction::FMul:
        case llvm::Instruction::FDiv:
        case llvm::Instruction::FRem:
            return inputTypes[0]->isFloatingPointTy();

        case llvm::Instruction::ICmp:
            return inputTypes[0]->isIntegerTy() ||
                   inputTypes[0]->isPointerTy();

        case llvm::Instruction::FCmp:
            return inputTypes[0]->isFloatingPointTy();

        default:
            return true;
    }
}

std::vector<unsigned> TypePruning::getValidOpcodes(llvm::Type* resultType) {
    std::vector<unsigned> opcodes;

    if (resultType->isIntegerTy()) {
        opcodes = {
            llvm::Instruction::Add,
            llvm::Instruction::Sub,
            llvm::Instruction::Mul,
            llvm::Instruction::UDiv,
            llvm::Instruction::SDiv,
            llvm::Instruction::URem,
            llvm::Instruction::SRem,
            llvm::Instruction::And,
            llvm::Instruction::Or,
            llvm::Instruction::Xor,
            llvm::Instruction::Shl,
            llvm::Instruction::LShr,
            llvm::Instruction::AShr,
        };

        // i1 can also come from comparisons
        if (resultType->isIntegerTy(1)) {
            opcodes.push_back(llvm::Instruction::ICmp);
        }
    } else if (resultType->isFloatingPointTy()) {
        opcodes = {
            llvm::Instruction::FAdd,
            llvm::Instruction::FSub,
            llvm::Instruction::FMul,
            llvm::Instruction::FDiv,
            llvm::Instruction::FRem,
        };
    }

    return opcodes;
}

bool TypePruning::canProduceType(const SynthesizedSequence& partial,
                                  llvm::Type* targetType) {
    // Check if any instruction in the sequence can produce the target type
    // This is a simplified check

    for (const auto& templ : partial.templates) {
        if (templ.resultType == targetType) {
            return true;
        }
    }

    return false;
}

} // namespace superopt
