#include "superopt/Cache.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <sstream>

namespace superopt {

//===----------------------------------------------------------------------===//
// CacheKey implementation
//===----------------------------------------------------------------------===//

uint64_t CacheKey::getHash() const {
    uint64_t hash = opcode;
    hash ^= resultTypeHash << 13;

    for (uint64_t h : operandTypeHashes) {
        hash ^= h;
        hash *= 0x100000001b3ULL;
    }

    hash ^= operandPatternHash;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;

    return hash;
}

CacheKey CacheKey::fromInstruction(const llvm::Instruction& inst) {
    CacheKey key;
    key.opcode = inst.getOpcode();

    // Hash operand types
    for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
        auto* type = inst.getOperand(i)->getType();
        uint64_t typeHash = 0;

        if (type->isIntegerTy()) {
            typeHash = 1ULL | (static_cast<uint64_t>(type->getIntegerBitWidth()) << 8);
        } else if (type->isFloatTy()) {
            typeHash = 2ULL;
        } else if (type->isDoubleTy()) {
            typeHash = 3ULL;
        } else if (type->isPointerTy()) {
            typeHash = 4ULL;
        } else if (type->isVectorTy()) {
            typeHash = 5ULL;
        }

        key.operandTypeHashes.push_back(typeHash);
    }

    // Hash result type
    auto* resultType = inst.getType();
    if (resultType->isIntegerTy()) {
        key.resultTypeHash = 1ULL | (static_cast<uint64_t>(resultType->getIntegerBitWidth()) << 8);
    } else if (resultType->isFloatTy()) {
        key.resultTypeHash = 2ULL;
    } else if (resultType->isDoubleTy()) {
        key.resultTypeHash = 3ULL;
    } else if (resultType->isPointerTy()) {
        key.resultTypeHash = 4ULL;
    } else {
        key.resultTypeHash = 0ULL;
    }

    // Hash operand pattern (captures constant values, etc.)
    key.operandPatternHash = 0;
    for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
        auto* op = inst.getOperand(i);

        uint64_t opHash = 0;
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(op)) {
            // For constants, hash the value
            opHash = ci->getZExtValue();
            opHash |= (1ULL << 62);  // Mark as constant
        } else if (llvm::isa<llvm::Argument>(op)) {
            // For arguments, hash the argument number
            if (auto* arg = llvm::dyn_cast<llvm::Argument>(op)) {
                opHash = arg->getArgNo();
                opHash |= (2ULL << 62);
            }
        } else if (llvm::isa<llvm::Instruction>(op)) {
            // For instruction results, use a marker
            opHash = 3ULL << 62;
        }

        key.operandPatternHash ^= opHash;
        key.operandPatternHash *= 0x100000001b3ULL;
    }

    return key;
}

//===----------------------------------------------------------------------===//
// OptimizationCache implementation
//===----------------------------------------------------------------------===//

OptimizationCache::OptimizationCache() = default;

OptimizationCache::OptimizationCache(const std::string& filePath)
    : filePath_(filePath) {
    if (!filePath.empty()) {
        load(filePath);
    }
}

OptimizationCache::~OptimizationCache() {
    if (!filePath_.empty()) {
        save(filePath_);
    }
}

void OptimizationCache::lockIfNeeded() const {
    if (threadSafe_) {
        const_cast<std::mutex&>(mutex_).lock();
    }
}

void OptimizationCache::unlockIfNeeded() const {
    if (threadSafe_) {
        const_cast<std::mutex&>(mutex_).unlock();
    }
}

std::optional<CacheEntry> OptimizationCache::lookup(const CacheKey& key) {
    lockIfNeeded();

    auto it = cache_.find(key);
    if (it != cache_.end()) {
        hits_++;
        auto result = it->second;
        unlockIfNeeded();
        return result;
    }

    misses_++;
    unlockIfNeeded();
    return std::nullopt;
}

std::optional<CacheEntry> OptimizationCache::lookup(const llvm::Instruction& inst) {
    return lookup(CacheKey::fromInstruction(inst));
}

void OptimizationCache::store(const CacheKey& key, const CacheEntry& entry) {
    lockIfNeeded();
    cache_[key] = entry;
    unlockIfNeeded();
}

void OptimizationCache::store(const llvm::Instruction& inst,
                               const SynthesizedSequence& seq,
                               double originalCost,
                               double optimizedCost) {
    CacheEntry entry;
    entry.sequence = seq;
    entry.originalCost = originalCost;
    entry.optimizedCost = optimizedCost;
    entry.hasOptimization = true;
    entry.timestamp = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());

    store(CacheKey::fromInstruction(inst), entry);
}

