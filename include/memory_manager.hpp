#pragma once

#include "page.hpp"

#include <cstdint>
#include <string>
#include <vector>

class MemoryManager {
public:
    MemoryManager(uint64_t size, uint32_t page_size);
    ~MemoryManager();

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    bool reserve(std::string& error);
    void* page_address(PageId page_id) const;
    // Devuelve UINT64_MAX si la direccion no pertenece a esta reserva.
    PageId page_id_from_address(const void* address) const;
    bool protect_page(PageId page_id, int prot, std::string& error);
    bool install_page(PageId page_id, const std::vector<uint8_t>& data,
                      int prot, std::string& error);

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
