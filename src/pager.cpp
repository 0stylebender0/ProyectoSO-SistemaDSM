#include "pager.hpp"

#include <sys/mman.h>
#include <sys/random.h>

#include <cerrno>
#include <chrono>
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

Pager::Pager(MemoryManager& memory, NodeId local_id, uint32_t timeout_ms)
    : memory_(memory), local_id_(local_id), timeout_ms_(timeout_ms) {}

void Pager::set_page_fetcher(PageFetcher fetcher) {
    fetcher_ = std::move(fetcher);
}

void Pager::set_page_invalidator(PageInvalidator invalidator) {
    invalidator_ = std::move(invalidator);
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
            regions_.erase(descriptor.region_id);
            for (uint64_t j = 0; j < i; ++j) pages_.erase(first_page + j);
            error = "colision de PageId al registrar la region";
            return false;
        }
        if (descriptor.owner_id == local_id_) {
            GlobalPageControl control;
            control.page_id = page.page_id;
            control.region_id = descriptor.region_id;
            control.owner_id = local_id_;
            controls_.emplace(page.page_id, control);
        }
    }

    return true;
}

bool Pager::resolve_fault(PageId page_id, bool write, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (pages_.find(page_id) == pages_.end()) {
        error = "pagina desconocida: " + std::to_string(page_id);
        return false;
    }
    if (!ready_.wait_until(lock, deadline, [&] { return !sync_[page_id].fetching; })) {
        error = "timeout esperando un fallo pendiente";
        return false;
    }
    while (true) {
        PageInfo& page = pages_.at(page_id);
        if (page.owner_id == local_id_) {
            if (!ready_.wait_until(lock, deadline, [&] {
                    return !controls_[page_id].write_in_progress;
                })) {
                error = "timeout esperando el directorio local";
                return false;
            }
        }
        if (page.state == MSIState::MODIFIED || (!write && page.state == MSIState::SHARED)) {
            return true;
        }
        const RegionInfo& region = regions_.at(page.region_id);
        PageRequest request;
        request.page_id = page_id;
        request.region_id = page.region_id;
        request.region_offset = page_id - region.first_page;
        request.access_type = write ? 1 : 0;
        request.current_state = static_cast<uint8_t>(page.state);
        request.known_version = page.version;
        if (page.owner_id == local_id_) {
            PageData response;
            return serve_locked(request, local_id_, page_id, response, lock, error);
        }
        if (!fetcher_) {
            error = "no hay fetcher configurado para paginas remotas";
            return false;
        }
        RemotePageQuery query;
        query.page_id = page_id;
        query.region_id = page.region_id;
        query.region_offset = request.region_offset;
        query.owner_id = page.owner_id;
        query.current_state = page.state;
        query.known_version = page.version;
        query.write = write;
        const uint64_t epoch = sync_[page_id].epoch;
        sync_[page_id].fetching = true;
        lock.unlock();
        PageData response;
        const bool fetched = fetcher_(query, response, error);
        lock.lock();
        sync_[page_id].fetching = false;
        ready_.notify_all();
        if (!fetched) return false;
        // Invalidar nuestra copia S forma parte de concedernos M.
        // Las invalidaciones de otras operaciones descartan la descarga pendiente.
        const auto& sync = sync_.at(page_id);
        const bool own_grant = sync.grant_recipient == local_id_ &&
                               sync.invalidation_version == response.version;
        if (epoch != sync.epoch && !own_grant) {
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "timeout reintentando una pagina invalidada durante la descarga";
                return false;
            }
            continue;
        }
        const auto expected = write ? MSIState::MODIFIED : MSIState::SHARED;
        if (response.page_id != page_id || response.region_id != query.region_id ||
            response.region_offset != query.region_offset ||
            response.state != static_cast<uint8_t>(expected) ||
            response.version < pages_.at(page_id).version ||
            response.data.size() != memory_.page_size() ||
            response.data_size != response.data.size()) {
            error = "PAGE_DATA no corresponde a la pagina solicitada";
            return false;
        }
        const int prot = PROT_READ | (write ? PROT_WRITE : 0);
        if (!memory_.install_page(page_id, response.data, prot, error)) return false;
        PageInfo& installed = pages_.at(page_id);
        installed.state = expected;
        installed.version = response.version;
        installed.locally_present = true;
        sync_[page_id].writeback = PageData{};
        return true;
    }
}