void OptimizationCache::storeNoOptimization(const llvm::Instruction& inst, double cost) {
    CacheEntry entry;
    entry.originalCost = cost;
    entry.optimizedCost = cost;
    entry.hasOptimization = false;
    entry.timestamp = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());

    store(CacheKey::fromInstruction(inst), entry);
}

bool OptimizationCache::contains(const CacheKey& key) const {
    lockIfNeeded();
    bool result = cache_.find(key) != cache_.end();
    unlockIfNeeded();
    return result;
}

void OptimizationCache::clear() {
    lockIfNeeded();
    cache_.clear();
    hits_ = 0;
    misses_ = 0;
    unlockIfNeeded();
}

bool OptimizationCache::save(const std::string& filePath) {
    lockIfNeeded();

    std::ofstream file(filePath, std::ios::binary);
    if (!file) {
        unlockIfNeeded();
        return false;
    }

    // Write header
    uint32_t magic = 0x53505443;  // "SPTC"
    uint32_t version = 1;
    uint64_t numEntries = cache_.size();

    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&numEntries), sizeof(numEntries));

    // Write entries
    for (const auto& [key, entry] : cache_) {
        // Write key
        file.write(reinterpret_cast<const char*>(&key.opcode), sizeof(key.opcode));
        file.write(reinterpret_cast<const char*>(&key.resultTypeHash), sizeof(key.resultTypeHash));
        file.write(reinterpret_cast<const char*>(&key.operandPatternHash), sizeof(key.operandPatternHash));

        uint32_t numOperandHashes = static_cast<uint32_t>(key.operandTypeHashes.size());
        file.write(reinterpret_cast<const char*>(&numOperandHashes), sizeof(numOperandHashes));
        for (uint64_t h : key.operandTypeHashes) {
            file.write(reinterpret_cast<const char*>(&h), sizeof(h));
        }

        // Write entry metadata
        file.write(reinterpret_cast<const char*>(&entry.originalCost), sizeof(entry.originalCost));
        file.write(reinterpret_cast<const char*>(&entry.optimizedCost), sizeof(entry.optimizedCost));
        file.write(reinterpret_cast<const char*>(&entry.hasOptimization), sizeof(entry.hasOptimization));
        file.write(reinterpret_cast<const char*>(&entry.timestamp), sizeof(entry.timestamp));

        // Write sequence (simplified - just template count and constants)
        uint32_t numTemplates = static_cast<uint32_t>(entry.sequence.templates.size());
        file.write(reinterpret_cast<const char*>(&numTemplates), sizeof(numTemplates));

        for (const auto& templ : entry.sequence.templates) {
            file.write(reinterpret_cast<const char*>(&templ.opcode), sizeof(templ.opcode));

            uint32_t numOperands = static_cast<uint32_t>(templ.operands.size());
            file.write(reinterpret_cast<const char*>(&numOperands), sizeof(numOperands));
            for (int op : templ.operands) {
                file.write(reinterpret_cast<const char*>(&op), sizeof(op));
            }

            file.write(reinterpret_cast<const char*>(&templ.constantIndex), sizeof(templ.constantIndex));
        }

        uint32_t numConstants = static_cast<uint32_t>(entry.sequence.constants.size());
        file.write(reinterpret_cast<const char*>(&numConstants), sizeof(numConstants));
        for (const auto& c : entry.sequence.constants) {
            file.write(reinterpret_cast<const char*>(&c.intValue), sizeof(c.intValue));
            file.write(reinterpret_cast<const char*>(&c.isPowerOfTwo), sizeof(c.isPowerOfTwo));
        }
    }

    unlockIfNeeded();
    return true;
}

