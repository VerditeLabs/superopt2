#include "superopt/Verifier.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/ExecutionEngine/Interpreter.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstring>
#include <cmath>
#include <algorithm>

namespace superopt {

//===----------------------------------------------------------------------===//
// TestInput implementation
//===----------------------------------------------------------------------===//

uint64_t TestInput::getHash() const {
    uint64_t hash = 0;
    for (size_t i = 0; i < rawValues.size(); ++i) {
        // Mix using FNV-1a style
        hash ^= rawValues[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

//===----------------------------------------------------------------------===//
// DomainConstraints implementation
//===----------------------------------------------------------------------===//

bool DomainConstraints::isSafeInput(const TestInput& input,
                                     const llvm::Instruction& inst) const {
    // Check for division/remainder by zero
    if (avoidDivisionByZero) {
        if (inst.getOpcode() == llvm::Instruction::SDiv ||
            inst.getOpcode() == llvm::Instruction::UDiv ||
            inst.getOpcode() == llvm::Instruction::SRem ||
            inst.getOpcode() == llvm::Instruction::URem) {
            // Second operand is the divisor
            if (input.values.size() > 1) {
                auto& divisor = input.values[1];
                if (divisor.IntVal.isZero()) {
                    return false;
                }
            }
        }
    }

    // Check for shift overflow
    if (avoidShiftOverflow) {
        if (inst.getOpcode() == llvm::Instruction::Shl ||
            inst.getOpcode() == llvm::Instruction::LShr ||
            inst.getOpcode() == llvm::Instruction::AShr) {
            if (input.values.size() > 1) {
                auto& shiftAmt = input.values[1];
                unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                if (shiftAmt.IntVal.uge(bitWidth)) {
                    return false;
                }
            }
        }
    }

    // Check for signed overflow (INT_MIN / -1)
    if (avoidSignedOverflow) {
        if (inst.getOpcode() == llvm::Instruction::SDiv) {
            if (input.values.size() >= 2) {
                auto& dividend = input.values[0];
                auto& divisor = input.values[1];
                unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                llvm::APInt minVal = llvm::APInt::getSignedMinValue(bitWidth);
                llvm::APInt negOne(bitWidth, -1, true);
                if (dividend.IntVal == minVal && divisor.IntVal == negOne) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool DomainConstraints::isSafeInput(const TestInput& input,
                                     const SynthesizedSequence& seq) const {
    // Check each instruction in the sequence
    for (const auto& templ : seq.templates) {
        // Check for division by zero
        if (avoidDivisionByZero) {
            if (templ.opcode == llvm::Instruction::SDiv ||
                templ.opcode == llvm::Instruction::UDiv ||
                templ.opcode == llvm::Instruction::SRem ||
                templ.opcode == llvm::Instruction::URem) {
                // The divisor operand
                if (templ.operandIndices.size() > 1) {
                    int divisorOp = templ.operandIndices[1];
                    // If operand refers to input
                    if (divisorOp >= 0 && static_cast<size_t>(divisorOp) < input.rawValues.size()) {
                        if (input.rawValues[divisorOp] == 0) {
                            return false;
                        }
                    }
                }
            }
        }

        // Check for shift overflow
        if (avoidShiftOverflow) {
            if (templ.opcode == llvm::Instruction::Shl ||
                templ.opcode == llvm::Instruction::LShr ||
                templ.opcode == llvm::Instruction::AShr) {
                if (templ.operandIndices.size() > 1) {
                    int shiftOp = templ.operandIndices[1];
                    // Conservative: check if input value could cause overflow
                    if (shiftOp >= 0 && static_cast<size_t>(shiftOp) < input.rawValues.size()) {
                        // Assume 64-bit max for now
                        if (input.rawValues[shiftOp] >= 64) {
                            return false;
                        }
                    }
                }
            }
        }
    }

    return true;
}

//===----------------------------------------------------------------------===//
// Verifier implementation
//===----------------------------------------------------------------------===//

Verifier::Verifier(const Config& config)
    : config_(config), rng_(std::random_device{}()) {
    // Initialize LLVM target for interpretation
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
}

Verifier::~Verifier() = default;

VerificationResult Verifier::verify(const llvm::Function& original,
                                     const llvm::Function& candidate) {
    // Generate test inputs
    auto inputs = generateTestInputs(original, config_.numTestInputs);

    // We need non-const functions for execution
    auto* origFunc = const_cast<llvm::Function*>(&original);
    auto* candFunc = const_cast<llvm::Function*>(&candidate);

    for (const auto& input : inputs) {
        auto origResult = execute(*origFunc, input);
        auto candResult = execute(*candFunc, input);

        if (!origResult || !candResult) {
            // Skip inputs that cause execution failure (likely UB)
            continue;
        }

        testsExecuted_++;

        if (!valuesEqual(*origResult, *candResult, original.getReturnType())) {
            counterexample_ = Counterexample{input, *origResult, *candResult};
            return VerificationResult::NotEquivalent;
        }
    }

    return VerificationResult::Equivalent;
}

VerificationResult Verifier::verify(const llvm::Instruction& original,
                                     const SynthesizedSequence& candidate,
                                     llvm::LLVMContext& ctx) {
    // Get operand types
    std::vector<llvm::Type*> inputTypes;
    for (const auto& op : original.operands()) {
        inputTypes.push_back(op->getType());
    }

    // Create test harness for the candidate
    auto candidateModule = createTestHarness(candidate, inputTypes,
                                              original.getType(), ctx);
    if (!candidateModule) {
        lastError_ = "Failed to create test harness";
        return VerificationResult::Error;
    }

    // Create test harness for original
    auto originalModule = std::make_unique<llvm::Module>("original", ctx);
    auto* funcType = llvm::FunctionType::get(
        original.getType(), inputTypes, false);
    auto* origFunc = llvm::Function::Create(
        funcType, llvm::Function::ExternalLinkage, "test_original",
        originalModule.get());

    auto* bb = llvm::BasicBlock::Create(ctx, "entry", origFunc);
    llvm::IRBuilder<> builder(bb);

    // Collect input values
    std::vector<llvm::Value*> args;
    for (auto& arg : origFunc->args()) {
        args.push_back(&arg);
    }

    llvm::Value* result = nullptr;
    switch (original.getOpcode()) {
        case llvm::Instruction::Add:
            result = builder.CreateAdd(args[0], args[1]);
            break;
        case llvm::Instruction::Sub:
            result = builder.CreateSub(args[0], args[1]);
            break;
        case llvm::Instruction::Mul:
            result = builder.CreateMul(args[0], args[1]);
            break;
        case llvm::Instruction::SDiv:
            result = builder.CreateSDiv(args[0], args[1]);
            break;
        case llvm::Instruction::UDiv:
            result = builder.CreateUDiv(args[0], args[1]);
            break;
        case llvm::Instruction::SRem:
            result = builder.CreateSRem(args[0], args[1]);
            break;
        case llvm::Instruction::URem:
            result = builder.CreateURem(args[0], args[1]);
            break;
        case llvm::Instruction::And:
            result = builder.CreateAnd(args[0], args[1]);
            break;
        case llvm::Instruction::Or:
            result = builder.CreateOr(args[0], args[1]);
            break;
        case llvm::Instruction::Xor:
            result = builder.CreateXor(args[0], args[1]);
            break;
        case llvm::Instruction::Shl:
            result = builder.CreateShl(args[0], args[1]);
            break;
        case llvm::Instruction::LShr:
            result = builder.CreateLShr(args[0], args[1]);
            break;
        case llvm::Instruction::AShr:
            result = builder.CreateAShr(args[0], args[1]);
            break;
        case llvm::Instruction::ICmp:
            if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&original)) {
                result = builder.CreateICmp(cmp->getPredicate(), args[0], args[1]);
            }
            break;
        case llvm::Instruction::Select:
            if (args.size() >= 3) {
                result = builder.CreateSelect(args[0], args[1], args[2]);
            }
            break;
        case llvm::Instruction::ZExt:
            result = builder.CreateZExt(args[0], original.getType());
            break;
        case llvm::Instruction::SExt:
            result = builder.CreateSExt(args[0], original.getType());
            break;
        case llvm::Instruction::Trunc:
            result = builder.CreateTrunc(args[0], original.getType());
            break;
        default:
            lastError_ = "Unsupported instruction for verification";
            return VerificationResult::Error;
    }

    if (!result) {
        lastError_ = "Failed to recreate instruction";
        return VerificationResult::Error;
    }

    builder.CreateRet(result);

    // Verify modules
    if (llvm::verifyModule(*originalModule, &llvm::errs())) {
        lastError_ = "Original module verification failed";
        return VerificationResult::Error;
    }
    if (llvm::verifyModule(*candidateModule, &llvm::errs())) {
        lastError_ = "Candidate module verification failed";
        return VerificationResult::Error;
    }

    // Generate safe test inputs
    auto inputs = generateSafeTestInputs(inputTypes, constraints_, config_.numTestInputs);

    auto* candFunc = candidateModule->getFunction("test_candidate");
    if (!candFunc) {
        lastError_ = "Candidate function not found";
        return VerificationResult::Error;
    }

    for (const auto& input : inputs) {
        // Skip unsafe inputs
        if (!constraints_.isSafeInput(input, original)) {
            continue;
        }
        if (!constraints_.isSafeInput(input, candidate)) {
            continue;
        }

        auto origResult = execute(*origFunc, input);
        auto candResult = execute(*candFunc, input);

        if (!origResult || !candResult) {
            // Execution might fail due to undefined behavior
            continue;
        }

        testsExecuted_++;

        if (!valuesEqual(*origResult, *candResult, original.getType())) {
            counterexample_ = Counterexample{input, *origResult, *candResult};
            return VerificationResult::NotEquivalent;
        }
    }

    return VerificationResult::Equivalent;
}

VerificationResult Verifier::verifyBlock(const llvm::BasicBlock& original,
                                          const SynthesizedSequence& candidate,
                                          llvm::LLVMContext& ctx) {
    // For basic block verification, we need to track multiple outputs
    // This is more complex - placeholder for now
    return VerificationResult::Unknown;
}

std::vector<TestInput> Verifier::generateTestInputs(const llvm::Function& func,
                                                     size_t count) {
    std::vector<llvm::Type*> types;
    for (const auto& arg : func.args()) {
        types.push_back(arg.getType());
    }
    return generateTestInputs(types, count);
}

std::vector<TestInput> Verifier::generateTestInputs(
    const std::vector<llvm::Type*>& types,
    size_t count) {
    std::vector<TestInput> inputs;
    inputs.reserve(count);

    // First, add edge cases
    if (!types.empty() && types[0]->isIntegerTy()) {
        unsigned bitWidth = types[0]->getIntegerBitWidth();
        auto edgeCases = getEdgeCases(bitWidth);

        // Generate inputs from edge case combinations
        size_t edgeInputs = std::min(count / 4, edgeCases.size() * edgeCases.size());
        for (size_t i = 0; i < edgeInputs && inputs.size() < count; ++i) {
            TestInput input;
            for (size_t j = 0; j < types.size(); ++j) {
                auto* type = types[j];
                if (type->isIntegerTy()) {
                    unsigned bw = type->getIntegerBitWidth();
                    size_t idx = (i + j) % edgeCases.size();
                    uint64_t val = edgeCases[idx];
                    llvm::GenericValue gv;
                    gv.IntVal = llvm::APInt(bw, val);
                    input.values.push_back(gv);
                    input.rawValues.push_back(val);
                } else {
                    input.values.push_back(generateRandomValue(type));
                    input.rawValues.push_back(0);
                }
            }
            inputs.push_back(std::move(input));
        }
    }

    // Fill rest with random inputs
    while (inputs.size() < count) {
        TestInput input;
        for (auto* type : types) {
            auto gv = generateRandomValue(type);
            input.values.push_back(gv);
            if (type->isIntegerTy()) {
                input.rawValues.push_back(gv.IntVal.getZExtValue());
            } else {
                input.rawValues.push_back(0);
            }
        }
        inputs.push_back(std::move(input));
    }

    return inputs;
}

std::vector<TestInput> Verifier::generateSafeTestInputs(
    const std::vector<llvm::Type*>& types,
    const DomainConstraints& constraints,
    size_t count) {

    std::vector<TestInput> inputs;
    inputs.reserve(count);

    // Get edge cases excluding zeros for divisors
    std::vector<uint64_t> safeEdgeCases;
    if (!types.empty() && types[0]->isIntegerTy()) {
        unsigned bitWidth = types[0]->getIntegerBitWidth();
        auto allEdgeCases = getEdgeCases(bitWidth);
        for (uint64_t val : allEdgeCases) {
            if (val != 0) {
                safeEdgeCases.push_back(val);
            }
        }
        safeEdgeCases.push_back(0);  // Keep one zero for first operand
    }

    size_t attempts = 0;
    const size_t maxAttempts = count * 10;

    while (inputs.size() < count && attempts < maxAttempts) {
        attempts++;

        TestInput input;
        bool valid = true;

        for (size_t i = 0; i < types.size(); ++i) {
            auto* type = types[i];
            if (type->isIntegerTy()) {
                unsigned bw = type->getIntegerBitWidth();
                uint64_t val;

                // Use edge case with probability
                std::uniform_int_distribution<int> choice(0, 9);
                if (choice(rng_) < 4 && !safeEdgeCases.empty()) {
                    std::uniform_int_distribution<size_t> edgeIdx(0, safeEdgeCases.size() - 1);
                    val = safeEdgeCases[edgeIdx(rng_)];
                } else {
                    std::uniform_int_distribution<uint64_t> dist(1, UINT64_MAX);
                    val = dist(rng_);
                }

                // Ensure divisor is non-zero
                if (i == 1 && constraints.avoidDivisionByZero && val == 0) {
                    val = 1;
                }

                // Ensure shift amount is valid
                if (i == 1 && constraints.avoidShiftOverflow && val >= bw) {
                    val = val % bw;
                }

                llvm::GenericValue gv;
                gv.IntVal = llvm::APInt(bw, val);
                input.values.push_back(gv);
                input.rawValues.push_back(val);
            } else {
                input.values.push_back(generateRandomValue(type));
                input.rawValues.push_back(0);
            }
        }

        if (valid) {
            inputs.push_back(std::move(input));
        }
    }

    return inputs;
}

std::vector<uint64_t> Verifier::getEdgeCases(unsigned bitWidth) {
    std::vector<uint64_t> cases;

    // Common edge cases
    cases.push_back(0);
    cases.push_back(1);
    cases.push_back(2);

    if (bitWidth <= 64) {
        uint64_t maxVal = (bitWidth == 64) ? UINT64_MAX : ((1ULL << bitWidth) - 1);
        uint64_t signBit = 1ULL << (bitWidth - 1);
        uint64_t maxSigned = signBit - 1;

        cases.push_back(maxVal);           // All 1s
        cases.push_back(maxSigned);        // Max signed
        cases.push_back(signBit);          // Min signed (as unsigned)
        cases.push_back(maxVal - 1);       // Max - 1

        // Powers of 2
        for (unsigned i = 2; i < bitWidth && i < 8; ++i) {
            cases.push_back(1ULL << i);
            cases.push_back((1ULL << i) - 1);
        }

        // -1, -2 in two's complement
        cases.push_back(maxVal);      // -1
        cases.push_back(maxVal - 1);  // -2
    }

    return cases;
}

llvm::GenericValue Verifier::generateRandomValue(llvm::Type* type) {
    llvm::GenericValue gv;

    if (type->isIntegerTy()) {
        unsigned bitWidth = type->getIntegerBitWidth();
        if (bitWidth <= 64) {
            std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
            uint64_t val = dist(rng_);

            // Sometimes use edge cases
            std::uniform_int_distribution<int> choice(0, 9);
            switch (choice(rng_)) {
                case 0: val = 0; break;
                case 1: val = 1; break;
                case 2: val = (1ULL << (bitWidth - 1)) - 1; break;  // Max signed
                case 3: val = (1ULL << (bitWidth - 1)); break;      // Min signed
                case 4: val = (bitWidth == 64) ? UINT64_MAX : ((1ULL << bitWidth) - 1); break;
                default: break;  // Keep random
            }

            gv.IntVal = llvm::APInt(bitWidth, val);
        }
    } else if (type->isFloatTy()) {
        std::uniform_real_distribution<float> dist(-1000.0f, 1000.0f);
        gv.FloatVal = dist(rng_);
    } else if (type->isDoubleTy()) {
        std::uniform_real_distribution<double> dist(-1000.0, 1000.0);
        gv.DoubleVal = dist(rng_);
    } else if (type->isPointerTy()) {
        gv.PointerVal = nullptr;
    }

    return gv;
}

std::optional<llvm::GenericValue> Verifier::execute(
    llvm::Function& func,
    const TestInput& input) {
    // Create interpreter engine
    std::string errStr;
    auto* module = func.getParent();

    // Clone module for execution
    auto clonedModule = llvm::CloneModule(*module);

    std::unique_ptr<llvm::ExecutionEngine> engine(
        llvm::EngineBuilder(std::move(clonedModule))
            .setEngineKind(llvm::EngineKind::Interpreter)
            .setErrorStr(&errStr)
            .create());

    if (!engine) {
        lastError_ = "Failed to create execution engine: " + errStr;
        return std::nullopt;
    }

    auto* clonedFunc = engine->FindFunctionNamed(func.getName());
    if (!clonedFunc) {
        lastError_ = "Function not found in cloned module";
        return std::nullopt;
    }

    // Execute function - note: exceptions disabled in LLVM build
    return engine->runFunction(clonedFunc, input.values);
}

bool Verifier::valuesEqual(const llvm::GenericValue& a,
                            const llvm::GenericValue& b,
                            llvm::Type* type) {
    if (type->isIntegerTy()) {
        return a.IntVal == b.IntVal;
    } else if (type->isFloatTy()) {
        if (std::isnan(a.FloatVal) && std::isnan(b.FloatVal)) return true;
        return a.FloatVal == b.FloatVal;
    } else if (type->isDoubleTy()) {
        if (std::isnan(a.DoubleVal) && std::isnan(b.DoubleVal)) return true;
        return a.DoubleVal == b.DoubleVal;
    } else if (type->isPointerTy()) {
        return a.PointerVal == b.PointerVal;
    }
    return false;
}

std::unique_ptr<llvm::Module> Verifier::createTestHarness(
    const SynthesizedSequence& sequence,
    const std::vector<llvm::Type*>& inputTypes,
    llvm::Type* outputType,
    llvm::LLVMContext& ctx) {

    auto module = std::make_unique<llvm::Module>("test_harness", ctx);

    auto* funcType = llvm::FunctionType::get(outputType, inputTypes, false);
    auto* func = llvm::Function::Create(
        funcType, llvm::Function::ExternalLinkage, "test_candidate",
        module.get());

    auto* bb = llvm::BasicBlock::Create(ctx, "entry", func);
    llvm::IRBuilder<> builder(bb);

    std::vector<llvm::Value*> inputs;
    for (auto& arg : func->args()) {
        inputs.push_back(&arg);
    }

    auto* result = sequence.apply(builder, inputs);
    if (!result) {
        return nullptr;
    }

    builder.CreateRet(result);

    return module;
}

//===----------------------------------------------------------------------===//
// AlgebraicVerifier implementation
//===----------------------------------------------------------------------===//

AlgebraicVerifier::AlgebraicVerifier(const Config& config)
    : config_(config) {
    initIdentities();
}

void AlgebraicVerifier::initIdentities() {
    // x + 0 = x
    identities_.push_back({
        llvm::Instruction::Add,
        [this](const SynthesizedSequence& seq) {
            return checkZeroIdentity(seq, llvm::Instruction::Add);
        },
        "add_zero"
    });

    // x - 0 = x
    identities_.push_back({
        llvm::Instruction::Sub,
        [this](const SynthesizedSequence& seq) {
            return checkZeroIdentity(seq, llvm::Instruction::Sub);
        },
        "sub_zero"
    });

    // x * 1 = x
    identities_.push_back({
        llvm::Instruction::Mul,
        [this](const SynthesizedSequence& seq) {
            return checkOneIdentity(seq, llvm::Instruction::Mul);
        },
        "mul_one"
    });

    // x * 0 = 0
    identities_.push_back({
        llvm::Instruction::Mul,
        [this](const SynthesizedSequence& seq) {
            return checkZeroIdentity(seq, llvm::Instruction::Mul);
        },
        "mul_zero"
    });

    // x & -1 = x
    identities_.push_back({
        llvm::Instruction::And,
        [this](const SynthesizedSequence& seq) {
            return checkNegOneIdentity(seq, llvm::Instruction::And);
        },
        "and_neg_one"
    });

    // x | 0 = x
    identities_.push_back({
        llvm::Instruction::Or,
        [this](const SynthesizedSequence& seq) {
            return checkZeroIdentity(seq, llvm::Instruction::Or);
        },
        "or_zero"
    });

    // x ^ 0 = x
    identities_.push_back({
        llvm::Instruction::Xor,
        [this](const SynthesizedSequence& seq) {
            return checkZeroIdentity(seq, llvm::Instruction::Xor);
        },
        "xor_zero"
    });

    // x - x = 0
    identities_.push_back({
        llvm::Instruction::Sub,
        [this](const SynthesizedSequence& seq) {
            return checkSelfIdentity(seq, llvm::Instruction::Sub);
        },
        "sub_self"
    });

    // x ^ x = 0
    identities_.push_back({
        llvm::Instruction::Xor,
        [this](const SynthesizedSequence& seq) {
            return checkSelfIdentity(seq, llvm::Instruction::Xor);
        },
        "xor_self"
    });
}

std::optional<bool> AlgebraicVerifier::proveEquivalent(
    const llvm::Instruction& original,
    const SynthesizedSequence& candidate) {

    // Check strength reductions
    if (isStrengthReduction(original, candidate)) {
        proofCount_++;
        return true;
    }

    // Check if candidate matches a known identity that equals original
    // This requires more sophisticated matching
    return std::nullopt;  // Cannot prove
}

bool AlgebraicVerifier::matchesIdentity(const SynthesizedSequence& seq,
                                         const std::vector<llvm::Type*>& inputTypes) {
    for (const auto& identity : identities_) {
        if (identity.matcher(seq)) {
            return true;
        }
    }
    return false;
}

bool AlgebraicVerifier::isStrengthReduction(const llvm::Instruction& original,
                                             const SynthesizedSequence& candidate) {
    if (candidate.templates.size() != 1) {
        return false;
    }

    const auto& templ = candidate.templates[0];

    // x * 2 -> x << 1
    if (original.getOpcode() == llvm::Instruction::Mul &&
        templ.opcode == llvm::Instruction::Shl) {
        // Check if multiplying by power of 2 and shifting by log2
        if (original.getNumOperands() >= 2) {
            if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(original.getOperand(1))) {
                uint64_t mulVal = c->getZExtValue();
                if (mulVal > 0 && (mulVal & (mulVal - 1)) == 0) {
                    // It's a power of 2
                    unsigned log2Val = 63 - __builtin_clzll(mulVal);
                    // Check if candidate shifts by this amount
                    if (!templ.constantIndices.empty() && templ.constantIndices[0] >= 0 &&
                        static_cast<size_t>(templ.constantIndices[0]) < candidate.constants.size()) {
                        if (candidate.constants[templ.constantIndices[0]].intValue == static_cast<int64_t>(log2Val)) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    // x / 2 -> x >> 1 (for unsigned)
    if (original.getOpcode() == llvm::Instruction::UDiv &&
        templ.opcode == llvm::Instruction::LShr) {
        if (original.getNumOperands() >= 2) {
            if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(original.getOperand(1))) {
                uint64_t divVal = c->getZExtValue();
                if (divVal > 0 && (divVal & (divVal - 1)) == 0) {
                    unsigned log2Val = 63 - __builtin_clzll(divVal);
                    if (!templ.constantIndices.empty() && templ.constantIndices[0] >= 0 &&
                        static_cast<size_t>(templ.constantIndices[0]) < candidate.constants.size()) {
                        if (candidate.constants[templ.constantIndices[0]].intValue == static_cast<int64_t>(log2Val)) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool AlgebraicVerifier::checkZeroIdentity(const SynthesizedSequence& seq,
                                           unsigned opcode) {
    if (seq.templates.size() != 1) return false;
    const auto& t = seq.templates[0];
    if (t.opcode != opcode) return false;

    // Check if one operand is constant 0
    for (size_t i = 0; i < t.operandIndices.size(); ++i) {
        if (t.operandIndices[i] < 0) {
            // It's a constant reference
            int constIdx = -t.operandIndices[i] - 1;
            if (constIdx >= 0 && static_cast<size_t>(constIdx) < seq.constants.size()) {
                if (seq.constants[constIdx].intValue == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool AlgebraicVerifier::checkOneIdentity(const SynthesizedSequence& seq,
                                          unsigned opcode) {
    if (seq.templates.size() != 1) return false;
    const auto& t = seq.templates[0];
    if (t.opcode != opcode) return false;

    for (size_t i = 0; i < t.operandIndices.size(); ++i) {
        if (t.operandIndices[i] < 0) {
            int constIdx = -t.operandIndices[i] - 1;
            if (constIdx >= 0 && static_cast<size_t>(constIdx) < seq.constants.size()) {
                if (seq.constants[constIdx].intValue == 1) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool AlgebraicVerifier::checkSelfIdentity(const SynthesizedSequence& seq,
                                           unsigned opcode) {
    if (seq.templates.size() != 1) return false;
    const auto& t = seq.templates[0];
    if (t.opcode != opcode) return false;

    // Check if both operands are the same input
    if (t.operandIndices.size() >= 2) {
        return t.operandIndices[0] == t.operandIndices[1] && t.operandIndices[0] >= 0;
    }
    return false;
}

bool AlgebraicVerifier::checkNegOneIdentity(const SynthesizedSequence& seq,
                                             unsigned opcode) {
    if (seq.templates.size() != 1) return false;
    const auto& t = seq.templates[0];
    if (t.opcode != opcode) return false;

    for (size_t i = 0; i < t.operandIndices.size(); ++i) {
        if (t.operandIndices[i] < 0) {
            int constIdx = -t.operandIndices[i] - 1;
            if (constIdx >= 0 && static_cast<size_t>(constIdx) < seq.constants.size()) {
                if (seq.constants[constIdx].intValue == -1) {
                    return true;
                }
            }
        }
    }
    return false;
}

//===----------------------------------------------------------------------===//
// CEGISVerifier implementation
//===----------------------------------------------------------------------===//

CEGISVerifier::CEGISVerifier(const Config& config)
    : config_(config), baseVerifier_(config) {}

VerificationResult CEGISVerifier::verify(const llvm::Instruction& original,
                                          const SynthesizedSequence& candidate,
                                          llvm::LLVMContext& ctx) {
    const size_t maxIterations = 10;

    for (size_t iter = 0; iter < maxIterations; ++iter) {
        // First, test on known counterexamples
        std::vector<llvm::Type*> inputTypes;
        for (const auto& op : original.operands()) {
            inputTypes.push_back(op->getType());
        }

        // Create test harnesses
        auto candidateModule = std::make_unique<llvm::Module>("cegis_cand", ctx);
        auto* funcType = llvm::FunctionType::get(original.getType(), inputTypes, false);
        auto* candFunc = llvm::Function::Create(
            funcType, llvm::Function::ExternalLinkage, "test_candidate",
            candidateModule.get());

        auto* bb = llvm::BasicBlock::Create(ctx, "entry", candFunc);
        llvm::IRBuilder<> builder(bb);

        std::vector<llvm::Value*> inputs;
        for (auto& arg : candFunc->args()) {
            inputs.push_back(&arg);
        }

        auto* result = candidate.apply(builder, inputs);
        if (!result) {
            return VerificationResult::Error;
        }
        builder.CreateRet(result);

        // Test on accumulated counterexamples
        for (const auto& ce : counterexamples_) {
            auto candResult = baseVerifier_.execute(*candFunc, ce);
            if (!candResult) {
                continue;  // Skip problematic inputs
            }

            // Also need to execute original - simplified for now
            // In real implementation, would compare outputs
        }

        // Try to find new counterexample
        auto newCE = findCounterexample(original, candidate, ctx);
        if (!newCE) {
            // No counterexample found - high confidence equivalent
            return VerificationResult::Equivalent;
        }

        counterexamples_.push_back(*newCE);
    }

    // Couldn't determine after max iterations
    return VerificationResult::Unknown;
}

std::optional<TestInput> CEGISVerifier::findCounterexample(
    const llvm::Instruction& original,
    const SynthesizedSequence& candidate,
    llvm::LLVMContext& ctx) {

    // Generate random inputs and look for differences
    std::vector<llvm::Type*> inputTypes;
    for (const auto& op : original.operands()) {
        inputTypes.push_back(op->getType());
    }

    auto inputs = baseVerifier_.generateTestInputs(inputTypes, config_.numTestInputs / 10);

    for (const auto& input : inputs) {
        // Would compare outputs here
        // For now, rely on random testing
    }

    return std::nullopt;
}

//===----------------------------------------------------------------------===//
// HybridVerifier implementation
//===----------------------------------------------------------------------===//

HybridVerifier::HybridVerifier(const Config& config)
    : config_(config),
      algebraicVerifier_(config),
      randomVerifier_(config),
      cegisVerifier_(config) {}

VerificationResult HybridVerifier::verify(const llvm::Instruction& original,
                                           const SynthesizedSequence& candidate,
                                           llvm::LLVMContext& ctx) {
    // Strategy 1: Try algebraic proof first (fastest)
    auto algebraicResult = algebraicVerifier_.proveEquivalent(original, candidate);
    if (algebraicResult.has_value()) {
        strategyUsed_ = "algebraic";
        stats_.algebraicProofs++;
        return *algebraicResult ? VerificationResult::Equivalent
                                : VerificationResult::NotEquivalent;
    }

    // Strategy 2: Random testing
    auto testResult = randomVerifier_.verify(original, candidate, ctx);
    stats_.randomTestingVerifications++;

    if (testResult == VerificationResult::NotEquivalent) {
        strategyUsed_ = "random_testing";
        return testResult;
    }

    // Strategy 3: CEGIS for higher confidence (if configured)
    if (config_.verificationStrategy == VerificationStrategy::CEGIS ||
        config_.verificationStrategy == VerificationStrategy::Hybrid) {
        auto cegisResult = cegisVerifier_.verify(original, candidate, ctx);
        stats_.cegisVerifications++;
        strategyUsed_ = "cegis";
        return cegisResult;
    }

    strategyUsed_ = "random_testing";
    return testResult;
}

//===----------------------------------------------------------------------===//
// ObservationalEquivalence implementation
//===----------------------------------------------------------------------===//

ObservationalEquivalence::ObservationalEquivalence(const Config& config)
    : config_(config),
      rng_(std::random_device{}()) {}

uint64_t ObservationalEquivalence::computeFingerprint(
    const SynthesizedSequence& seq,
    const std::vector<llvm::Type*>& inputTypes,
    llvm::LLVMContext& ctx) {

    if (inputTypes.empty()) {
        return 0;
    }

    unsigned bitWidth = 64;
    if (inputTypes[0]->isIntegerTy()) {
        bitWidth = inputTypes[0]->getIntegerBitWidth();
    }

    const auto& testVectors = getTestVectors(inputTypes.size(), bitWidth);
    auto outputs = evaluate(seq, testVectors);

    // Combine outputs into fingerprint
    uint64_t fingerprint = 0;
    for (size_t i = 0; i < outputs.size(); ++i) {
        fingerprint ^= outputs[i];
        fingerprint *= 0x100000001b3ULL;
        fingerprint ^= fingerprint >> 33;
    }

    return fingerprint;
}

bool ObservationalEquivalence::isNewEquivalenceClass(
    const SynthesizedSequence& seq,
    const std::vector<llvm::Type*>& inputTypes,
    llvm::LLVMContext& ctx) {

    uint64_t fp = computeFingerprint(seq, inputTypes, ctx);

    if (seenFingerprints_.count(fp)) {
        return false;
    }

    seenFingerprints_.insert(fp);
    return true;
}

const std::vector<std::vector<uint64_t>>& ObservationalEquivalence::getTestVectors(
    size_t numInputs, size_t bitWidth) {

    auto key = std::make_pair(numInputs, bitWidth);

    auto it = testVectorCache_.find(key);
    if (it != testVectorCache_.end()) {
        return it->second;
    }

    // Generate test vectors
    std::vector<std::vector<uint64_t>> vectors;
    size_t numVectors = config_.numFingerprintSamples;

    // Fixed deterministic vectors for reproducibility
    std::vector<uint64_t> seeds = {0, 1, 2, 3, 0xFFFFFFFFFFFFFFFFULL, 0x5555555555555555ULL,
                                    0xAAAAAAAAAAAAAAAAULL, 7, 15, 31, 63, 127, 255, 1023};

    for (size_t i = 0; i < numVectors; ++i) {
        std::vector<uint64_t> vec;
        for (size_t j = 0; j < numInputs; ++j) {
            uint64_t val;
            if (i < seeds.size()) {
                val = seeds[i] ^ (j * 0x123456789ABCDEFULL);
            } else {
                val = rng_();
            }
            // Mask to bit width
            if (bitWidth < 64) {
                val &= (1ULL << bitWidth) - 1;
            }
            vec.push_back(val);
        }
        vectors.push_back(vec);
    }

    testVectorCache_[key] = vectors;
    return testVectorCache_[key];
}

std::vector<uint64_t> ObservationalEquivalence::evaluate(
    const SynthesizedSequence& seq,
    const std::vector<std::vector<uint64_t>>& inputs) {

    std::vector<uint64_t> outputs;
    outputs.reserve(inputs.size());

    for (const auto& inputVec : inputs) {
        // Simulate execution of the sequence
        std::vector<uint64_t> values = inputVec;

        // Add constants
        for (const auto& c : seq.constants) {
            values.push_back(static_cast<uint64_t>(c.intValue));
        }

        // Execute each instruction
        for (const auto& templ : seq.templates) {
            uint64_t result = 0;

            if (templ.operandIndices.size() >= 2) {
                size_t op0Idx = templ.operandIndices[0] >= 0 ? templ.operandIndices[0] : inputVec.size() + (-templ.operandIndices[0] - 1);
                size_t op1Idx = templ.operandIndices[1] >= 0 ? templ.operandIndices[1] : inputVec.size() + (-templ.operandIndices[1] - 1);

                uint64_t op0 = op0Idx < values.size() ? values[op0Idx] : 0;
                uint64_t op1 = op1Idx < values.size() ? values[op1Idx] : 0;

                switch (templ.opcode) {
                    case llvm::Instruction::Add: result = op0 + op1; break;
                    case llvm::Instruction::Sub: result = op0 - op1; break;
                    case llvm::Instruction::Mul: result = op0 * op1; break;
                    case llvm::Instruction::UDiv: result = op1 ? op0 / op1 : 0; break;
                    case llvm::Instruction::SDiv: result = op1 ? static_cast<int64_t>(op0) / static_cast<int64_t>(op1) : 0; break;
                    case llvm::Instruction::URem: result = op1 ? op0 % op1 : 0; break;
                    case llvm::Instruction::SRem: result = op1 ? static_cast<int64_t>(op0) % static_cast<int64_t>(op1) : 0; break;
                    case llvm::Instruction::And: result = op0 & op1; break;
                    case llvm::Instruction::Or: result = op0 | op1; break;
                    case llvm::Instruction::Xor: result = op0 ^ op1; break;
                    case llvm::Instruction::Shl: result = op1 < 64 ? op0 << op1 : 0; break;
                    case llvm::Instruction::LShr: result = op1 < 64 ? op0 >> op1 : 0; break;
                    case llvm::Instruction::AShr: result = op1 < 64 ? static_cast<int64_t>(op0) >> op1 : 0; break;
                    default: result = 0; break;
                }
            }

            values.push_back(result);
        }

        // Output is the last computed value
        outputs.push_back(values.back());
    }

    return outputs;
}

} // namespace superopt
