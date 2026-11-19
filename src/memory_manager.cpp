#include "memory_manager.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>

namespace {
    uint64_t round_up(uint64_t value, uint64_t multiple) {
        return ((value + multiple - 1) / multiple) * multiple;
    }
}  // namespace

MemoryManager::MemoryManager(uint64_t size, uint32_t page_size)
    : size_(size), page_size_(page_size) {}

MemoryManager::~MemoryManager() {
    if (base_ != nullptr) {
        munmap(base_, size_);
    }
}

bool MemoryManager::reserve(std::string& error) {
    if (base_ != nullptr) {
        error = "la memoria ya esta reservada";
        return false;
    }
    if (size_ == 0) {
        error = "el tamano de memoria debe ser mayor que 0";
        return false;
    }

    if (page_size_ == 0) {
        error = "el tamano de pagina debe ser mayor que 0";
        return false;
    }

    const long os_page_raw = sysconf(_SC_PAGESIZE);
    if (os_page_raw <= 0) {
        error = "no se pudo obtener el tamano de pagina del sistema";
        return false;
    }

    const uint64_t os_page = static_cast<uint64_t>(os_page_raw);

    if (page_size_ % os_page != 0) {
        error = "el tamano de pagina del DSM debe ser multiplo de " +
                std::to_string(os_page) + " bytes";
        return false;
    }

    if (size_ > std::numeric_limits<std::size_t>::max() - (page_size_ - 1)) {
        error = "el tamano de memoria excede el espacio de direcciones";
        return false;
    }
    size_ = round_up(size_, page_size_);

    void* address = mmap(
        nullptr,
        size_,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (address == MAP_FAILED) {
        error = std::string("mmap fallo: ") + std::strerror(errno);
        base_ = nullptr;
        return false;
    }

    base_ = address;
    page_count_ = size_ / page_size_;
    return true;
}

void* MemoryManager::page_address(PageId page_id) const {
    if (base_ == nullptr || page_id >= page_count_) return nullptr;
    return static_cast<char*>(base_) + page_id * page_size_;
}

PageId MemoryManager::page_id_from_address(const void* address) const {
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(base_);
    if (base_ == nullptr || value < begin || value - begin >= size_) return UINT64_MAX;
    return (value - begin) / page_size_;
}

bool MemoryManager::protect_page(PageId page_id, int prot, std::string& error) {
    void* address = page_address(page_id);
    if (address == nullptr) {
        error = "pagina fuera de la memoria reservada";
        return false;
    }
    if (mprotect(address, page_size_, prot) == -1) {
        error = std::string("mprotect fallo: ") + std::strerror(errno);
        return false;
    }
    return true;
}
