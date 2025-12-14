#ifndef SUPEROPT_CACHE_H
#define SUPEROPT_CACHE_H

#include "superopt/Common.h"
#include "superopt/Enumerator.h"
#include "llvm/IR/Instructions.h"

#include <unordered_map>
#include <mutex>
#include <fstream>

namespace superopt {

/// Key for caching optimization results
struct CacheKey {
    unsigned opcode;
    std::vector<uint64_t> operandTypeHashes;
    uint64_t resultTypeHash;
    uint64_t operandPatternHash;  // Captures constant operands, etc.

    uint64_t getHash() const;

    bool operator==(const CacheKey& other) const {
        return opcode == other.opcode &&
               operandTypeHashes == other.operandTypeHashes &&
               resultTypeHash == other.resultTypeHash &&
               operandPatternHash == other.operandPatternHash;
    }

    static CacheKey fromInstruction(const llvm::Instruction& inst);
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& key) const {
        return static_cast<size_t>(key.getHash());
    }
};

/// Cached optimization result
struct CacheEntry {
    SynthesizedSequence sequence;
    double originalCost;
    double optimizedCost;
    bool hasOptimization;
    uint64_t timestamp;
};

/// Optimization result cache
class OptimizationCache {
public:
    OptimizationCache();
    explicit OptimizationCache(const std::string& filePath);
    ~OptimizationCache();

    /// Look up a cached result
    std::optional<CacheEntry> lookup(const CacheKey& key);

    /// Look up by instruction
    std::optional<CacheEntry> lookup(const llvm::Instruction& inst);

    /// Store a result
    void store(const CacheKey& key, const CacheEntry& entry);

    /// Store a result for an instruction
    void store(const llvm::Instruction& inst,
               const SynthesizedSequence& seq,
               double originalCost,
               double optimizedCost);

    /// Store a negative result (no optimization found)
    void storeNoOptimization(const llvm::Instruction& inst, double cost);

    /// Check if key is in cache
    bool contains(const CacheKey& key) const;

    /// Get cache statistics
    size_t getHits() const { return hits_; }
    size_t getMisses() const { return misses_; }
    size_t getSize() const { return cache_.size(); }
    double getHitRate() const {
        size_t total = hits_ + misses_;
        return total > 0 ? static_cast<double>(hits_) / total : 0.0;
    }

    /// Clear the cache
    void clear();

    /// Save cache to file
    bool save(const std::string& filePath);

    /// Load cache from file
    bool load(const std::string& filePath);

    /// Enable/disable thread safety
    void setThreadSafe(bool enabled) { threadSafe_ = enabled; }

private:
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> cache_;
    mutable std::mutex mutex_;
    std::string filePath_;
    size_t hits_ = 0;
    size_t misses_ = 0;
    bool threadSafe_ = true;

    void lockIfNeeded() const;
    void unlockIfNeeded() const;
};

/// LRU cache with size limit
class LRUCache {
public:
    explicit LRUCache(size_t maxSize);

    std::optional<CacheEntry> get(const CacheKey& key);
    void put(const CacheKey& key, const CacheEntry& entry);
    void clear();
    size_t size() const { return cache_.size(); }

private:
    struct Node {
        CacheKey key;
        CacheEntry entry;
        Node* prev = nullptr;
        Node* next = nullptr;
    };

    size_t maxSize_;
    std::unordered_map<CacheKey, Node*, CacheKeyHash> cache_;
    Node* head_ = nullptr;
    Node* tail_ = nullptr;
    mutable std::mutex mutex_;

    void moveToFront(Node* node);
    void removeTail();
    void addToFront(Node* node);
};

} // namespace superopt

#endif // SUPEROPT_CACHE_H
