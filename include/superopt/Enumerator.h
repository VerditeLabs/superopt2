#ifndef SUPEROPT_ENUMERATOR_H
#define SUPEROPT_ENUMERATOR_H

#include "superopt/Common.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <vector>
#include <functional>
#include <memory>

namespace superopt {

/// Represents an abstract instruction template for enumeration
struct InstructionTemplate {
    unsigned opcode;
    std::vector<int> operandIndices;  // Indices into available values
    llvm::Type* resultType = nullptr;

    // For comparison instructions
    llvm::CmpInst::Predicate predicate = llvm::CmpInst::ICMP_EQ;
};

/// Represents a synthesized instruction sequence
struct SynthesizedSequence {
    std::vector<InstructionTemplate> templates;
    int outputIndex = -1;  // Index of the result value

    /// Apply this sequence to a basic block
    llvm::Value* apply(llvm::IRBuilder<>& builder,
                       const std::vector<llvm::Value*>& inputs) const;
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

    /// Enumerate sequences that could replace a given instruction
    bool enumerateReplacements(const llvm::Instruction& original,
                               size_t maxLength,
                               CandidateCallback callback);

    /// Enumerate sequences that could replace a basic block
    bool enumerateBlockReplacements(const llvm::BasicBlock& original,
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

private:
    Config config_;
    std::vector<unsigned> enumeratedOpcodes_;
    size_t candidatesGenerated_ = 0;

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

} // namespace superopt

#endif // SUPEROPT_ENUMERATOR_H