bool Pager::serve_page_request(const PageRequest& request, NodeId requester,
                               PageData& response, std::string& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto region = regions_.find(request.region_id);
    if (requester == 0 || request.access_type > 1 || region == regions_.end() ||
        region->second.descriptor.owner_id != local_id_ ||
        request.region_offset >= region->second.page_count) {
        error = "PAGE_REQUEST con region, pagina o solicitante invalido";
        return false;
    }
    const PageId page_id = region->second.first_page + request.region_offset;
    if (!ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms_), [&] {
            return !controls_[page_id].write_in_progress;
        })) {
        error = "timeout esperando una transferencia de pagina";
        return false;
    }
    return serve_locked(request, requester, page_id, response, lock, error);
}

bool Pager::serve_locked(const PageRequest& request, NodeId requester, PageId page_id,
                         PageData& response, std::unique_lock<std::mutex>& lock,
                         std::string& error) {
    auto& control = controls_[page_id];
    control.page_id = page_id;
    control.region_id = request.region_id;
    control.owner_id = local_id_;
    const bool write = request.access_type == 1;
    if (write && control.version == std::numeric_limits<uint64_t>::max()) {
        error = "la version de pagina excede el limite";
        return false;
    }
    control.write_in_progress = true;
    auto finish = [&](bool ok) {
        controls_.at(page_id).write_in_progress = false;
        ready_.notify_all();
        return ok;
    };
    auto& page = pages_.at(page_id);
    auto& data = backing_[page_id];
    if (data.empty()) data.resize(memory_.page_size(), 0);
    if (page.state == MSIState::MODIFIED) {
        if (!memory_.protect_page(page_id, PROT_READ, error)) return finish(false);
        std::memcpy(data.data(), memory_.page_address(page_id), data.size());
        page.state = MSIState::SHARED;
        control.current_holder = 0;
    }
    if (write) {
        if (!memory_.protect_page(page_id, PROT_NONE, error)) return finish(false);
        page.state = MSIState::INVALID;
        page.locally_present = false;
    }
    control.owner_state = page.state;
    const uint64_t version = control.version;
    const NodeId holder = control.current_holder;
    std::set<NodeId> targets;
    if (write) targets = control.sharers;
    if (holder != 0 && holder != local_id_) targets.insert(holder);
    InvalidateMessage message;
    message.page_id = page_id;
    message.region_id = request.region_id;
    message.region_offset = request.region_offset;
    message.new_version = version + (write ? 1 : 0);
    message.new_owner = requester;
    message.target_state = static_cast<uint8_t>(write ? MSIState::INVALID : MSIState::SHARED);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
    for (NodeId target : targets) {
        if (target == local_id_) continue;
        if (!invalidator_) {
            error = "no hay invalidator configurado";
            return finish(false);
        }
        lock.unlock();
        PageData writeback;
        const bool invalidated = invalidator_(target, message, writeback, error);
        lock.lock();
        if (!invalidated) return finish(false);
        auto& current = controls_.at(page_id);
        if (target == holder) {
            if (!writeback.data.empty()) {
                if (writeback.data.size() != memory_.page_size() || writeback.version != version ||
                    writeback.region_id != request.region_id ||
                    writeback.region_offset != request.region_offset || writeback.page_id != page_id) {
                    error = "write-back incompatible con la version del directorio";
                    return finish(false);
                }
                backing_.at(page_id) = std::move(writeback.data);
            }
            // Un holder todavia descargando no tiene cambios; su respuesta se descartara.
            current.current_holder = 0;
            if (!write) current.sharers.insert(target);
        }
        if (write) current.sharers.erase(target);
        pages_.at(page_id).sharers = current.sharers;
        if (std::chrono::steady_clock::now() > deadline) {
            error = "timeout esperando confirmaciones de invalidacion";
            return finish(false);
        }
    }
    auto& current = controls_.at(page_id);
    auto& local = pages_.at(page_id);
    const auto& content = backing_.at(page_id);
    const uint64_t next_version = version + (write ? 1 : 0);
    response.page_id = request.page_id;
    response.region_id = request.region_id;
    response.region_offset = request.region_offset;
    response.version = next_version;
    response.state = static_cast<uint8_t>(write ? MSIState::MODIFIED : MSIState::SHARED);
    response.data = content;
    response.data_size = static_cast<uint32_t>(content.size());
    if (!write || requester == local_id_) {
        const int prot = PROT_READ | (write ? PROT_WRITE : 0);
        if (!memory_.install_page(page_id, content, prot, error)) return finish(false);
        local.state = write ? MSIState::MODIFIED : MSIState::SHARED;
        local.locally_present = true;
    }
    current.version = next_version;
    current.current_holder = write ? requester : 0;
    if (write) current.sharers.clear();
    else if (requester != local_id_) current.sharers.insert(requester);
    current.owner_state = local.state;
    local.version = next_version;
    local.sharers = current.sharers;
    return finish(true);
}

