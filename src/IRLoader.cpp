#include "superopt/IRLoader.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/Error.h"

namespace superopt {

IRLoader::IRLoader(llvm::LLVMContext& ctx) : context_(ctx) {}

std::unique_ptr<llvm::Module> IRLoader::loadBitcode(const std::string& path) {
    auto bufferOrErr = llvm::MemoryBuffer::getFile(path);
    if (!bufferOrErr) {
        lastError_ = "Failed to open file: " + path;
        return nullptr;
    }

    auto moduleOrErr = llvm::parseBitcodeFile(
        bufferOrErr.get()->getMemBufferRef(), context_);

    if (!moduleOrErr) {
        lastError_ = "Failed to parse bitcode: " +
                     llvm::toString(moduleOrErr.takeError());
        return nullptr;
    }

    return std::move(moduleOrErr.get());
}

std::unique_ptr<llvm::Module> IRLoader::loadIR(const std::string& path) {
    llvm::SMDiagnostic err;
    auto module = llvm::parseIRFile(path, err, context_);

    if (!module) {
        lastError_ = "Failed to parse IR: " + err.getMessage().str();
        return nullptr;
    }

    return module;
}

std::unique_ptr<llvm::Module> IRLoader::load(const std::string& path) {
    // Try to detect file type by extension
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".bc") {
        return loadBitcode(path);
    } else if (path.size() >= 3 && path.substr(path.size() - 3) == ".ll") {
        return loadIR(path);
    }

    // Try bitcode first, then IR
    auto module = loadBitcode(path);
    if (module) return module;

    return loadIR(path);
}

std::unique_ptr<llvm::Module> IRLoader::loadFromBuffer(llvm::MemoryBufferRef buffer) {
    // Try bitcode first
    auto bitcodeResult = llvm::parseBitcodeFile(buffer, context_);
    if (bitcodeResult) {
        return std::move(bitcodeResult.get());
    }

    // Consume the error
    llvm::consumeError(bitcodeResult.takeError());

    // Try IR
    llvm::SMDiagnostic err;
    auto module = llvm::parseIR(buffer, err, context_);

    if (!module) {
        lastError_ = "Failed to parse: " + err.getMessage().str();
        return nullptr;
    }

    return module;
}

std::unique_ptr<llvm::Module> IRLoader::parseIR(const std::string& ir) {
    auto buffer = llvm::MemoryBuffer::getMemBuffer(ir);
    llvm::SMDiagnostic err;
    auto module = llvm::parseIR(buffer->getMemBufferRef(), err, context_);

    if (!module) {
        lastError_ = "Failed to parse IR: " + err.getMessage().str();
        return nullptr;
    }

    return module;
}

bool IRLoader::saveBitcode(const llvm::Module& module, const std::string& path) {
    std::error_code ec;
    llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_None);

    if (ec) {
        lastError_ = "Failed to open output file: " + ec.message();
        return false;
    }

    llvm::WriteBitcodeToFile(module, os);
    return true;
}

bool IRLoader::saveIR(const llvm::Module& module, const std::string& path) {
    std::error_code ec;
    llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_Text);

    if (ec) {
        lastError_ = "Failed to open output file: " + ec.message();
        return false;
    }

    module.print(os, nullptr);
    return true;
}

std::unique_ptr<llvm::Module> IRLoader::cloneFunction(const llvm::Function& func) {
    auto module = std::make_unique<llvm::Module>(
        func.getName().str() + "_clone", context_);

    // Clone the function
    llvm::ValueToValueMapTy vmap;
    auto* cloned = llvm::CloneFunction(&func, vmap);

    // Insert into new module
    module->getFunctionList().push_back(cloned);

    return module;
}

std::string IRLoader::getIRString(const llvm::Module& module) {
    std::string str;
    llvm::raw_string_ostream os(str);
    module.print(os, nullptr);
    return str;
}

std::string IRLoader::getIRString(const llvm::Function& func) {
    std::string str;
    llvm::raw_string_ostream os(str);
    func.print(os);
    return str;
}

std::string IRLoader::getIRString(const llvm::BasicBlock& bb) {
    std::string str;
    llvm::raw_string_ostream os(str);
    bb.print(os);
    return str;
}

std::string IRLoader::getIRString(const llvm::Instruction& inst) {
    std::string str;
    llvm::raw_string_ostream os(str);
    inst.print(os);
    return str;
}

} // namespace superopt
