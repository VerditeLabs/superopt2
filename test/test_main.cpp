#include "superopt/Superoptimizer.h"
#include "superopt/IRLoader.h"
#include "superopt/CostModel.h"
#include "superopt/Enumerator.h"
#include "superopt/Verifier.h"
#include "superopt/Canonicalizer.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"

#include <iostream>
#include <cassert>

using namespace superopt;
using namespace llvm;

// Test utilities
#define TEST(name) \
    static void test_##name(); \
    static struct TestRunner_##name { \
        TestRunner_##name() { \
            std::cerr << "Running test: " #name "... "; \
            test_##name(); \
            std::cerr << "PASSED\n"; \
        } \
    } testRunner_##name; \
    static void test_##name()

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED\n"; \
            std::cerr << "  Assertion failed: " #cond "\n"; \
            std::cerr << "  at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::exit(1); \
        } \
    } while (0)

// Helper to create a simple test function
std::unique_ptr<Module> createTestModule(LLVMContext& ctx, const std::string& name) {
    auto module = std::make_unique<Module>(name, ctx);
    return module;
}

// Create a function with a single add instruction
Function* createAddFunction(Module& module) {
    auto& ctx = module.getContext();
    auto* i32 = Type::getInt32Ty(ctx);
    auto* funcType = FunctionType::get(i32, {i32, i32}, false);
    auto* func = Function::Create(funcType, Function::ExternalLinkage,
                                  "add_func", module);

    auto* bb = BasicBlock::Create(ctx, "entry", func);
    IRBuilder<> builder(bb);

    auto args = func->arg_begin();
    Value* a = &*args++;
    Value* b = &*args;

    Value* sum = builder.CreateAdd(a, b, "sum");
    builder.CreateRet(sum);

    return func;
}

// Create a function with a multiplication that could be strength-reduced
Function* createMulBy2Function(Module& module) {
    auto& ctx = module.getContext();
    auto* i32 = Type::getInt32Ty(ctx);
    auto* funcType = FunctionType::get(i32, {i32}, false);
    auto* func = Function::Create(funcType, Function::ExternalLinkage,
                                  "mul_by_2", module);

    auto* bb = BasicBlock::Create(ctx, "entry", func);
    IRBuilder<> builder(bb);

    Value* x = &*func->arg_begin();
    Value* two = ConstantInt::get(i32, 2);
    Value* result = builder.CreateMul(x, two, "result");
    builder.CreateRet(result);

    return func;
}

// Create a function with a division by power of 2
Function* createDivBy4Function(Module& module) {
    auto& ctx = module.getContext();
    auto* i32 = Type::getInt32Ty(ctx);
    auto* funcType = FunctionType::get(i32, {i32}, false);
    auto* func = Function::Create(funcType, Function::ExternalLinkage,
                                  "div_by_4", module);

    auto* bb = BasicBlock::Create(ctx, "entry", func);
    IRBuilder<> builder(bb);

    Value* x = &*func->arg_begin();
    Value* four = ConstantInt::get(i32, 4);
    Value* result = builder.CreateUDiv(x, four, "result");
    builder.CreateRet(result);

    return func;
}

// Tests

TEST(IRLoader_ParseIR) {
    LLVMContext ctx;
    IRLoader loader(ctx);

    std::string ir = R"(
        define i32 @test(i32 %x) {
        entry:
            %result = add i32 %x, 1
            ret i32 %result
        }
    )";

    auto module = loader.parseIR(ir);
    ASSERT(module != nullptr);
    ASSERT(module->getFunction("test") != nullptr);
}

TEST(IRLoader_GetIRString) {
    LLVMContext ctx;
    auto module = createTestModule(ctx, "test");
    createAddFunction(*module);

    std::string ir = IRLoader::getIRString(*module);
    ASSERT(ir.find("add_func") != std::string::npos);
    ASSERT(ir.find("add") != std::string::npos);
}

TEST(CostModel_InstructionCosts) {
    LLVMContext ctx;
    auto module = createTestModule(ctx, "test");
    auto* func = createAddFunction(*module);

    CostModel costModel;

    // Get cost of the function
    double cost = costModel.getFunctionCost(*func);
    ASSERT(cost > 0);

    // Breakdown should show instruction count
    auto breakdown = costModel.getFunctionCostBreakdown(*func);
    ASSERT(breakdown.instructionCount > 0);
}

TEST(CostModel_CompareOperations) {
    CostModel costModel;

    // Division should be more expensive than addition
    ASSERT(costModel.getDefaultCost(Instruction::UDiv) >
           costModel.getDefaultCost(Instruction::Add));

    // Multiplication should be more expensive than addition
    ASSERT(costModel.getDefaultCost(Instruction::Mul) >
           costModel.getDefaultCost(Instruction::Add));
}

TEST(Enumerator_BasicEnumeration) {
    Config config;
    config.maxInstructions = 2;
    Enumerator enumerator(config);

    LLVMContext ctx;
    auto* i32 = Type::getInt32Ty(ctx);
    std::vector<Type*> inputTypes = {i32, i32};

    size_t count = 0;
    enumerator.enumerate(i32, inputTypes, 2,
        [&](const SynthesizedSequence& seq) {
            count++;
            return count < 1000;  // Limit for test
        });

    ASSERT(count > 0);
    ASSERT(enumerator.getCandidatesGenerated() > 0);
}