bool OptimizationCache::load(const std::string& filePath) {
    lockIfNeeded();

    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        unlockIfNeeded();
        return false;
    }

    // Read header
    uint32_t magic, version;
    uint64_t numEntries;

    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&numEntries), sizeof(numEntries));

    if (magic != 0x53505443 || version != 1) {
        unlockIfNeeded();
        return false;
    }

    cache_.clear();

    // Read entries
    for (uint64_t i = 0; i < numEntries; ++i) {
        CacheKey key;
        CacheEntry entry;

        // Read key
        file.read(reinterpret_cast<char*>(&key.opcode), sizeof(key.opcode));
        file.read(reinterpret_cast<char*>(&key.resultTypeHash), sizeof(key.resultTypeHash));
        file.read(reinterpret_cast<char*>(&key.operandPatternHash), sizeof(key.operandPatternHash));

        uint32_t numOperandHashes;
        file.read(reinterpret_cast<char*>(&numOperandHashes), sizeof(numOperandHashes));
        key.operandTypeHashes.resize(numOperandHashes);
        for (uint32_t j = 0; j < numOperandHashes; ++j) {
            file.read(reinterpret_cast<char*>(&key.operandTypeHashes[j]), sizeof(uint64_t));
        }

        // Read entry metadata
        file.read(reinterpret_cast<char*>(&entry.originalCost), sizeof(entry.originalCost));
        file.read(reinterpret_cast<char*>(&entry.optimizedCost), sizeof(entry.optimizedCost));
        file.read(reinterpret_cast<char*>(&entry.hasOptimization), sizeof(entry.hasOptimization));
        file.read(reinterpret_cast<char*>(&entry.timestamp), sizeof(entry.timestamp));

        // Read sequence
        uint32_t numTemplates;
        file.read(reinterpret_cast<char*>(&numTemplates), sizeof(numTemplates));

        entry.sequence.templates.resize(numTemplates);
        for (uint32_t j = 0; j < numTemplates; ++j) {
            auto& templ = entry.sequence.templates[j];
            file.read(reinterpret_cast<char*>(&templ.opcode), sizeof(templ.opcode));

            uint32_t numOperands;
            file.read(reinterpret_cast<char*>(&numOperands), sizeof(numOperands));
            templ.operands.resize(numOperands);
            for (uint32_t k = 0; k < numOperands; ++k) {
                file.read(reinterpret_cast<char*>(&templ.operands[k]), sizeof(int));
            }

            file.read(reinterpret_cast<char*>(&templ.constantIndex), sizeof(templ.constantIndex));
        }

        uint32_t numConstants;
        file.read(reinterpret_cast<char*>(&numConstants), sizeof(numConstants));
        entry.sequence.constants.resize(numConstants);
        for (uint32_t j = 0; j < numConstants; ++j) {
            file.read(reinterpret_cast<char*>(&entry.sequence.constants[j].intValue), sizeof(int64_t));
            file.read(reinterpret_cast<char*>(&entry.sequence.constants[j].isPowerOfTwo), sizeof(bool));
        }

        cache_[key] = entry;
    }

    unlockIfNeeded();
    return true;
}

//===----------------------------------------------------------------------===//
// LRUCache implementation
//===----------------------------------------------------------------------===//

LRUCache::LRUCache(size_t maxSize) : maxSize_(maxSize) {}

void LRUCache::addToFront(Node* node) {
    node->next = head_;
    node->prev = nullptr;

    if (head_) {
        head_->prev = node;
    }
    head_ = node;

    if (!tail_) {
        tail_ = node;
    }
}

void LRUCache::moveToFront(Node* node) {
    if (node == head_) {
        return;
    }

    // Remove from current position
    if (node->prev) {
        node->prev->next = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    if (node == tail_) {
        tail_ = node->prev;
    }

    // Add to front
    addToFront(node);
}

void LRUCache::removeTail() {
    if (!tail_) {
        return;
    }

    Node* oldTail = tail_;
    cache_.erase(oldTail->key);

    if (tail_->prev) {
        tail_ = tail_->prev;
        tail_->next = nullptr;
    } else {
        head_ = nullptr;
        tail_ = nullptr;
    }

    delete oldTail;
}

std::optional<CacheEntry> LRUCache::get(const CacheKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return std::nullopt;
    }

    moveToFront(it->second);
    return it->second->entry;
}

void LRUCache::put(const CacheKey& key, const CacheEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(key);
    if (it != cache_.end()) {
        it->second->entry = entry;
        moveToFront(it->second);
        return;
    }

    // Evict if necessary
    while (cache_.size() >= maxSize_) {
        removeTail();
    }

    // Add new node
    Node* node = new Node();
    node->key = key;
    node->entry = entry;

    cache_[key] = node;
    addToFront(node);
}

void LRUCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    Node* current = head_;
    while (current) {
        Node* next = current->next;
        delete current;
        current = next;
    }

    cache_.clear();
    head_ = nullptr;
    tail_ = nullptr;
}

} // namespace superopt
