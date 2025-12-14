#include "superopt/Verifier.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/ExecutionEngine/Interpreter.h"
#include "llvm/Support/TargetSelect.h"

#include <cstring>

namespace superopt {

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
            lastError_ = "Execution failed";
            return VerificationResult::Error;
        }

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

    // Recreate the original instruction
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

    // Generate test inputs and compare
    auto inputs = generateTestInputs(inputTypes, config_.numTestInputs);

    auto* candFunc = candidateModule->getFunction("test_candidate");
    if (!candFunc) {
        lastError_ = "Candidate function not found";
        return VerificationResult::Error;
    }

    for (const auto& input : inputs) {
        auto origResult = execute(*origFunc, input);
        auto candResult = execute(*candFunc, input);

        if (!origResult || !candResult) {
            // Execution might fail due to undefined behavior (e.g., div by zero)
            // Skip these inputs
            continue;
        }

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
    // Similar to instruction verification but for basic blocks
    // For now, delegate to instruction-level verification
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

    for (size_t i = 0; i < count; ++i) {
        TestInput input;
        for (auto* type : types) {
            input.values.push_back(generateRandomValue(type));
        }
        inputs.push_back(std::move(input));
    }

    return inputs;
}

llvm::GenericValue Verifier::generateRandomValue(llvm::Type* type) {
    llvm::GenericValue gv;

    if (type->isIntegerTy()) {
        unsigned bitWidth = type->getIntegerBitWidth();
        if (bitWidth <= 64) {
            // Mix of random values and edge cases
            std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
            uint64_t val = dist(rng_);

            // Sometimes use edge cases
            std::uniform_int_distribution<int> choice(0, 9);
            switch (choice(rng_)) {
                case 0: val = 0; break;
                case 1: val = 1; break;
                case 2: val = (1ULL << (bitWidth - 1)) - 1; break;  // Max signed
                case 3: val = (1ULL << (bitWidth - 1)); break;      // Min signed (2's comp)
                case 4: val = (1ULL << bitWidth) - 1; break;        // Max unsigned
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
        gv.PointerVal = nullptr;  // Safe default
    }

    return gv;
}

std::optional<llvm::GenericValue> Verifier::execute(
    llvm::Function& func,
    const TestInput& input) {
    // Create interpreter engine
    std::string errStr;
    auto* module = func.getParent();

    // Clone module for execution (interpreter modifies it)
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

    // Find function in cloned module
    auto* clonedFunc = engine->FindFunctionNamed(func.getName());
    if (!clonedFunc) {
        lastError_ = "Function not found in cloned module";
        return std::nullopt;
    }

    // Execute
    try {
        return engine->runFunction(clonedFunc, input.values);
    } catch (...) {
        lastError_ = "Execution threw exception";
        return std::nullopt;
    }
}

bool Verifier::valuesEqual(const llvm::GenericValue& a,
                            const llvm::GenericValue& b,
                            llvm::Type* type) {
    if (type->isIntegerTy()) {
        return a.IntVal == b.IntVal;
    } else if (type->isFloatTy()) {
        // Handle NaN
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

    // Collect input values
    std::vector<llvm::Value*> inputs;
    for (auto& arg : func->args()) {
        inputs.push_back(&arg);
    }

    // Apply the synthesized sequence
    auto* result = sequence.apply(builder, inputs);
    if (!result) {
        return nullptr;
    }

    builder.CreateRet(result);

    return module;
}

// SymbolicVerifier implementation

SymbolicVerifier::SymbolicVerifier(const Config& config) : config_(config) {}

VerificationResult SymbolicVerifier::verify(const llvm::Function& original,
                                             const llvm::Function& candidate) {
    // Abstract interpretation-based verification
    // This is a simplified placeholder - full implementation would track
    // abstract values through the functions
    return VerificationResult::Unknown;
}

bool SymbolicVerifier::checkAlgebraicEquivalence(
    const llvm::Instruction& original,
    const SynthesizedSequence& candidate) {
    // Check common algebraic identities
    // This is a simplified placeholder
    return false;
}

std::pair<int64_t, int64_t> SymbolicVerifier::computeRange(
    const llvm::Instruction& inst) {
    // Compute value range using abstract interpretation
    // Placeholder - returns full range
    return {INT64_MIN, INT64_MAX};
}

} // namespace superopt
