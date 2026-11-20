#include "pager.hpp"

#include <sys/mman.h>
#include <sys/random.h>

#include <cerrno>
#include <cstring>
#include <limits>

namespace {
    uint64_t round_up(uint64_t value, uint64_t multiple) {
        return ((value + multiple - 1) / multiple) * multiple;
    }

    bool generate_region_id(RegionId& out, std::string& error) {
        do {
            RegionId value = 0;
            std::size_t received = 0;
            while (received < sizeof(value)) {
                const ssize_t count = getrandom(
                    reinterpret_cast<uint8_t*>(&value) + received,
                    sizeof(value) - received, 0);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) {
                    error = "no se pudo generar RegionId";
                    if (count < 0) error += std::string(": ") + std::strerror(errno);
                    return false;
                }
                received += static_cast<std::size_t>(count);
            }
            out = value;
        } while (out == 0);
        return true;
    }

    bool same_descriptor(const RegionDescriptor& a, const RegionDescriptor& b) {
        return a.region_id == b.region_id && a.owner_id == b.owner_id &&
               a.size == b.size && a.page_size == b.page_size;
    }
}  // namespace

Pager::Pager(MemoryManager& memory, NodeId local_id)
    : memory_(memory), local_id_(local_id) {}

bool Pager::create_region(uint64_t size, RegionId& region_id, std::string& error) {
    if (size == 0) {
        error = "el tamano de la region debe ser mayor que 0";
        return false;
    }

    const uint64_t page_size = memory_.page_size();
    if (page_size == 0) {
        error = "el tamano de pagina del DSM debe ser mayor que 0";
        return false;
    }

    if (size > std::numeric_limits<uint64_t>::max() - (page_size - 1)) {
        error = "el tamano de la region excede el espacio de direcciones";
        return false;
    }

    const uint64_t rounded = round_up(size, page_size);
    const uint64_t count = rounded / page_size;

    std::lock_guard<std::mutex> lock(mutex_);

    const uint64_t total = memory_.page_count();
    if (next_free_page_ > total || count > total - next_free_page_) {
        error = "no hay espacio local para la region";
        return false;
    }

    RegionId id = 0;
    bool unique = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!generate_region_id(id, error)) return false;
        if (regions_.find(id) == regions_.end()) {
            unique = true;
            break;
        }
    }
    if (!unique) {
        error = "no se pudo generar un RegionId unico";
        return false;
    }

    RegionDescriptor descriptor;
    descriptor.region_id = id;
    descriptor.owner_id = local_id_;
    descriptor.size = rounded;
    descriptor.page_size = static_cast<uint32_t>(page_size);

    if (!attach_locked(descriptor, next_free_page_, error)) return false;

    next_free_page_ += count;
    region_id = id;
    return true;
}

bool Pager::attach_region(const RegionDescriptor& descriptor, std::string& error) {
    if (descriptor.region_id == 0 || descriptor.owner_id == 0) {
        error = "el descriptor de region requiere region_id y owner_id";
        return false;
    }
    if (descriptor.size == 0 || descriptor.page_size == 0) {
        error = "el descriptor de region requiere tamano y page_size";
        return false;
    }
    if (descriptor.page_size != memory_.page_size()) {
        error = "page_size de la region incompatible con el nodo local";
        return false;
    }
    if (descriptor.size % descriptor.page_size != 0) {
        error = "el tamano de la region no es multiplo del page_size";
        return false;
    }

    const uint64_t count = descriptor.size / descriptor.page_size;

    std::lock_guard<std::mutex> lock(mutex_);

    const auto found = regions_.find(descriptor.region_id);
    if (found != regions_.end()) {
        if (same_descriptor(found->second.descriptor, descriptor)) return true;
        error = "RegionId en conflicto: " + std::to_string(descriptor.region_id);
        return false;
    }

    const uint64_t total = memory_.page_count();
    if (next_free_page_ > total || count > total - next_free_page_) {
        error = "no hay espacio local para adjuntar la region";
        return false;
    }

    if (!attach_locked(descriptor, next_free_page_, error)) return false;

    next_free_page_ += count;
    return true;
}

