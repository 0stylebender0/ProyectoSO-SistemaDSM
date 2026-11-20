#include "pager.hpp"

#include <sys/mman.h>
#include <sys/random.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

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

void Pager::set_page_fetcher(PageFetcher fetcher) {
    fetcher_ = std::move(fetcher);
}

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

bool Pager::resolve_owner_locked(PageInfo& page, bool write, std::string& error) {
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

    if (!memory_.protect_page(page.page_id, permissions, error)) {
        return false;
    }

    page.state = next_state;
    page.version = next_version;
    page.locally_present = true;
    return true;
}

bool Pager::resolve_fault(PageId page_id, bool write, std::string& error) {
    RemotePageQuery query;
    {
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
        if (page.owner_id == local_id_) {
            return resolve_owner_locked(page, write, error);
        }

        // La escritura remota con invalidacion pertenece al hito 2.5.
        if (write) {
            error = "escritura remota sin soporte de invalidacion (hito 2.5)";
            return false;
        }
        if (!fetcher_) {
            error = "no hay fetcher configurado para paginas remotas";
            return false;
        }

        const auto region = regions_.find(page.region_id);
        if (region == regions_.end() ||
            page_id < region->second.first_page ||
            page_id >= region->second.first_page + region->second.page_count ||
            region->second.descriptor.owner_id != page.owner_id) {
            error = "la pagina remota no pertenece a una region conocida";
            return false;
        }

        query.page_id = page_id;
        query.region_id = page.region_id;
        query.region_offset = page_id - region->second.first_page;
        query.owner_id = region->second.descriptor.owner_id;
        query.current_state = page.state;
        query.known_version = page.version;
        query.write = false;
    }

    // La red no se ejecuta con mutex_ tomado.
    PageData response;
    if (!fetcher_(query, response, error)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pages_.find(page_id);
    if (found == pages_.end()) {
        error = "la pagina local desaparecio durante la transferencia";
        return false;
    }

    PageInfo& page = found->second;
    if (page.owner_id == local_id_) {
        error = "la pagina cambio de dueno durante la transferencia";
        return false;
    }
    if (page.locally_present && page.state != MSIState::INVALID) {
        // Otro hilo ya materializo la pagina mientras esperabamos la red.
        return true;
    }
    if (response.page_id != query.page_id ||
        response.region_id != query.region_id ||
        response.region_offset != query.region_offset ||
        response.state != static_cast<uint8_t>(MSIState::SHARED)) {
        error = "PAGE_DATA no corresponde a la pagina solicitada";
        return false;
    }

    const uint64_t page_size = memory_.page_size();
    if (response.data.size() != page_size) {
        error = "PAGE_DATA con tamano de pagina invalido";
        return false;
    }
    void* address = memory_.page_address(page_id);
    if (address == nullptr) {
        error = "pagina fuera de la memoria reservada";
        return false;
    }
    if (!memory_.protect_page(page_id, PROT_READ | PROT_WRITE, error)) {
        return false;
    }
    std::memcpy(address, response.data.data(), static_cast<std::size_t>(page_size));
    if (!memory_.protect_page(page_id, PROT_READ, error)) {
        return false;
    }

    page.state = MSIState::SHARED;
    page.version = response.version;
    page.locally_present = true;
    return true;
}

bool Pager::serve_page_request(const PageRequest& request, NodeId requester,
                               PageData& response, std::string& error) {
    if (request.access_type != 0) {
        error = "la escritura remota se implementa en el hito 2.5";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto region = regions_.find(request.region_id);
    if (region == regions_.end()) {
        error = "region desconocida: " + std::to_string(request.region_id);
        return false;
    }
    if (region->second.descriptor.owner_id != local_id_) {
        error = "la region no pertenece al nodo local";
        return false;
    }
    if (request.region_offset >= region->second.page_count) {
        error = "region_offset fuera de la region";
        return false;
    }

    const PageId page_id = region->second.first_page + request.region_offset;
    const auto found = pages_.find(page_id);
    if (found == pages_.end()) {
        error = "pagina desconocida: " + std::to_string(page_id);
        return false;
    }

    PageInfo& page = found->second;
    const uint64_t page_size = memory_.page_size();

    if (!page.locally_present || page.state == MSIState::INVALID) {
        if (!memory_.protect_page(page_id, PROT_READ | PROT_WRITE, error)) {
            return false;
        }
        std::memset(memory_.page_address(page_id), 0, static_cast<std::size_t>(page_size));
        if (!memory_.protect_page(page_id, PROT_READ, error)) {
            return false;
        }
        page.state = MSIState::SHARED;
        page.locally_present = true;
    } else if (page.state == MSIState::MODIFIED) {
        // Al compartir, el dueno deja de tener acceso exclusivo de escritura.
        if (!memory_.protect_page(page_id, PROT_READ, error)) {
            return false;
        }
        page.state = MSIState::SHARED;
    } else {
        if (!memory_.protect_page(page_id, PROT_READ, error)) {
            return false;
        }
        page.locally_present = true;
    }

    if (requester != 0 && requester != local_id_) {
        page.sharers.insert(requester);
    }

    response.page_id = request.page_id;
    response.region_id = request.region_id;
    response.region_offset = request.region_offset;
    response.version = page.version;
    response.state = static_cast<uint8_t>(MSIState::SHARED);
    response.data_size = static_cast<uint32_t>(page_size);
    response.data.resize(static_cast<std::size_t>(page_size));
    std::memcpy(response.data.data(), memory_.page_address(page_id),
                static_cast<std::size_t>(page_size));
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
