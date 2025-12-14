#include "superopt/Superoptimizer.h"
#include "superopt/IRLoader.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LLVMContext.h"

#include <iostream>
#include <iomanip>

using namespace llvm;
using namespace superopt;

// Command line options
static cl::opt<std::string> InputFile(
    cl::Positional,
    cl::desc("<input bitcode/IR file>"),
    cl::Required);

static cl::opt<std::string> OutputFile(
    "o",
    cl::desc("Output file (default: stdout for .ll, required for .bc)"),
    cl::value_desc("filename"));

static cl::opt<bool> OutputBitcode(
    "emit-bc",
    cl::desc("Emit bitcode instead of text IR"),
    cl::init(false));

static cl::opt<unsigned> MaxInstructions(
    "max-instructions",
    cl::desc("Maximum instructions in synthesized sequences"),
    cl::init(5));

static cl::opt<unsigned> MaxSearchDepth(
    "max-depth",
    cl::desc("Maximum search depth"),
    cl::init(10));

static cl::opt<unsigned> NumTestInputs(
    "num-tests",
    cl::desc("Number of random test inputs for verification"),
    cl::init(100));

static cl::opt<std::string> FunctionFilter(
    "function",
    cl::desc("Only optimize functions matching this pattern"),
    cl::value_desc("regex"));

static cl::opt<double> MinImprovement(
    "min-improvement",
    cl::desc("Minimum cost improvement ratio to accept (0.0-1.0)"),
    cl::init(0.1));

static cl::opt<bool> Verbose(
    "v",
    cl::desc("Verbose output"),
    cl::init(false));

static cl::opt<bool> Debug(
    "debug",
    cl::desc("Debug output"),
    cl::init(false));

static cl::opt<bool> Stats(
    "stats",
    cl::desc("Print statistics"),
    cl::init(false));

static cl::opt<bool> DryRun(
    "dry-run",
    cl::desc("Don't write output, just report what would be optimized"),
    cl::init(false));

static cl::opt<bool> ShowCost(
    "show-cost",
    cl::desc("Show cost of each function before/after optimization"),
    cl::init(false));

void printBanner() {
    errs() << "╔═══════════════════════════════════════════════════════════════╗\n";
    errs() << "║                  LLVM Bitcode Superoptimizer                  ║\n";
    errs() << "║                        Version 0.1.0                          ║\n";
    errs() << "╚═══════════════════════════════════════════════════════════════╝\n\n";
}

void printStats(const Stats& stats) {
    errs() << "\n=== Superoptimization Statistics ===\n";
    errs() << "  Functions processed: " << stats.functionsProcessed << "\n";
    errs() << "  Functions optimized: " << stats.functionsOptimized << "\n";
    errs() << "  Candidates generated: " << stats.candidatesGenerated << "\n";
    errs() << "  Candidates verified: " << stats.candidatesVerified << "\n";
    errs() << "  Candidates pruned: " << stats.candidatesPruned << "\n";
    errs() << "  Total time: " << std::fixed << std::setprecision(2)
           << stats.totalTimeSeconds << "s\n";
    errs() << "  Total cost reduction: " << std::fixed << std::setprecision(2)
           << stats.totalCostReduction << "\n";
}

int main(int argc, char** argv) {
    InitLLVM X(argc, argv);

    // Initialize LLVM targets
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    cl::ParseCommandLineOptions(argc, argv,
        "LLVM Bitcode Superoptimizer\n\n"
        "  This tool performs superoptimization on LLVM bitcode/IR.\n"
        "  It searches for shorter, more efficient instruction sequences\n"
        "  that compute the same result as the original code.\n");

    if (Verbose) {
        printBanner();
    }

    // Create LLVM context and load input
    LLVMContext context;
    IRLoader loader(context);

    if (Verbose) {
        errs() << "Loading: " << InputFile << "\n";
    }

    auto module = loader.load(InputFile);
    if (!module) {
        errs() << "Error: " << loader.getLastError() << "\n";
        return 1;
    }

    if (Verbose) {
        errs() << "Loaded module: " << module->getName() << "\n";
        errs() << "  Functions: " << module->size() << "\n";
    }

    // Configure superoptimizer
    Config config;
    config.maxInstructions = MaxInstructions;
    config.maxSearchDepth = MaxSearchDepth;
    config.numTestInputs = NumTestInputs;
    config.functionFilter = FunctionFilter;
    config.minCostImprovement = MinImprovement;
    config.debug = Debug;

    // Show initial costs if requested
    if (ShowCost) {
        errs() << "\n=== Initial Function Costs ===\n";
        CostModel costModel;
        for (auto& func : *module) {
            if (!func.isDeclaration()) {
                double cost = costModel.getFunctionCost(func);
                errs() << "  " << func.getName() << ": " << std::fixed
                       << std::setprecision(2) << cost << "\n";
            }
        }
    }

    // Run superoptimizer
    Superoptimizer superopt(config);

    if (Verbose) {
        superopt.setProgressCallback([](const std::string& msg, double progress) {
            errs() << "[" << std::fixed << std::setprecision(0)
                   << (progress * 100) << "%] " << msg << "\n";
        });
    }

    if (Verbose) {
        errs() << "\nRunning superoptimization...\n";
    }

    bool changed = superopt.optimize(*module);

    // Show final costs if requested
    if (ShowCost) {
        errs() << "\n=== Final Function Costs ===\n";
        CostModel costModel;
        for (auto& func : *module) {
            if (!func.isDeclaration()) {
                double cost = costModel.getFunctionCost(func);
                errs() << "  " << func.getName() << ": " << std::fixed
                       << std::setprecision(2) << cost << "\n";
            }
        }
    }

    // Print statistics
    if (Stats || Verbose) {
        printStats(superopt.getStats());
    }

    // Output result
    if (!DryRun) {
        if (OutputBitcode) {
            if (OutputFile.empty()) {
                errs() << "Error: Output file required for bitcode output\n";
                return 1;
            }
            if (!loader.saveBitcode(*module, OutputFile)) {
                errs() << "Error: " << loader.getLastError() << "\n";
                return 1;
            }
            if (Verbose) {
                errs() << "\nWrote bitcode to: " << OutputFile << "\n";
            }
        } else {
            if (OutputFile.empty()) {
                // Print to stdout
                module->print(outs(), nullptr);
            } else {
                if (!loader.saveIR(*module, OutputFile)) {
                    errs() << "Error: " << loader.getLastError() << "\n";
                    return 1;
                }
                if (Verbose) {
                    errs() << "\nWrote IR to: " << OutputFile << "\n";
                }
            }
        }
    } else {
        if (changed) {
            errs() << "\n[Dry run] Would have optimized the module.\n";
        } else {
            errs() << "\n[Dry run] No optimizations found.\n";
        }
    }

    if (Verbose) {
        errs() << "\nDone.\n";
    }

    return 0;
}