bool Pager::attach_locked(const RegionDescriptor& descriptor, uint64_t first_page,
                          std::string& error) {
    const uint64_t count = descriptor.size / descriptor.page_size;

    RegionInfo region;
    region.descriptor = descriptor;
    region.first_page = first_page;
    region.page_count = count;
    regions_.emplace(descriptor.region_id, region);

    for (uint64_t i = 0; i < count; ++i) {
        PageInfo page;
        page.page_id = first_page + i;
        page.region_id = descriptor.region_id;
        page.owner_id = descriptor.owner_id;
        page.state = MSIState::INVALID;
        page.version = 0;
        page.locally_present = false;

        if (!pages_.emplace(page.page_id, page).second) {
            // El bump allocator nunca reutiliza paginas; esto solo protege
            // contra un estado interno inconsistente.
            regions_.erase(descriptor.region_id);
            for (uint64_t j = 0; j < i; ++j) pages_.erase(first_page + j);
            error = "colision de PageId al registrar la region";
            return false;
        }
    }

    return true;
}

bool Pager::resolve_fault(PageId page_id, bool write, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (page_id >= memory_.page_count()) {
        error = "pagina fuera de la memoria reservada";
        return false;
    }

    const auto found = pages_.find(page_id);
    if (found == pages_.end()) {
        error = "pagina desconocida: " + std::to_string(page_id);
        return false;
    }

    PageInfo& page = found->second;

    if (page.owner_id != local_id_) {
        error = "pagina remota sin soporte de transferencia (hito 2.4)";
        return false;
    }

    MSIState next_state = page.state;
    uint64_t next_version = page.version;
    int permissions = PROT_READ;

    switch (page.state) {
        case MSIState::INVALID:
            if (write) {
                next_state = MSIState::MODIFIED;
                next_version += 1;
                permissions = PROT_READ | PROT_WRITE;
            } else {
                next_state = MSIState::SHARED;
                permissions = PROT_READ;
            }
            break;

        case MSIState::SHARED:
            if (write) {
                next_state = MSIState::MODIFIED;
                next_version += 1;
                permissions = PROT_READ | PROT_WRITE;
            } else {
                permissions = PROT_READ;
            }
            break;

        case MSIState::MODIFIED:
            // M + lectura sigue en M; el dueno conserva escritura.
            permissions = PROT_READ | PROT_WRITE;
            break;
    }

    if (!memory_.protect_page(page_id, permissions, error)) {
        return false;
    }

    page.state = next_state;
    page.version = next_version;
    page.locally_present = true;
    return true;
}

bool Pager::page_info(PageId page_id, PageInfo& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pages_.find(page_id);
    if (found == pages_.end()) return false;
    out = found->second;
    return true;
}

std::vector<PageInfo> Pager::page_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PageInfo> result;
    result.reserve(pages_.size());
    for (const auto& entry : pages_) result.push_back(entry.second);
    return result;
}

bool Pager::region_info(RegionId region_id, RegionInfo& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = regions_.find(region_id);
    if (found == regions_.end()) return false;
    out = found->second;
    return true;
}

std::vector<RegionInfo> Pager::region_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RegionInfo> result;
    result.reserve(regions_.size());
    for (const auto& entry : regions_) result.push_back(entry.second);
    return result;
}

bool Pager::assignment(PageId page_id, PageAssignment& out) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto page = pages_.find(page_id);
    if (page == pages_.end()) return false;

    const auto region = regions_.find(page->second.region_id);
    if (region == regions_.end() || page_id < region->second.first_page) return false;

    PageAssignment result;
    result.page_id = page_id;
    result.region_id = page->second.region_id;
    result.owner_id = page->second.owner_id;
    result.region_offset = page_id - region->second.first_page;
    out = result;
    return true;
}
