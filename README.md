# LLVM Bitcode Superoptimizer

A superoptimizer for LLVM bitcode that finds optimal instruction sequences through exhaustive enumeration and verification.

## Overview

This superoptimizer takes LLVM bitcode (`.bc`) or LLVM IR (`.ll`) as input and produces optimized LLVM bitcode/IR as output. It works by:

1. **Enumerating** candidate instruction sequences up to a configurable length
2. **Pruning** obviously invalid or suboptimal candidates
3. **Verifying** semantic equivalence using random testing (interpretation)
4. **Selecting** the lowest-cost equivalent sequence

## Building

### Prerequisites

- CMake 3.16+
- LLVM 15+ (with development libraries)
- C++17 compatible compiler

### Build Steps

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Finding LLVM

If LLVM is installed in a non-standard location:

```bash
cmake .. -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
```

## Usage

### Basic Usage

```bash
# Optimize IR and print to stdout
./superopt input.ll

# Optimize and write to file
./superopt input.ll -o output.ll

# Optimize bitcode
./superopt input.bc -o output.bc -emit-bc
```

### Options

```
-o <file>           Output file (stdout if not specified for .ll)
-emit-bc            Emit bitcode instead of text IR
-max-instructions N Maximum instructions in synthesized sequences (default: 5)
-max-depth N        Maximum search depth (default: 10)
-num-tests N        Number of random tests for verification (default: 100)
-function <regex>   Only optimize functions matching pattern
-min-improvement F  Minimum cost improvement ratio (default: 0.1)
-v                  Verbose output
-debug              Debug output
-stats              Print statistics
-dry-run            Don't write output, just report optimizations
-show-cost          Show function costs before/after
```

### Examples

```bash
# Verbose optimization with statistics
./superopt examples/example1.ll -v -stats

# Only optimize specific function
./superopt input.ll -function "^mul_" -o output.ll

# Dry run to see what would be optimized
./superopt input.ll -dry-run -show-cost
```

## Architecture

### Components

- **IRLoader**: Loads and saves LLVM bitcode/IR
- **CostModel**: Estimates instruction costs for ranking candidates
- **Enumerator**: Generates candidate instruction sequences
- **Verifier**: Checks semantic equivalence using interpretation
- **Canonicalizer**: Normalizes IR for better comparison
- **PruningEngine**: Eliminates obviously suboptimal candidates
- **Superoptimizer**: Main orchestration of the optimization process

### How It Works

1. **Pre-optimization**: Run standard LLVM optimizations to clean up IR
2. **Canonicalization**: Normalize instruction ordering and patterns
3. **Instruction-level optimization**: For each instruction:
   - Compute current cost
   - Enumerate candidate replacement sequences
   - Verify each candidate for semantic equivalence
   - Replace with lowest-cost equivalent
4. **Post-optimization**: Run cleanup passes (DCE, etc.)

## Supported Optimizations

The superoptimizer can discover optimizations including:

- **Strength reduction**: `x * 2` → `x + x` or `x << 1`
- **Division to shift**: `x / 4` → `x >> 2` (unsigned)
- **Modulo to mask**: `x % 8` → `x & 7` (unsigned)
- **Algebraic simplification**: `(x + y) - y` → `x`
- **Redundant operation elimination**: `x ^ x` → `0`
- **Bit manipulation patterns**: Various bitwise identities

## Limitations

- Currently focuses on integer arithmetic operations
- Verification uses random testing (not formal verification)
- Search space explodes with larger instruction counts
- No loop optimization (operates on straight-line code)

## Future Work

- [ ] Add SMT-based verification (Z3 integration)
- [ ] Support floating-point operations
- [ ] Add parallel search
- [ ] Implement CEGIS (Counterexample-Guided Inductive Synthesis)
- [ ] Add support for vector operations
- [ ] Implement loop-level superoptimization

## References

- Massalin, "Superoptimizer: A Look at the Smallest Program" (1987)
- Bansal & Aiken, "Automatic Generation of Peephole Superoptimizers" (2006)
- Schkufza et al., "Stochastic Superoptimization" (2013)
- Phothilimthana et al., "Scaling Up Superoptimization" (2016)

## License

MIT License
