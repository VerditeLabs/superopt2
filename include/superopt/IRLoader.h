#ifndef SUPEROPT_IRLOADER_H
#define SUPEROPT_IRLOADER_H

#include "superopt/Common.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <optional>

namespace superopt {

/// Handles loading and saving LLVM bitcode/IR
class IRLoader {
public:
    explicit IRLoader(llvm::LLVMContext& ctx);

    /// Load a module from a bitcode file
    std::unique_ptr<llvm::Module> loadBitcode(const std::string& path);

    /// Load a module from an IR text file (.ll)
    std::unique_ptr<llvm::Module> loadIR(const std::string& path);

    /// Load a module from either bitcode or IR (auto-detected)
    std::unique_ptr<llvm::Module> load(const std::string& path);

    /// Load a module from a memory buffer
    std::unique_ptr<llvm::Module> loadFromBuffer(llvm::MemoryBufferRef buffer);

    /// Parse IR from a string
    std::unique_ptr<llvm::Module> parseIR(const std::string& ir);

    /// Save a module to a bitcode file
    bool saveBitcode(const llvm::Module& module, const std::string& path);

    /// Save a module to an IR text file
    bool saveIR(const llvm::Module& module, const std::string& path);

    /// Get the last error message
    const std::string& getLastError() const { return lastError_; }

    /// Clone a function into a new module
    std::unique_ptr<llvm::Module> cloneFunction(const llvm::Function& func);

    /// Get IR as a string
    static std::string getIRString(const llvm::Module& module);
    static std::string getIRString(const llvm::Function& func);
    static std::string getIRString(const llvm::BasicBlock& bb);
    static std::string getIRString(const llvm::Instruction& inst);

private:
    llvm::LLVMContext& context_;
    std::string lastError_;
};

} // namespace superopt

#endif // SUPEROPT_IRLOADER_H
