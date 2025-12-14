#include "superopt/Enumerator.h"
#include "llvm/IR/Constants.h"

namespace superopt {

// SequenceBuilder implementation

SequenceBuilder::SequenceBuilder(llvm::LLVMContext& ctx) : context_(ctx) {}

size_t SequenceBuilder::addInput(llvm::Type* type) {
    size_t idx = types_.size();
    types_.push_back(type);
    numInputs_++;
    return idx;
}

size_t SequenceBuilder::addConstant(llvm::Constant* c) {
    size_t idx = types_.size();
    types_.push_back(c->getType());
    return idx;
}

size_t SequenceBuilder::addInstruction(unsigned opcode,
                                        const std::vector<size_t>& operandIndices,
                                        llvm::Type* resultType) {
    InstructionTemplate templ;
    templ.opcode = opcode;
    for (auto idx : operandIndices) {
        templ.operandIndices.push_back(static_cast<int>(idx));
    }
    templ.resultType = resultType;

    sequence_.templates.push_back(templ);
    size_t idx = types_.size();
    types_.push_back(resultType ? resultType : types_[operandIndices[0]]);
    return idx;
}

llvm::Type* SequenceBuilder::getType(size_t index) const {
    if (index >= types_.size()) return nullptr;
    return types_[index];
}

void SequenceBuilder::reset() {
    sequence_ = SynthesizedSequence();
    types_.clear();
    numInputs_ = 0;
}

// SynthesizedSequence implementation

llvm::Value* SynthesizedSequence::apply(llvm::IRBuilder<>& builder,
                                         const std::vector<llvm::Value*>& inputs) const {
    std::vector<llvm::Value*> values = inputs;

    for (const auto& templ : templates) {
        std::vector<llvm::Value*> operands;
        for (int idx : templ.operandIndices) {
            if (idx >= 0 && static_cast<size_t>(idx) < values.size()) {
                operands.push_back(values[idx]);
            }
        }

        if (operands.empty()) continue;

        llvm::Value* result = nullptr;

        switch (templ.opcode) {
            // Binary arithmetic
            case llvm::Instruction::Add:
                result = builder.CreateAdd(operands[0], operands[1]);
                break;
            case llvm::Instruction::Sub:
                result = builder.CreateSub(operands[0], operands[1]);
                break;
            case llvm::Instruction::Mul:
                result = builder.CreateMul(operands[0], operands[1]);
                break;
            case llvm::Instruction::UDiv:
                result = builder.CreateUDiv(operands[0], operands[1]);
                break;
            case llvm::Instruction::SDiv:
                result = builder.CreateSDiv(operands[0], operands[1]);
                break;
            case llvm::Instruction::URem:
                result = builder.CreateURem(operands[0], operands[1]);
                break;
            case llvm::Instruction::SRem:
                result = builder.CreateSRem(operands[0], operands[1]);
                break;

            // Floating point
            case llvm::Instruction::FAdd:
                result = builder.CreateFAdd(operands[0], operands[1]);
                break;
            case llvm::Instruction::FSub:
                result = builder.CreateFSub(operands[0], operands[1]);
                break;
            case llvm::Instruction::FMul:
                result = builder.CreateFMul(operands[0], operands[1]);
                break;
            case llvm::Instruction::FDiv:
                result = builder.CreateFDiv(operands[0], operands[1]);
                break;

            // Logical
            case llvm::Instruction::And:
                result = builder.CreateAnd(operands[0], operands[1]);
                break;
            case llvm::Instruction::Or:
                result = builder.CreateOr(operands[0], operands[1]);
                break;
            case llvm::Instruction::Xor:
                result = builder.CreateXor(operands[0], operands[1]);
                break;

            // Shifts
            case llvm::Instruction::Shl:
                result = builder.CreateShl(operands[0], operands[1]);
                break;
            case llvm::Instruction::LShr:
                result = builder.CreateLShr(operands[0], operands[1]);
                break;
            case llvm::Instruction::AShr:
                result = builder.CreateAShr(operands[0], operands[1]);
                break;

            // Comparisons
            case llvm::Instruction::ICmp:
                result = builder.CreateICmp(templ.predicate, operands[0], operands[1]);
                break;
            case llvm::Instruction::FCmp:
                result = builder.CreateFCmp(
                    static_cast<llvm::CmpInst::Predicate>(templ.predicate),
                    operands[0], operands[1]);
                break;

            // Select
            case llvm::Instruction::Select:
                if (operands.size() >= 3) {
                    result = builder.CreateSelect(operands[0], operands[1], operands[2]);
                }
                break;

            // Casts
            case llvm::Instruction::Trunc:
                result = builder.CreateTrunc(operands[0], templ.resultType);
                break;
            case llvm::Instruction::ZExt:
                result = builder.CreateZExt(operands[0], templ.resultType);
                break;
            case llvm::Instruction::SExt:
                result = builder.CreateSExt(operands[0], templ.resultType);
                break;

            // Unary negation (implemented as 0 - x or xor with -1)
            case llvm::Instruction::FNeg:
                result = builder.CreateFNeg(operands[0]);
                break;

            default:
                // Unknown opcode
                break;
        }

        if (result) {
            values.push_back(result);
        }
    }

    // Return the output value
    if (outputIndex >= 0 && static_cast<size_t>(outputIndex) < values.size()) {
        return values[outputIndex];
    }
    return values.empty() ? nullptr : values.back();
}

// Enumerator implementation

Enumerator::Enumerator(const Config& config) : config_(config) {
    initDefaultOpcodes();
}

void Enumerator::initDefaultOpcodes() {
    // Common arithmetic and logical operations
    enumeratedOpcodes_ = {
        // Arithmetic
        llvm::Instruction::Add,
        llvm::Instruction::Sub,
        llvm::Instruction::Mul,
        llvm::Instruction::UDiv,
        llvm::Instruction::SDiv,
        llvm::Instruction::URem,
        llvm::Instruction::SRem,

        // Logical
        llvm::Instruction::And,
        llvm::Instruction::Or,
        llvm::Instruction::Xor,

        // Shifts
        llvm::Instruction::Shl,
        llvm::Instruction::LShr,
        llvm::Instruction::AShr,

        // Comparisons
        llvm::Instruction::ICmp,

        // Select (conditional move)
        llvm::Instruction::Select,
    };
}

bool Enumerator::isValidOpcode(unsigned opcode,
                                const std::vector<llvm::Type*>& operandTypes,
                                llvm::Type* resultType) {
    if (operandTypes.empty()) return false;

    switch (opcode) {
        // Binary integer operations require matching integer types
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
            if (operandTypes.size() < 2) return false;
            if (!operandTypes[0]->isIntegerTy()) return false;
            if (operandTypes[0] != operandTypes[1]) return false;
            return true;

        // Binary FP operations
        case llvm::Instruction::FAdd:
        case llvm::Instruction::FSub:
        case llvm::Instruction::FMul:
        case llvm::Instruction::FDiv:
        case llvm::Instruction::FRem:
            if (operandTypes.size() < 2) return false;
            if (!operandTypes[0]->isFloatingPointTy()) return false;
            if (operandTypes[0] != operandTypes[1]) return false;
            return true;

        // Integer comparison
        case llvm::Instruction::ICmp:
            if (operandTypes.size() < 2) return false;
            if (!operandTypes[0]->isIntegerTy() &&
                !operandTypes[0]->isPointerTy()) return false;
            if (operandTypes[0] != operandTypes[1]) return false;
            return true;

        // FP comparison
        case llvm::Instruction::FCmp:
            if (operandTypes.size() < 2) return false;
            if (!operandTypes[0]->isFloatingPointTy()) return false;
            if (operandTypes[0] != operandTypes[1]) return false;
            return true;

        // Select needs i1 condition and matching true/false types
        case llvm::Instruction::Select:
            if (operandTypes.size() < 3) return false;
            if (!operandTypes[0]->isIntegerTy(1)) return false;
            if (operandTypes[1] != operandTypes[2]) return false;
            return true;

        default:
            return false;
    }
}

llvm::Type* Enumerator::getResultType(unsigned opcode,
                                       const std::vector<llvm::Type*>& operandTypes) {
    if (operandTypes.empty()) return nullptr;

    switch (opcode) {
        // Binary ops return same type as operands
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
        case llvm::Instruction::FAdd:
        case llvm::Instruction::FSub:
        case llvm::Instruction::FMul:
        case llvm::Instruction::FDiv:
        case llvm::Instruction::FRem:
            return operandTypes[0];

        // Comparisons return i1
        case llvm::Instruction::ICmp:
        case llvm::Instruction::FCmp:
            return llvm::Type::getInt1Ty(operandTypes[0]->getContext());

        // Select returns type of true/false values
        case llvm::Instruction::Select:
            if (operandTypes.size() >= 2) return operandTypes[1];
            return nullptr;

        default:
            return nullptr;
    }
}

bool Enumerator::enumerate(llvm::Type* outputType,
                            const std::vector<llvm::Type*>& inputTypes,
                            size_t maxLength,
                            CandidateCallback callback) {
    candidatesGenerated_ = 0;

    SynthesizedSequence current;
    std::vector<llvm::Type*> availableTypes = inputTypes;

    return enumerateRecursive(availableTypes, outputType, 0, maxLength,
                              current, callback);
}

bool Enumerator::enumerateRecursive(
    const std::vector<llvm::Type*>& availableTypes,
    llvm::Type* targetType,
    size_t currentLength,
    size_t maxLength,
    SynthesizedSequence& current,
    CandidateCallback& callback) {

    // Check if any available value matches target type
    for (size_t i = 0; i < availableTypes.size(); ++i) {
        if (availableTypes[i] == targetType) {
            current.outputIndex = static_cast<int>(i);
            candidatesGenerated_++;

            // Call the callback with this candidate
            if (!callback(current)) {
                return false;  // Callback requested stop
            }
        }
    }

    // If we've reached max length, stop
    if (currentLength >= maxLength) {
        return true;
    }

    // Try adding each opcode
    for (unsigned opcode : enumeratedOpcodes_) {
        // Determine operand count for this opcode
        size_t numOperands = 2;  // Most are binary
        if (opcode == llvm::Instruction::Select) {
            numOperands = 3;
        }

        // Enumerate all combinations of operands
        std::vector<size_t> operandIndices(numOperands, 0);
        bool done = false;

        while (!done) {
            // Build operand types for this combination
            std::vector<llvm::Type*> operandTypes;
            for (size_t i = 0; i < numOperands; ++i) {
                if (operandIndices[i] < availableTypes.size()) {
                    operandTypes.push_back(availableTypes[operandIndices[i]]);
                }
            }

            // Check if this is a valid combination
            if (operandTypes.size() == numOperands &&
                isValidOpcode(opcode, operandTypes, targetType)) {

                // Create template
                InstructionTemplate templ;
                templ.opcode = opcode;
                for (size_t idx : operandIndices) {
                    templ.operandIndices.push_back(static_cast<int>(idx));
                }
                templ.resultType = getResultType(opcode, operandTypes);

                // Add comparison predicates for ICmp
                if (opcode == llvm::Instruction::ICmp) {
                    // Try different predicates
                    std::vector<llvm::CmpInst::Predicate> predicates = {
                        llvm::CmpInst::ICMP_EQ,
                        llvm::CmpInst::ICMP_NE,
                        llvm::CmpInst::ICMP_ULT,
                        llvm::CmpInst::ICMP_ULE,
                        llvm::CmpInst::ICMP_UGT,
                        llvm::CmpInst::ICMP_UGE,
                        llvm::CmpInst::ICMP_SLT,
                        llvm::CmpInst::ICMP_SLE,
                        llvm::CmpInst::ICMP_SGT,
                        llvm::CmpInst::ICMP_SGE,
                    };

                    for (auto pred : predicates) {
                        templ.predicate = pred;
                        current.templates.push_back(templ);

                        // Extend available types
                        std::vector<llvm::Type*> newAvailable = availableTypes;
                        newAvailable.push_back(templ.resultType);

                        // Recurse
                        if (!enumerateRecursive(newAvailable, targetType,
                                               currentLength + 1, maxLength,
                                               current, callback)) {
                            return false;
                        }

                        current.templates.pop_back();
                    }
                } else {
                    current.templates.push_back(templ);

                    // Extend available types
                    std::vector<llvm::Type*> newAvailable = availableTypes;
                    newAvailable.push_back(templ.resultType);

                    // Recurse
                    if (!enumerateRecursive(newAvailable, targetType,
                                           currentLength + 1, maxLength,
                                           current, callback)) {
                        return false;
                    }

                    current.templates.pop_back();
                }
            }

            // Increment operand indices (lexicographic order)
            size_t pos = 0;
            while (pos < numOperands) {
                operandIndices[pos]++;
                if (operandIndices[pos] < availableTypes.size()) {
                    break;
                }
                operandIndices[pos] = 0;
                pos++;
            }
            if (pos >= numOperands) {
                done = true;
            }
        }
    }

    return true;
}

bool Enumerator::enumerateReplacements(const llvm::Instruction& original,
                                        size_t maxLength,
                                        CandidateCallback callback) {
    // Get input types from original instruction's operands
    std::vector<llvm::Type*> inputTypes;
    for (const auto& op : original.operands()) {
        inputTypes.push_back(op->getType());
    }

    // Get output type
    llvm::Type* outputType = original.getType();

    return enumerate(outputType, inputTypes, maxLength, callback);
}

bool Enumerator::enumerateBlockReplacements(const llvm::BasicBlock& original,
                                             size_t maxLength,
                                             CandidateCallback callback) {
    // Get live-in values (values used but not defined in block)
    std::vector<llvm::Type*> inputTypes;
    std::unordered_set<const llvm::Value*> definedInBlock;

    for (const auto& inst : original) {
        definedInBlock.insert(&inst);
    }

    for (const auto& inst : original) {
        for (const auto& op : inst.operands()) {
            if (definedInBlock.find(op) == definedInBlock.end()) {
                // This operand is live-in
                inputTypes.push_back(op->getType());
            }
        }
    }

    // Get output type (return value or last non-terminator result)
    llvm::Type* outputType = nullptr;
    for (auto it = original.rbegin(); it != original.rend(); ++it) {
        if (!it->isTerminator() && !it->getType()->isVoidTy()) {
            outputType = it->getType();
            break;
        }
    }

    if (!outputType) {
        return true;  // Nothing to optimize
    }

    return enumerate(outputType, inputTypes, maxLength, callback);
}

} // namespace superopt
