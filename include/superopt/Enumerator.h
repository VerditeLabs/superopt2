#ifndef SUPEROPT_ENUMERATOR_H
#define SUPEROPT_ENUMERATOR_H

#include "superopt/Common.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"

#include <vector>
#include <functional>
#include <memory>
#include <random>

namespace superopt {

/// Represents a constant value for synthesis
struct SynthesisConstant {
    llvm::Constant* value = nullptr;
    int64_t intValue = 0;      // For quick access
    bool isPowerOfTwo = false;
    bool isNegative = false;

    static SynthesisConstant fromInt(llvm::LLVMContext& ctx, llvm::Type* type, int64_t val);
};

/// Pool of constants available for synthesis
class ConstantPool {
public:
    ConstantPool() = default;

    /// Generate default constant pool for a type
    static ConstantPool getDefaultPool(llvm::LLVMContext& ctx, llvm::Type* type,
                                        size_t maxConstants = 8);

    /// Add a constant to the pool
    void add(const SynthesisConstant& c) { constants_.push_back(c); }

    /// Get all constants
    const std::vector<SynthesisConstant>& getConstants() const { return constants_; }

    /// Get constants of a specific type
    std::vector<SynthesisConstant> getConstantsOfType(llvm::Type* type) const;

    /// Check if pool is empty
    bool empty() const { return constants_.empty(); }

    /// Get size
    size_t size() const { return constants_.size(); }

private:
    std::vector<SynthesisConstant> constants_;
};

/// Represents an abstract instruction template for enumeration
struct InstructionTemplate {
    unsigned opcode;
    std::vector<int> operandIndices;  // Indices into available values
    llvm::Type* resultType = nullptr;

    // For comparison instructions
    llvm::CmpInst::Predicate predicate = llvm::CmpInst::ICMP_EQ;

    // For constants (-1 means use value from inputs, >= 0 means constant index)
    std::vector<int> constantIndices;

    bool operator==(const InstructionTemplate& other) const {
        return opcode == other.opcode &&
               operandIndices == other.operandIndices &&
               predicate == other.predicate &&
               constantIndices == other.constantIndices;
    }
};

/// Represents a synthesized instruction sequence
struct SynthesizedSequence {
    std::vector<InstructionTemplate> templates;
    int outputIndex = -1;  // Index of the result value

    // Constants used in this sequence
    std::vector<SynthesisConstant> constants;

    /// Apply this sequence to a basic block
    llvm::Value* apply(llvm::IRBuilder<>& builder,
                       const std::vector<llvm::Value*>& inputs) const;

    /// Get structural hash for deduplication
    uint64_t getHash() const;

    /// Get the number of instructions
    size_t size() const { return templates.size(); }

    /// Check if empty
    bool empty() const { return templates.empty() && outputIndex < 0; }

    /// Estimate cost (simple instruction count)
    double estimateCost() const;

    bool operator==(const SynthesizedSequence& other) const {
        return templates == other.templates && outputIndex == other.outputIndex;
    }
};

/// Specification for basic block synthesis
struct BlockSpec {
    std::vector<llvm::Value*> inputs;
    std::vector<llvm::Value*> outputs;
    std::vector<llvm::Type*> inputTypes;
    std::vector<llvm::Type*> outputTypes;

    static BlockSpec fromBasicBlock(const llvm::BasicBlock& bb);
};

/// Enumerates candidate instruction sequences
class Enumerator {
public:
    using CandidateCallback = std::function<bool(const SynthesizedSequence&)>;

    explicit Enumerator(const Config& config);

    /// Enumerate all sequences up to maxLength that could compute output
    /// from the given inputs. Calls callback for each candidate.
    /// Returns true if enumeration completed, false if stopped early.
    bool enumerate(llvm::Type* outputType,
                   const std::vector<llvm::Type*>& inputTypes,
                   size_t maxLength,
                   CandidateCallback callback);

    /// Enumerate with constant synthesis
    bool enumerateWithConstants(llvm::Type* outputType,
                                 const std::vector<llvm::Type*>& inputTypes,
                                 const ConstantPool& constants,
                                 size_t maxLength,
                                 CandidateCallback callback);

    /// Enumerate sequences that could replace a given instruction
    bool enumerateReplacements(const llvm::Instruction& original,
                               size_t maxLength,
                               CandidateCallback callback);

    /// Enumerate sequences that could replace a basic block
    bool enumerateBlockReplacements(const llvm::BasicBlock& original,
                                    size_t maxLength,
                                    CandidateCallback callback);

    /// Enumerate using iterative deepening
    bool enumerateIterativeDeepening(llvm::Type* outputType,
                                      const std::vector<llvm::Type*>& inputTypes,
                                      size_t maxLength,
                                      CandidateCallback callback);

    /// Get the set of opcodes we enumerate
    const std::vector<unsigned>& getEnumeratedOpcodes() const {
        return enumeratedOpcodes_;
    }

