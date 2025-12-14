#include "superopt/Enumerator.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"

#include <cmath>
#include <algorithm>

namespace superopt {

// SynthesisConstant implementation

SynthesisConstant SynthesisConstant::fromInt(llvm::LLVMContext& ctx,
                                              llvm::Type* type, int64_t val) {
    SynthesisConstant sc;
    sc.intValue = val;
    sc.isNegative = val < 0;
    sc.isPowerOfTwo = (val > 0) && ((val & (val - 1)) == 0);

    if (type->isIntegerTy()) {
        sc.value = llvm::ConstantInt::get(type, val, true);
    }

    return sc;
}

// ConstantPool implementation

ConstantPool ConstantPool::getDefaultPool(llvm::LLVMContext& ctx,
                                           llvm::Type* type,
                                           size_t maxConstants) {
    ConstantPool pool;

    if (!type->isIntegerTy()) {
        return pool;
    }

    unsigned bitWidth = type->getIntegerBitWidth();

    // Essential constants
    std::vector<int64_t> values = {
        0, 1, -1, 2, -2
    };

    // Powers of 2 (useful for strength reduction)
    for (unsigned i = 2; i < bitWidth && values.size() < maxConstants; ++i) {
        values.push_back(1LL << i);
    }

    // Powers of 2 minus 1 (bit masks)
    for (unsigned i = 1; i < bitWidth && values.size() < maxConstants; ++i) {
        int64_t mask = (1LL << i) - 1;
        if (std::find(values.begin(), values.end(), mask) == values.end()) {
            values.push_back(mask);
        }
    }

    // Add some small constants
    for (int64_t v = 3; v <= 8 && values.size() < maxConstants; ++v) {
        if (std::find(values.begin(), values.end(), v) == values.end()) {
            values.push_back(v);
        }
    }

    // Create constants
    for (int64_t val : values) {
        if (pool.size() >= maxConstants) break;
        pool.add(SynthesisConstant::fromInt(ctx, type, val));
    }

    return pool;
}

std::vector<SynthesisConstant> ConstantPool::getConstantsOfType(llvm::Type* type) const {
    std::vector<SynthesisConstant> result;
    for (const auto& c : constants_) {
        if (c.value && c.value->getType() == type) {
            result.push_back(c);
        }
    }
    return result;
}

// SynthesizedSequence implementation

llvm::Value* SynthesizedSequence::apply(llvm::IRBuilder<>& builder,
                                         const std::vector<llvm::Value*>& inputs) const {
    std::vector<llvm::Value*> values = inputs;

    // Add constants to available values
    for (const auto& c : constants) {
        if (c.value) {
            values.push_back(c.value);
        }
    }

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
                if (operands.size() >= 2)
                    result = builder.CreateAdd(operands[0], operands[1]);
                break;
            case llvm::Instruction::Sub:
                if (operands.size() >= 2)
                    result = builder.CreateSub(operands[0], operands[1]);
                break;
            case llvm::Instruction::Mul:
                if (operands.size() >= 2)
                    result = builder.CreateMul(operands[0], operands[1]);
                break;
            case llvm::Instruction::UDiv:
                if (operands.size() >= 2)
                    result = builder.CreateUDiv(operands[0], operands[1]);
                break;
            case llvm::Instruction::SDiv:
                if (operands.size() >= 2)
                    result = builder.CreateSDiv(operands[0], operands[1]);
                break;
            case llvm::Instruction::URem:
                if (operands.size() >= 2)
                    result = builder.CreateURem(operands[0], operands[1]);
                break;
            case llvm::Instruction::SRem:
                if (operands.size() >= 2)
                    result = builder.CreateSRem(operands[0], operands[1]);
                break;

            // Floating point
            case llvm::Instruction::FAdd:
                if (operands.size() >= 2)
                    result = builder.CreateFAdd(operands[0], operands[1]);
                break;
            case llvm::Instruction::FSub:
                if (operands.size() >= 2)
                    result = builder.CreateFSub(operands[0], operands[1]);
                break;
            case llvm::Instruction::FMul:
                if (operands.size() >= 2)
                    result = builder.CreateFMul(operands[0], operands[1]);
                break;
            case llvm::Instruction::FDiv:
                if (operands.size() >= 2)
                    result = builder.CreateFDiv(operands[0], operands[1]);
                break;

            // Logical
            case llvm::Instruction::And:
                if (operands.size() >= 2)
                    result = builder.CreateAnd(operands[0], operands[1]);
                break;
            case llvm::Instruction::Or:
                if (operands.size() >= 2)
                    result = builder.CreateOr(operands[0], operands[1]);
                break;
            case llvm::Instruction::Xor:
                if (operands.size() >= 2)
                    result = builder.CreateXor(operands[0], operands[1]);
                break;

            // Shifts
            case llvm::Instruction::Shl:
                if (operands.size() >= 2)
                    result = builder.CreateShl(operands[0], operands[1]);
                break;
            case llvm::Instruction::LShr:
                if (operands.size() >= 2)
                    result = builder.CreateLShr(operands[0], operands[1]);
                break;
            case llvm::Instruction::AShr:
                if (operands.size() >= 2)
                    result = builder.CreateAShr(operands[0], operands[1]);
                break;

            // Comparisons
            case llvm::Instruction::ICmp:
                if (operands.size() >= 2)
                    result = builder.CreateICmp(templ.predicate, operands[0], operands[1]);
                break;
            case llvm::Instruction::FCmp:
                if (operands.size() >= 2)
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

            // Unary negation
            case llvm::Instruction::FNeg:
                result = builder.CreateFNeg(operands[0]);
                break;

            default:
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

uint64_t SynthesizedSequence::getHash() const {
    uint64_t hash = templates.size() * 31;
    hash ^= static_cast<uint64_t>(outputIndex + 1000) * 17;

    for (size_t i = 0; i < templates.size(); ++i) {
        const auto& t = templates[i];
        hash ^= (static_cast<uint64_t>(t.opcode) << (i % 8)) * 37;
        for (size_t j = 0; j < t.operandIndices.size(); ++j) {
            hash ^= static_cast<uint64_t>(t.operandIndices[j] + 100) * (j + 1) * 53;
        }
        hash ^= static_cast<uint64_t>(t.predicate) * 97;
    }

    for (size_t i = 0; i < constants.size(); ++i) {
        hash ^= static_cast<uint64_t>(constants[i].intValue) * (i + 1) * 113;
    }

    return hash;
}

double SynthesizedSequence::estimateCost() const {
    double cost = 0.0;
    for (const auto& t : templates) {
        switch (t.opcode) {
            case llvm::Instruction::Add:
            case llvm::Instruction::Sub:
            case llvm::Instruction::And:
            case llvm::Instruction::Or:
            case llvm::Instruction::Xor:
            case llvm::Instruction::Shl:
            case llvm::Instruction::LShr:
            case llvm::Instruction::AShr:
            case llvm::Instruction::ICmp:
                cost += 1.0;
                break;
            case llvm::Instruction::Mul:
                cost += 3.0;
                break;
            case llvm::Instruction::UDiv:
            case llvm::Instruction::SDiv:
            case llvm::Instruction::URem:
            case llvm::Instruction::SRem:
                cost += 20.0;
                break;
            default:
                cost += 1.0;
        }
    }
    return cost;
}

// BlockSpec implementation

BlockSpec BlockSpec::fromBasicBlock(const llvm::BasicBlock& bb) {
    BlockSpec spec;

    std::unordered_set<const llvm::Value*> definedInBlock;
    for (const auto& inst : bb) {
        definedInBlock.insert(&inst);
    }

    // Find inputs (values used but not defined in block)
    std::unordered_set<const llvm::Value*> seenInputs;
    for (const auto& inst : bb) {
        for (const auto& op : inst.operands()) {
            if (definedInBlock.find(op) == definedInBlock.end() &&
                seenInputs.find(op) == seenInputs.end()) {
                spec.inputs.push_back(const_cast<llvm::Value*>(op.get()));
                spec.inputTypes.push_back(op->getType());
                seenInputs.insert(op);
            }
        }
    }

    // Find outputs (values used outside block)
    for (const auto& inst : bb) {
        for (const auto* user : inst.users()) {
            if (auto* userInst = llvm::dyn_cast<llvm::Instruction>(user)) {
                if (userInst->getParent() != &bb) {
                    spec.outputs.push_back(const_cast<llvm::Value*>(
                        static_cast<const llvm::Value*>(&inst)));
                    spec.outputTypes.push_back(inst.getType());
                    break;
                }
            }
        }
    }

    return spec;
}

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

// Enumerator implementation

Enumerator::Enumerator(const Config& config) : config_(config) {
    initDefaultOpcodes();
}

void Enumerator::initDefaultOpcodes() {
    enumeratedOpcodes_ = {
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
        llvm::Instruction::ICmp,
        llvm::Instruction::Select,
    };
}

bool Enumerator::isValidOpcode(unsigned opcode,
                                const std::vector<llvm::Type*>& operandTypes,
                                llvm::Type* resultType) {
    if (operandTypes.empty()) return false;

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
            if (operandTypes.size() < 2) return false;
            if (!operandTypes[0]->isIntegerTy()) return false;
            if (operandTypes[0] != operandTypes[1]) return false;
            return true;

        case llvm::Instruction::FAdd:
        case llvm::Instruction::FSub:
        case llvm::Instruction::FMul:
        case llvm::Instruction::FDiv:
        case llvm::Instruction::FRem:
            if (operandTypes.size() < 2) return false;
            if (!operandTypes[0]->isFloatingPointTy()) return false;
            if (operandTypes[0] != operandTypes[1]) return false;
            return true;

        case llvm::Instruction::ICmp:
            if (operandTypes.size() < 2) return false;
            if (!operandTypes[0]->isIntegerTy() &&
                !operandTypes[0]->isPointerTy()) return false;
            if (operandTypes[0] != operandTypes[1]) return false;
            return true;

        case llvm::Instruction::FCmp:
            if (operandTypes.size() < 2) return false;
            if (!operandTypes[0]->isFloatingPointTy()) return false;
            if (operandTypes[0] != operandTypes[1]) return false;
            return true;

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

        case llvm::Instruction::ICmp:
        case llvm::Instruction::FCmp:
            return llvm::Type::getInt1Ty(operandTypes[0]->getContext());

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
    timedOut_ = false;
    timer_.reset();

    // If constants are enabled, use constant enumeration
    if (config_.enableConstantSynthesis && outputType->isIntegerTy()) {
        auto& ctx = outputType->getContext();
        ConstantPool pool = ConstantPool::getDefaultPool(ctx, outputType,
                                                          config_.maxConstants);
        return enumerateWithConstants(outputType, inputTypes, pool, maxLength, callback);
    }

    SynthesizedSequence current;
    std::vector<llvm::Type*> availableTypes = inputTypes;

    return enumerateRecursive(availableTypes, outputType, 0, maxLength,
                              current, callback);
}

bool Enumerator::enumerateWithConstants(llvm::Type* outputType,
                                          const std::vector<llvm::Type*>& inputTypes,
                                          const ConstantPool& constants,
                                          size_t maxLength,
                                          CandidateCallback callback) {
    candidatesGenerated_ = 0;
    timedOut_ = false;
    timer_.reset();

    SynthesizedSequence current;

    // Add constants to the sequence
    for (const auto& c : constants.getConstants()) {
        current.constants.push_back(c);
    }

    // Build available types: inputs first, then constants
    std::vector<llvm::Type*> availableTypes = inputTypes;
    for (const auto& c : constants.getConstants()) {
        if (c.value) {
            availableTypes.push_back(c.value->getType());
        }
    }

    return enumerateRecursiveWithConstants(availableTypes, constants, outputType,
                                            0, maxLength, current, callback);
}

bool Enumerator::enumerateRecursive(
    const std::vector<llvm::Type*>& availableTypes,
    llvm::Type* targetType,
    size_t currentLength,
    size_t maxLength,
    SynthesizedSequence& current,
    CandidateCallback& callback) {

    if (checkTimeout()) return false;

    // Check if any available value matches target type
    for (size_t i = 0; i < availableTypes.size(); ++i) {
        if (availableTypes[i] == targetType) {
            current.outputIndex = static_cast<int>(i);
            candidatesGenerated_++;

            if (!callback(current)) {
                return false;
            }
        }
    }

    if (currentLength >= maxLength) {
        return true;
    }

    for (unsigned opcode : enumeratedOpcodes_) {
        if (checkTimeout()) return false;

        size_t numOperands = 2;
        if (opcode == llvm::Instruction::Select) {
            numOperands = 3;
        }

        std::vector<size_t> operandIndices(numOperands, 0);
        bool done = false;

        while (!done) {
            std::vector<llvm::Type*> operandTypes;
            for (size_t i = 0; i < numOperands; ++i) {
                if (operandIndices[i] < availableTypes.size()) {
                    operandTypes.push_back(availableTypes[operandIndices[i]]);
                }
            }

            if (operandTypes.size() == numOperands &&
                isValidOpcode(opcode, operandTypes, targetType)) {

                InstructionTemplate templ;
                templ.opcode = opcode;
                for (size_t idx : operandIndices) {
                    templ.operandIndices.push_back(static_cast<int>(idx));
                }
                templ.resultType = getResultType(opcode, operandTypes);

                if (opcode == llvm::Instruction::ICmp) {
                    std::vector<llvm::CmpInst::Predicate> predicates = {
                        llvm::CmpInst::ICMP_EQ, llvm::CmpInst::ICMP_NE,
                        llvm::CmpInst::ICMP_ULT, llvm::CmpInst::ICMP_ULE,
                        llvm::CmpInst::ICMP_UGT, llvm::CmpInst::ICMP_UGE,
                        llvm::CmpInst::ICMP_SLT, llvm::CmpInst::ICMP_SLE,
                        llvm::CmpInst::ICMP_SGT, llvm::CmpInst::ICMP_SGE,
                    };

                    for (auto pred : predicates) {
                        templ.predicate = pred;
                        current.templates.push_back(templ);

                        std::vector<llvm::Type*> newAvailable = availableTypes;
                        newAvailable.push_back(templ.resultType);

                        if (!enumerateRecursive(newAvailable, targetType,
                                               currentLength + 1, maxLength,
                                               current, callback)) {
                            return false;
                        }

                        current.templates.pop_back();
                    }
                } else {
                    current.templates.push_back(templ);

                    std::vector<llvm::Type*> newAvailable = availableTypes;
                    newAvailable.push_back(templ.resultType);

                    if (!enumerateRecursive(newAvailable, targetType,
                                           currentLength + 1, maxLength,
                                           current, callback)) {
                        return false;
                    }

                    current.templates.pop_back();
                }
            }

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

bool Enumerator::enumerateRecursiveWithConstants(
    std::vector<llvm::Type*>& availableTypes,
    const ConstantPool& constants,
    llvm::Type* targetType,
    size_t currentLength,
    size_t maxLength,
    SynthesizedSequence& current,
    CandidateCallback& callback) {

    if (checkTimeout()) return false;

    // Check if any available value matches target type
    for (size_t i = 0; i < availableTypes.size(); ++i) {
        if (availableTypes[i] == targetType) {
            current.outputIndex = static_cast<int>(i);
            candidatesGenerated_++;

            if (!callback(current)) {
                return false;
            }
        }
    }

    if (currentLength >= maxLength) {
        return true;
    }

    for (unsigned opcode : enumeratedOpcodes_) {
        if (checkTimeout()) return false;

        size_t numOperands = 2;
        if (opcode == llvm::Instruction::Select) {
            numOperands = 3;
        }

        std::vector<size_t> operandIndices(numOperands, 0);
        bool done = false;

        while (!done) {
            std::vector<llvm::Type*> operandTypes;
            for (size_t i = 0; i < numOperands; ++i) {
                if (operandIndices[i] < availableTypes.size()) {
                    operandTypes.push_back(availableTypes[operandIndices[i]]);
                }
            }

            if (operandTypes.size() == numOperands &&
                isValidOpcode(opcode, operandTypes, targetType)) {

                InstructionTemplate templ;
                templ.opcode = opcode;
                for (size_t idx : operandIndices) {
                    templ.operandIndices.push_back(static_cast<int>(idx));
                }
                templ.resultType = getResultType(opcode, operandTypes);

                if (opcode == llvm::Instruction::ICmp) {
                    std::vector<llvm::CmpInst::Predicate> predicates = {
                        llvm::CmpInst::ICMP_EQ, llvm::CmpInst::ICMP_NE,
                        llvm::CmpInst::ICMP_ULT, llvm::CmpInst::ICMP_SLT,
                    };

                    for (auto pred : predicates) {
                        templ.predicate = pred;
                        current.templates.push_back(templ);

                        availableTypes.push_back(templ.resultType);

                        if (!enumerateRecursiveWithConstants(availableTypes, constants,
                                                              targetType, currentLength + 1,
                                                              maxLength, current, callback)) {
                            availableTypes.pop_back();
                            return false;
                        }

                        availableTypes.pop_back();
                        current.templates.pop_back();
                    }
                } else {
                    current.templates.push_back(templ);
                    availableTypes.push_back(templ.resultType);

                    if (!enumerateRecursiveWithConstants(availableTypes, constants,
                                                          targetType, currentLength + 1,
                                                          maxLength, current, callback)) {
                        availableTypes.pop_back();
                        return false;
                    }

                    availableTypes.pop_back();
                    current.templates.pop_back();
                }
            }

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

bool Enumerator::enumerateIterativeDeepening(llvm::Type* outputType,
                                               const std::vector<llvm::Type*>& inputTypes,
                                               size_t maxLength,
                                               CandidateCallback callback) {
    timedOut_ = false;
    timer_.reset();

    for (size_t depth = 0; depth <= maxLength; ++depth) {
        if (checkTimeout()) return false;

        bool foundAtThisDepth = false;
        auto wrappedCallback = [&](const SynthesizedSequence& seq) -> bool {
            if (seq.templates.size() == depth) {
                foundAtThisDepth = true;
                return callback(seq);
            }
            return true;  // Continue
        };

        if (!enumerate(outputType, inputTypes, depth, wrappedCallback)) {
            return false;  // Callback requested stop
        }
    }

    return true;
}

bool Enumerator::enumerateReplacements(const llvm::Instruction& original,
                                        size_t maxLength,
                                        CandidateCallback callback) {
    std::vector<llvm::Type*> inputTypes;
    for (const auto& op : original.operands()) {
        inputTypes.push_back(op->getType());
    }

    llvm::Type* outputType = original.getType();

    if (config_.searchStrategy == SearchStrategy::IterativeDeepening) {
        return enumerateIterativeDeepening(outputType, inputTypes, maxLength, callback);
    }

    return enumerate(outputType, inputTypes, maxLength, callback);
}

bool Enumerator::enumerateBlockReplacements(const llvm::BasicBlock& original,
                                             size_t maxLength,
                                             CandidateCallback callback) {
    BlockSpec spec = BlockSpec::fromBasicBlock(original);

    if (spec.outputTypes.empty()) {
        return true;
    }

    // For now, optimize for single output
    return enumerate(spec.outputTypes[0], spec.inputTypes, maxLength, callback);
}

// StochasticEnumerator implementation

StochasticEnumerator::StochasticEnumerator(const Config& config)
    : config_(config), rng_(std::random_device{}()) {}

std::optional<SynthesizedSequence> StochasticEnumerator::search(
    llvm::Type* outputType,
    const std::vector<llvm::Type*>& inputTypes,
    const ConstantPool& constants,
    std::function<double(const SynthesizedSequence&)> costFn,
    std::function<bool(const SynthesizedSequence&)> verifyFn,
    size_t maxIterations) {

    iterations_ = 0;
    accepted_ = 0;

    // Start with a random sequence
    SynthesizedSequence current = generateRandom(outputType, inputTypes, constants, 2);
    double currentCost = std::numeric_limits<double>::infinity();

    // Try to find a valid starting point
    for (size_t i = 0; i < 100 && !verifyFn(current); ++i) {
        current = generateRandom(outputType, inputTypes, constants,
                                  1 + (i % config_.maxInstructions));
    }

    if (verifyFn(current)) {
        currentCost = costFn(current);
    }

    std::optional<SynthesizedSequence> best;
    double bestCost = std::numeric_limits<double>::infinity();

    if (currentCost < std::numeric_limits<double>::infinity()) {
        best = current;
        bestCost = currentCost;
    }

    double temperature = config_.stochasticTemperature;

    for (iterations_ = 0; iterations_ < maxIterations; ++iterations_) {
        // Generate proposal
        std::uniform_int_distribution<int> mutationType(0, 4);
        SynthesizedSequence proposal;

        switch (mutationType(rng_)) {
            case 0:
                proposal = mutateOpcode(current, inputTypes);
                break;
            case 1:
                proposal = mutateOperand(current, inputTypes.size());
                break;
            case 2:
                proposal = swapInstructions(current);
                break;
            case 3:
                proposal = insertInstruction(current, inputTypes);
                break;
            case 4:
                proposal = deleteInstruction(current);
                break;
        }

        // Check if proposal is valid
        if (verifyFn(proposal)) {
            double proposalCost = costFn(proposal);

            if (shouldAccept(currentCost, proposalCost, temperature)) {
                current = proposal;
                currentCost = proposalCost;
                accepted_++;

                if (currentCost < bestCost) {
                    best = current;
                    bestCost = currentCost;
                }
            }
        }

        // Cool down
        temperature *= 0.999;
    }

    return best;
}

SynthesizedSequence StochasticEnumerator::generateRandom(
    llvm::Type* outputType,
    const std::vector<llvm::Type*>& inputTypes,
    const ConstantPool& constants,
    size_t length) {

    SynthesizedSequence seq;

    // Copy constants
    for (const auto& c : constants.getConstants()) {
        seq.constants.push_back(c);
    }

    std::vector<unsigned> opcodes = {
        llvm::Instruction::Add, llvm::Instruction::Sub,
        llvm::Instruction::Mul, llvm::Instruction::And,
        llvm::Instruction::Or, llvm::Instruction::Xor,
        llvm::Instruction::Shl, llvm::Instruction::LShr,
    };

    size_t numValues = inputTypes.size() + constants.size();

    for (size_t i = 0; i < length; ++i) {
        InstructionTemplate templ;

        std::uniform_int_distribution<size_t> opDist(0, opcodes.size() - 1);
        templ.opcode = opcodes[opDist(rng_)];

        std::uniform_int_distribution<size_t> valDist(0, numValues - 1);
        templ.operandIndices.push_back(static_cast<int>(valDist(rng_)));
        templ.operandIndices.push_back(static_cast<int>(valDist(rng_)));

        templ.resultType = outputType;
        seq.templates.push_back(templ);
        numValues++;
    }

    seq.outputIndex = static_cast<int>(numValues - 1);
    return seq;
}

SynthesizedSequence StochasticEnumerator::mutateOpcode(
    const SynthesizedSequence& seq,
    const std::vector<llvm::Type*>& inputTypes) {

    if (seq.templates.empty()) return seq;

    SynthesizedSequence result = seq;

    std::uniform_int_distribution<size_t> posDist(0, result.templates.size() - 1);
    size_t pos = posDist(rng_);

    std::vector<unsigned> opcodes = {
        llvm::Instruction::Add, llvm::Instruction::Sub,
        llvm::Instruction::Mul, llvm::Instruction::And,
        llvm::Instruction::Or, llvm::Instruction::Xor,
        llvm::Instruction::Shl, llvm::Instruction::LShr,
    };

    std::uniform_int_distribution<size_t> opDist(0, opcodes.size() - 1);
    result.templates[pos].opcode = opcodes[opDist(rng_)];

    return result;
}

SynthesizedSequence StochasticEnumerator::mutateOperand(
    const SynthesizedSequence& seq,
    size_t numInputs) {

    if (seq.templates.empty()) return seq;

    SynthesizedSequence result = seq;

    std::uniform_int_distribution<size_t> posDist(0, result.templates.size() - 1);
    size_t pos = posDist(rng_);

    size_t numValues = numInputs + seq.constants.size() + pos;

    if (numValues == 0) return seq;

    std::uniform_int_distribution<size_t> opIdx(0,
        result.templates[pos].operandIndices.size() - 1);
    std::uniform_int_distribution<size_t> valDist(0, numValues - 1);

    result.templates[pos].operandIndices[opIdx(rng_)] =
        static_cast<int>(valDist(rng_));

    return result;
}

SynthesizedSequence StochasticEnumerator::swapInstructions(
    const SynthesizedSequence& seq) {

    if (seq.templates.size() < 2) return seq;

    SynthesizedSequence result = seq;

    std::uniform_int_distribution<size_t> dist(0, result.templates.size() - 1);
    size_t i = dist(rng_);
    size_t j = dist(rng_);

    std::swap(result.templates[i], result.templates[j]);
    return result;
}

SynthesizedSequence StochasticEnumerator::insertInstruction(
    const SynthesizedSequence& seq,
    const std::vector<llvm::Type*>& inputTypes) {

    SynthesizedSequence result = seq;

    InstructionTemplate templ;
    std::vector<unsigned> opcodes = {
        llvm::Instruction::Add, llvm::Instruction::Sub,
        llvm::Instruction::And, llvm::Instruction::Xor,
    };

    std::uniform_int_distribution<size_t> opDist(0, opcodes.size() - 1);
    templ.opcode = opcodes[opDist(rng_)];

    size_t numValues = inputTypes.size() + seq.constants.size();
    std::uniform_int_distribution<size_t> valDist(0, numValues > 0 ? numValues - 1 : 0);

    templ.operandIndices.push_back(static_cast<int>(valDist(rng_)));
    templ.operandIndices.push_back(static_cast<int>(valDist(rng_)));

    result.templates.push_back(templ);
    result.outputIndex = static_cast<int>(
        inputTypes.size() + result.constants.size() + result.templates.size() - 1);

    return result;
}

SynthesizedSequence StochasticEnumerator::deleteInstruction(
    const SynthesizedSequence& seq) {

    if (seq.templates.empty()) return seq;

    SynthesizedSequence result = seq;

    std::uniform_int_distribution<size_t> dist(0, result.templates.size() - 1);
    size_t pos = dist(rng_);

    result.templates.erase(result.templates.begin() + pos);

    if (result.outputIndex > static_cast<int>(pos)) {
        result.outputIndex--;
    }

    return result;
}

bool StochasticEnumerator::shouldAccept(double currentCost, double proposedCost,
                                         double temperature) {
    if (proposedCost < currentCost) {
        return true;
    }

    double delta = proposedCost - currentCost;
    double probability = std::exp(-delta / temperature);

    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng_) < probability;
}

} // namespace superopt
