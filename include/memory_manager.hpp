#pragma once

#include <cstdint>
#include <string>

class MemoryManager {
public:
    MemoryManager(uint64_t size, uint32_t page_size);
    ~MemoryManager();

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    bool reserve(std::string& error);

    void*    base()       const { return base_; }
    uint64_t size()       const { return size_; }
    uint64_t page_count() const { return page_count_; }
    uint32_t page_size()  const { return page_size_; }

private:
    void*    base_ = nullptr;
    uint64_t size_ = 0;
    uint64_t page_count_ = 0;
    uint32_t page_size_ = 0;
};