    /// Set opcodes to enumerate (default: common arithmetic/logic ops)
    void setEnumeratedOpcodes(const std::vector<unsigned>& opcodes) {
        enumeratedOpcodes_ = opcodes;
    }

    /// Get statistics about enumeration
    size_t getCandidatesGenerated() const { return candidatesGenerated_; }
    void resetStats() { candidatesGenerated_ = 0; }

    /// Set timeout
    void setTimeout(size_t timeoutMs) { timeoutMs_ = timeoutMs; }

    /// Check if timed out
    bool wasTimedOut() const { return timedOut_; }

private:
    Config config_;
    std::vector<unsigned> enumeratedOpcodes_;
    size_t candidatesGenerated_ = 0;
    size_t timeoutMs_ = 0;
    bool timedOut_ = false;
    Timer timer_;

    /// Initialize default enumerated opcodes
    void initDefaultOpcodes();

    /// Check if an opcode is valid for given operand types
    bool isValidOpcode(unsigned opcode,
                       const std::vector<llvm::Type*>& operandTypes,
                       llvm::Type* resultType);

    /// Get result type for an opcode with given operand types
    llvm::Type* getResultType(unsigned opcode,
                              const std::vector<llvm::Type*>& operandTypes);

    /// Enumerate with backtracking
    bool enumerateRecursive(
        const std::vector<llvm::Type*>& availableTypes,
        llvm::Type* targetType,
        size_t currentLength,
        size_t maxLength,
        SynthesizedSequence& current,
        CandidateCallback& callback);

    /// Enumerate with constants
    bool enumerateRecursiveWithConstants(
        std::vector<llvm::Type*>& availableTypes,
        const ConstantPool& constants,
        llvm::Type* targetType,
        size_t currentLength,
        size_t maxLength,
        SynthesizedSequence& current,
        CandidateCallback& callback);

    /// Check timeout
    bool checkTimeout() {
        if (timeoutMs_ > 0 && timer_.exceeds(timeoutMs_)) {
            timedOut_ = true;
            return true;
        }
        return false;
    }
};

/// Helper class to build instruction sequences
class SequenceBuilder {
public:
    explicit SequenceBuilder(llvm::LLVMContext& ctx);

    /// Add an input value
    size_t addInput(llvm::Type* type);

    /// Add a constant value
    size_t addConstant(llvm::Constant* c);

    /// Add an instruction
    size_t addInstruction(unsigned opcode,
                          const std::vector<size_t>& operandIndices,
                          llvm::Type* resultType = nullptr);

    /// Get the current sequence
    const SynthesizedSequence& getSequence() const { return sequence_; }

    /// Get a value by index
    llvm::Type* getType(size_t index) const;

    /// Get number of values available
    size_t numValues() const { return types_.size(); }

    /// Reset the builder
    void reset();

private:
    llvm::LLVMContext& context_;
    SynthesizedSequence sequence_;
    std::vector<llvm::Type*> types_;
    size_t numInputs_ = 0;
};

/// Stochastic search for longer sequences
class StochasticEnumerator {
public:
    explicit StochasticEnumerator(const Config& config);

    using CandidateCallback = std::function<bool(const SynthesizedSequence&, double cost)>;

    /// Search for optimal sequence using MCMC
    std::optional<SynthesizedSequence> search(
        llvm::Type* outputType,
        const std::vector<llvm::Type*>& inputTypes,
        const ConstantPool& constants,
        std::function<double(const SynthesizedSequence&)> costFn,
        std::function<bool(const SynthesizedSequence&)> verifyFn,
        size_t maxIterations);

    /// Get statistics
    size_t getIterations() const { return iterations_; }
    size_t getAccepted() const { return accepted_; }

private:
    Config config_;
    std::mt19937_64 rng_;
    size_t iterations_ = 0;
    size_t accepted_ = 0;

    /// Mutation operators
    SynthesizedSequence mutateOpcode(const SynthesizedSequence& seq,
                                      const std::vector<llvm::Type*>& inputTypes);
    SynthesizedSequence mutateOperand(const SynthesizedSequence& seq,
                                       size_t numInputs);
    SynthesizedSequence swapInstructions(const SynthesizedSequence& seq);
    SynthesizedSequence insertInstruction(const SynthesizedSequence& seq,
                                           const std::vector<llvm::Type*>& inputTypes);
    SynthesizedSequence deleteInstruction(const SynthesizedSequence& seq);

    /// Generate random sequence
    SynthesizedSequence generateRandom(llvm::Type* outputType,
                                        const std::vector<llvm::Type*>& inputTypes,
                                        const ConstantPool& constants,
                                        size_t length);

    /// Metropolis-Hastings acceptance
    bool shouldAccept(double currentCost, double proposedCost, double temperature);
};

} // namespace superopt

#endif // SUPEROPT_ENUMERATOR_H