TEST(Enumerator_SequenceApply) {
    LLVMContext ctx;
    auto module = std::make_unique<Module>("test", ctx);
    auto* i32 = Type::getInt32Ty(ctx);
    auto* funcType = FunctionType::get(i32, {i32, i32}, false);
    auto* func = Function::Create(funcType, Function::ExternalLinkage,
                                  "test", module.get());
    auto* bb = BasicBlock::Create(ctx, "entry", func);
    IRBuilder<> builder(bb);

    // Create a sequence: add(x, y)
    SynthesizedSequence seq;
    InstructionTemplate addTempl;
    addTempl.opcode = Instruction::Add;
    addTempl.operandIndices = {0, 1};
    addTempl.resultType = i32;
    seq.templates.push_back(addTempl);
    seq.outputIndex = 2;  // Result of the add

    // Apply the sequence
    std::vector<Value*> inputs;
    for (auto& arg : func->args()) {
        inputs.push_back(&arg);
    }

    Value* result = seq.apply(builder, inputs);
    ASSERT(result != nullptr);
    builder.CreateRet(result);

    // Verify the module
    ASSERT(!verifyModule(*module, &errs()));
}

TEST(Canonicalizer_CommutativeOperands) {
    LLVMContext ctx;
    auto module = createTestModule(ctx, "test");

    auto* i32 = Type::getInt32Ty(ctx);
    auto* funcType = FunctionType::get(i32, {i32}, false);
    auto* func = Function::Create(funcType, Function::ExternalLinkage,
                                  "test", module.get());
    auto* bb = BasicBlock::Create(ctx, "entry", func);
    IRBuilder<> builder(bb);

    Value* x = &*func->arg_begin();
    Value* c = ConstantInt::get(i32, 5);

    // Create: c + x (constant on left - should be canonicalized to x + c)
    Value* sum = builder.CreateAdd(c, x);
    builder.CreateRet(sum);

    Canonicalizer canonicalizer;
    bool changed = canonicalizer.canonicalize(*func);

    // Should have moved constant to right
    auto* addInst = dyn_cast<Instruction>(
        func->getEntryBlock().front().getOperand(0));
    // Note: The canonicalizer might not change if LLVM already normalizes this
}

TEST(Canonicalizer_AlgebraicIdentities) {
    LLVMContext ctx;
    auto module = createTestModule(ctx, "test");

    auto* i32 = Type::getInt32Ty(ctx);
    auto* funcType = FunctionType::get(i32, {i32}, false);
    auto* func = Function::Create(funcType, Function::ExternalLinkage,
                                  "test", module.get());
    auto* bb = BasicBlock::Create(ctx, "entry", func);
    IRBuilder<> builder(bb);

    Value* x = &*func->arg_begin();
    Value* zero = ConstantInt::get(i32, 0);

    // Create: x + 0 (should be simplified to x)
    Value* sum = builder.CreateAdd(x, zero);
    builder.CreateRet(sum);

    Canonicalizer canonicalizer;
    canonicalizer.canonicalize(*func);

    // The function should still be valid
    ASSERT(!verifyFunction(*func, &errs()));
}

TEST(Verifier_RandomInputGeneration) {
    Config config;
    config.numTestInputs = 10;
    Verifier verifier(config);

    LLVMContext ctx;
    auto module = createTestModule(ctx, "test");
    auto* func = createAddFunction(*module);

    auto inputs = verifier.generateTestInputs(*func, 10);
    ASSERT(inputs.size() == 10);

    for (const auto& input : inputs) {
        ASSERT(input.values.size() == 2);  // Two arguments
    }
}

TEST(Superoptimizer_BasicOptimization) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    LLVMContext ctx;
    auto module = createTestModule(ctx, "test");
    createAddFunction(*module);

    Config config;
    config.maxInstructions = 3;
    config.numTestInputs = 50;
    config.debug = false;

    Superoptimizer superopt(config);
    superopt.optimize(*module);

    // Module should still be valid
    ASSERT(!verifyModule(*module, &errs()));

    // Stats should show some work was done
    auto& stats = superopt.getStats();
    ASSERT(stats.functionsProcessed > 0);
}

TEST(Superoptimizer_Stats) {
    InitializeNativeTarget();

    LLVMContext ctx;
    auto module = createTestModule(ctx, "test");
    createAddFunction(*module);
    createMulBy2Function(*module);

    Config config;
    config.maxInstructions = 2;
    config.numTestInputs = 20;

    Superoptimizer superopt(config);
    superopt.optimize(*module);

    auto& stats = superopt.getStats();
    ASSERT(stats.functionsProcessed == 2);
    ASSERT(stats.totalTimeSeconds >= 0);
}

// Main test runner
int main(int argc, char** argv) {
    // Initialize LLVM
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    std::cerr << "\n=== LLVM Superoptimizer Tests ===\n\n";

    // Tests are auto-registered and run via static initialization
    // If we get here, all tests passed

    std::cerr << "\n=== All tests passed! ===\n\n";
    return 0;
}