bool Pager::invalidate_page(const InvalidateMessage& message, NodeId sender,
                            PageData& writeback, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto region = regions_.find(message.region_id);
    if (region == regions_.end() || sender == local_id_ ||
        region->second.descriptor.owner_id != sender ||
        message.region_offset >= region->second.page_count ||
        (message.target_state != static_cast<uint8_t>(MSIState::INVALID) &&
         message.target_state != static_cast<uint8_t>(MSIState::SHARED))) {
        error = "INVALIDATE no corresponde al dueno o a la region";
        return false;
    }
    const PageId page_id = region->second.first_page + message.region_offset;
    auto& page = pages_.at(page_id);
    if (message.new_version < page.version) {
        error = "INVALIDATE con version antigua";
        return false;
    }
    auto& sync = sync_[page_id];
    ++sync.epoch;
    sync.invalidation_version = message.new_version;
    sync.grant_recipient = message.new_owner;
    auto& saved = sync.writeback;
    if (page.state != MSIState::INVALID) {
        if (!memory_.protect_page(page_id, PROT_READ, error)) return false;
        page.state = MSIState::SHARED;
        saved.version = page.version;
        saved.data.resize(memory_.page_size());
        std::memcpy(saved.data.data(), memory_.page_address(page_id), saved.data.size());
    }
    const bool shared = message.target_state == static_cast<uint8_t>(MSIState::SHARED) &&
                        page.state != MSIState::INVALID;
    if (!memory_.protect_page(page_id, shared ? PROT_READ : PROT_NONE, error)) return false;
    page.state = shared ? MSIState::SHARED : MSIState::INVALID;
    page.locally_present = shared;
    // Se conserva el contenido retirado para repetir un ACK que se perdio en la red.
    writeback = saved;
    const uint64_t expected_version = message.new_version -
        (message.target_state == static_cast<uint8_t>(MSIState::INVALID) && message.new_version > 0 ? 1 : 0);
    if (writeback.version != expected_version) writeback.data.clear();
    writeback.page_id = message.page_id;
    writeback.region_id = message.region_id;
    writeback.region_offset = message.region_offset;
    writeback.state = static_cast<uint8_t>(page.state);
    writeback.data_size = static_cast<uint32_t>(writeback.data.size());
    return true;
}

bool Pager::control_info(PageId page_id, GlobalPageControl& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = controls_.find(page_id);
    if (found == controls_.end()) return false;
    out = found->second;
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
