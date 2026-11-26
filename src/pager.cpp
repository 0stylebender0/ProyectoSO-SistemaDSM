#include "pager.hpp"

#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
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

    // Las tablas se protegen con semaforo POSIX ademas del mutex; el semaforo
    // se libera siempre, incluso si una excepcion aborta la operacion.
    class TableGuard {
    public:
        TableGuard(std::mutex& mutex, sem_t& semaphore) : mutex_(mutex), semaphore_(semaphore) {
            if (sem_wait(&semaphore_) == -1) throw std::runtime_error("sem_wait de tablas fallo");
            mutex_.lock();
        }
        ~TableGuard() {
            mutex_.unlock();
            sem_post(&semaphore_);
        }
        TableGuard(const TableGuard&) = delete;
        TableGuard& operator=(const TableGuard&) = delete;

    private:
        std::mutex& mutex_;
        sem_t& semaphore_;
    };

    bool same_descriptor(const RegionDescriptor& a, const RegionDescriptor& b) {
        return a.region_id == b.region_id && a.owner_id == b.owner_id &&
               a.size == b.size && a.page_size == b.page_size;
    }
}  // namespace

Pager::Pager(MemoryManager& memory, NodeId local_id, uint32_t timeout_ms)
    : memory_(memory), local_id_(local_id), timeout_ms_(timeout_ms) {
    if (sem_init(&table_semaphore_, 0, 1) == -1) {
        throw std::runtime_error(std::string("sem_init de tablas fallo: ") + std::strerror(errno));
    }
    if (memory_.page_count()) free_ranges_.emplace(0, memory_.page_count());
}

Pager::~Pager() {
    sem_destroy(&table_semaphore_);
}

void Pager::set_page_fetcher(PageFetcher fetcher) {
    fetcher_ = std::move(fetcher);
}

void Pager::set_page_invalidator(PageInvalidator invalidator) {
    invalidator_ = std::move(invalidator);
}

void Pager::set_page_releaser(PageReleaser releaser) {
    releaser_ = std::move(releaser);
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

    TableGuard lock(mutex_, table_semaphore_);

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

    if (!allocate_locked(descriptor, error)) return false;
    RegionInfo& created = regions_.at(id);
    created.creator_pid = static_cast<uint32_t>(getpid());
    created.allocation_order = ++next_allocation_order_;
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

    TableGuard lock(mutex_, table_semaphore_);

    const auto found = regions_.find(descriptor.region_id);
    if (found != regions_.end()) {
        if (releasing_.count(descriptor.region_id)) { error = "region en liberacion"; return false; }
        if (same_descriptor(found->second.descriptor, descriptor)) return true;
        error = "RegionId en conflicto: " + std::to_string(descriptor.region_id);
        return false;
    }

    return allocate_locked(descriptor, error);
}

bool Pager::allocate_locked(const RegionDescriptor& descriptor, std::string& error) {
    const uint64_t count = descriptor.size / descriptor.page_size;
    for (auto range = free_ranges_.begin(); range != free_ranges_.end(); ++range) {
        if (range->second < count) continue;
        const PageId first = range->first;
        const uint64_t remaining = range->second - count;
        if (!attach_locked(descriptor, first, error)) return false;
        free_ranges_.erase(range);
        if (remaining) free_ranges_.emplace(first + count, remaining);
        return true;
    }
    error = "no hay espacio local para la region";
    return false;
}

bool Pager::release_region(RegionId id, std::string& error) {
    RegionInfo region;
    std::vector<std::shared_ptr<PageEntry>> entries;
    {
        TableGuard lock(mutex_, table_semaphore_);
        const auto found = regions_.find(id);
        if (found == regions_.end()) { error = "region desconocida"; return false; }
        if (!releasing_.insert(id).second) { error = "region en liberacion"; return false; }
        region = found->second;
        for (uint64_t i = 0; i < region.page_count; ++i) entries.push_back(pages_.at(region.first_page + i));
    }
    auto rollback = [&] {
        for (const auto& entry : entries) {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->releasing = false;
            entry->retired = false;
        }
        TableGuard lock(mutex_, table_semaphore_);
        releasing_.erase(id);
        return false;
    };
    for (const auto& entry : entries) {
        bool busy;
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            busy = entry->pending != nullptr || entry->control.write_in_progress;
            if (!busy) entry->releasing = true;
        }
        if (busy) { error = "region en uso"; return rollback(); }
    }
    for (const auto& entry : entries) {
        bool ok = true;
        {
            std::unique_lock<std::mutex> lock(entry->mutex);
            if (region.descriptor.owner_id == local_id_) {
                if (!entry->control.sharers.empty() ||
                    (entry->control.current_holder != 0 && entry->control.current_holder != local_id_)) {
                    PageRequest request;
                    request.page_id = entry->page.page_id;
                    request.region_id = id;
                    request.region_offset = request.page_id - region.first_page;
                    request.access_type = 1;
                    PageData response;
                    ok = serve_locked(request, local_id_, *entry, response, lock, error);
                }
            } else {
                auto& saved = entry->sync.writeback;
                if (entry->page.state != MSIState::INVALID) {
                    ok = memory_.protect_page(entry->page.page_id, PROT_READ, error);
                    if (ok) {
                        saved.version = entry->page.version;
                        saved.data.resize(memory_.page_size());
                        std::memcpy(saved.data.data(), memory_.page_address(entry->page.page_id), saved.data.size());
                    }
                }
                if (ok) ok = memory_.protect_page(entry->page.page_id, PROT_NONE, error);
                if (ok) {
                    entry->page.state = MSIState::INVALID;
                    entry->page.locally_present = false;
                    PageData data = saved;
                    data.page_id = entry->page.page_id;
                    data.region_id = id;
                    data.region_offset = data.page_id - region.first_page;
                    data.state = static_cast<uint8_t>(MSIState::INVALID);
                    data.data_size = static_cast<uint32_t>(data.data.size());
                    if (data.data.empty()) data.version = entry->page.version;
                    lock.unlock();
                    if (!releaser_) { error = "no hay releaser configurado"; ok = false; }
                    else ok = releaser_(region.descriptor.owner_id, data, error);
                }
            }
        }
        if (!ok) return rollback();
    }
    // Un callback que conserve la entrada antigua no puede tocar el espacio reutilizado.
    for (const auto& entry : entries) {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->retired = true;
    }
    if (!memory_.reset_pages(region.first_page, region.page_count, error)) return rollback();
    TableGuard lock(mutex_, table_semaphore_);
    for (uint64_t i = 0; i < region.page_count; ++i) pages_.erase(region.first_page + i);
    regions_.erase(id);
    releasing_.erase(id);
    PageId first = region.first_page;
    uint64_t count = region.page_count;
    auto next = free_ranges_.lower_bound(first);
    if (next != free_ranges_.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == first) {
            first = previous->first;
            count += previous->second;
            free_ranges_.erase(previous);
        }
    }
    if (next != free_ranges_.end() && first + count == next->first) {
        count += next->second;
        free_ranges_.erase(next);
    }
    free_ranges_.emplace(first, count);
    return true;
}

bool Pager::accept_page_release(const PageData& data, NodeId sender, std::string& error) {
    auto entry = find_region_page(data.region_id, data.region_offset);
    if (!entry || sender == 0 || sender == local_id_ || entry->region.descriptor.owner_id != local_id_) {
        error = "PAGE_RELEASE no corresponde al directorio";
        return false;
    }
    std::unique_lock<std::mutex> lock(entry->mutex);
    if (!entry->ready.wait_for(lock, std::chrono::milliseconds(timeout_ms_), [&] {
            return !entry->control.write_in_progress;
        }) || entry->releasing || entry->retired) {
        error = "region en uso";
        return false;
    }
    auto& control = entry->control;
    if (control.current_holder == sender) {
        if (!data.data.empty()) {
            if (data.data.size() != memory_.page_size() || data.version != control.version) {
                error = "PAGE_RELEASE con write-back invalido";
                return false;
            }
            entry->backing = data.data;
        }
        control.current_holder = 0;
    }
    control.sharers.erase(sender);
    entry->page.sharers = control.sharers;
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
        page.creator_pid = region.creator_pid;
        page.allocation_order = region.allocation_order;

        auto entry = std::make_shared<PageEntry>();
        entry->page = page;
        entry->region = region;
        if (!pages_.emplace(page.page_id, entry).second) {
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
            entry->control = control;
        }
    }

    return true;
}

std::shared_ptr<Pager::PageEntry> Pager::find_page(PageId page_id) const {
    TableGuard lock(mutex_, table_semaphore_);
    const auto found = pages_.find(page_id);
    return found == pages_.end() ? nullptr : found->second;
}

std::shared_ptr<Pager::PageEntry> Pager::find_region_page(RegionId id, uint64_t offset) const {
    TableGuard lock(mutex_, table_semaphore_);
    const auto region = regions_.find(id);
    if (region == regions_.end() || offset >= region->second.page_count) return nullptr;
    return pages_.at(region->second.first_page + offset);
}

bool Pager::resolve_fault(PageId page_id, bool write, std::string& error) {
    statistics_.fault();
    auto entry = find_page(page_id);
    if (!entry) {
        error = "pagina desconocida: " + std::to_string(page_id);
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
    std::unique_lock<std::mutex> lock(entry->mutex);
    while (entry->pending) {
        statistics_.coalesced();
        auto pending = entry->pending;
        if (!pending->ready.wait_until(lock, deadline, [&] { return pending->done; })) {
            error = "timeout esperando un fallo pendiente";
            return false;
        }
        if (!pending->success) {
            error = pending->error;
            return false;
        }
    }
    if (entry->releasing || entry->retired) { error = "region liberada o en liberacion"; return false; }
    auto pending = std::make_shared<PendingFault>();
    entry->pending = pending;
    auto finish = [&](bool success) {
        pending->success = success;
        pending->error = success ? "" : error;
        pending->done = true;
        entry->pending.reset();
        pending->ready.notify_all();
        return success;
    };
    while (true) {
        PageInfo& page = entry->page;
        if (page.owner_id == local_id_) {
            if (!entry->ready.wait_until(lock, deadline, [&] {
                    return !entry->control.write_in_progress;
                })) {
                error = "timeout esperando el directorio local";
                return finish(false);
            }
        }
        if (page.state == MSIState::MODIFIED || (!write && page.state == MSIState::SHARED)) {
            return finish(true);
        }
        const RegionInfo& region = entry->region;
        PageRequest request;
        request.page_id = page_id;
        request.region_id = page.region_id;
        request.region_offset = page_id - region.first_page;
        request.access_type = write ? 1 : 0;
        request.current_state = static_cast<uint8_t>(page.state);
        request.known_version = page.version;
        if (page.owner_id == local_id_) {
            PageData response;
            return finish(serve_locked(request, local_id_, *entry, response, lock, error));
        }
        if (!fetcher_) {
            error = "no hay fetcher configurado para paginas remotas";
            return finish(false);
        }
        RemotePageQuery query;
        query.page_id = page_id;
        query.region_id = page.region_id;
        query.region_offset = request.region_offset;
        query.owner_id = page.owner_id;
        query.current_state = page.state;
        query.known_version = page.version;
        query.write = write;
        const uint64_t epoch = entry->sync.epoch;
        lock.unlock();
        statistics_.request();
        PageData response;
        const bool fetched = fetcher_(query, response, error);
        lock.lock();
        if (!fetched) return finish(false);
        // Invalidar nuestra copia S forma parte de concedernos M.
        // Las invalidaciones de otras operaciones descartan la descarga pendiente.
        const auto& sync = entry->sync;
        const bool own_grant = sync.grant_recipient == local_id_ &&
                               sync.invalidation_version == response.version;
        if (epoch != sync.epoch && !own_grant) {
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "timeout reintentando una pagina invalidada durante la descarga";
                return finish(false);
            }
            continue;
        }
        const auto expected = write ? MSIState::MODIFIED : MSIState::SHARED;
        if (response.page_id != page_id || response.region_id != query.region_id ||
            response.region_offset != query.region_offset ||
            response.state != static_cast<uint8_t>(expected) ||
            response.version < page.version || response.data.size() != memory_.page_size() ||
            response.data_size != response.data.size()) {
            error = "PAGE_DATA no corresponde a la pagina solicitada";
            return finish(false);
        }
        const int prot = PROT_READ | (write ? PROT_WRITE : 0);
        if (!memory_.install_page(page_id, response.data, prot, error)) return finish(false);
        page.state = expected;
        page.version = response.version;
        page.locally_present = true;
        entry->sync.writeback = PageData{};
        statistics_.transfer();
        return finish(true);
    }
}

bool Pager::serve_page_request(const PageRequest& request, NodeId requester,
                               PageData& response, std::string& error) {
    auto entry = find_region_page(request.region_id, request.region_offset);
    if (requester == 0 || request.access_type > 1 || !entry ||
        entry->region.descriptor.owner_id != local_id_) {
        error = "PAGE_REQUEST con region, pagina o solicitante invalido";
        return false;
    }
    std::unique_lock<std::mutex> lock(entry->mutex);
    if (!entry->ready.wait_for(lock, std::chrono::milliseconds(timeout_ms_), [&] {
            return !entry->control.write_in_progress;
        })) {
        error = "timeout esperando una transferencia de pagina";
        return false;
    }
    if (entry->releasing || entry->retired) { error = "region liberada o en liberacion"; return false; }
    return serve_locked(request, requester, *entry, response, lock, error);
}

bool Pager::serve_locked(const PageRequest& request, NodeId requester, PageEntry& entry,
                         PageData& response, std::unique_lock<std::mutex>& lock,
                         std::string& error) {
    const PageId page_id = entry.page.page_id;
    auto& control = entry.control;
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
        entry.control.write_in_progress = false;
        entry.ready.notify_all();
        return ok;
    };
    auto& page = entry.page;
    auto& data = entry.backing;
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
        statistics_.invalidation_sent();
        PageData writeback;
        const bool invalidated = invalidator_(target, message, writeback, error);
        lock.lock();
        if (!invalidated) return finish(false);
        auto& current = entry.control;
        if (target == holder) {
            if (!writeback.data.empty()) {
                if (writeback.data.size() != memory_.page_size() || writeback.version != version ||
                    writeback.region_id != request.region_id ||
                    writeback.region_offset != request.region_offset || writeback.page_id != page_id) {
                    error = "write-back incompatible con la version del directorio";
                    return finish(false);
                }
                entry.backing = std::move(writeback.data);
            }
            // Un holder todavia descargando no tiene cambios; su respuesta se descartara.
            current.current_holder = 0;
            if (!write) current.sharers.insert(target);
        }
        if (write) current.sharers.erase(target);
        entry.page.sharers = current.sharers;
        if (std::chrono::steady_clock::now() > deadline) {
            error = "timeout esperando confirmaciones de invalidacion";
            return finish(false);
        }
    }
    auto& current = entry.control;
    auto& local = entry.page;
    const auto& content = entry.backing;
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
    auto entry = find_region_page(message.region_id, message.region_offset);
    if (!entry || sender == local_id_ || entry->region.descriptor.owner_id != sender ||
        (message.target_state != static_cast<uint8_t>(MSIState::INVALID) &&
         message.target_state != static_cast<uint8_t>(MSIState::SHARED))) {
        error = "INVALIDATE no corresponde al dueno o a la region";
        return false;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->retired) { error = "region liberada"; return false; }
    const PageId page_id = entry->page.page_id;
    auto& page = entry->page;
    if (message.new_version < page.version) {
        error = "INVALIDATE con version antigua";
        return false;
    }
    statistics_.invalidation_received();
    auto& sync = entry->sync;
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
    auto entry = find_page(page_id);
    if (!entry || entry->region.descriptor.owner_id != local_id_) return false;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->retired) return false;
    out = entry->control;
    return true;
}

StatisticsSnapshot Pager::statistics() const {
    StatisticsSnapshot result = statistics_.snapshot();
    const auto pages = page_snapshot();
    for (const auto& page : pages) {
        if (page.owner_id == local_id_) ++result.pages_owned;
        if (page.state == MSIState::SHARED && page.locally_present) ++result.pages_shared;
    }
    return result;
}

bool Pager::page_info(PageId page_id, PageInfo& out) const {
    auto entry = find_page(page_id);
    if (!entry) return false;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->retired) return false;
    out = entry->page;
    return true;
}

std::vector<PageInfo> Pager::page_snapshot() const {
    std::vector<std::shared_ptr<PageEntry>> entries;
    {
        TableGuard lock(mutex_, table_semaphore_);
        for (const auto& entry : pages_) entries.push_back(entry.second);
    }
    std::vector<PageInfo> result;
    result.reserve(entries.size());
    for (const auto& entry : entries) {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->retired) result.push_back(entry->page);
    }
    return result;
}

bool Pager::region_info(RegionId region_id, RegionInfo& out) const {
    TableGuard lock(mutex_, table_semaphore_);
    const auto found = regions_.find(region_id);
    if (found == regions_.end()) return false;
    out = found->second;
    return true;
}

std::vector<RegionInfo> Pager::region_snapshot() const {
    TableGuard lock(mutex_, table_semaphore_);
    std::vector<RegionInfo> result;
    result.reserve(regions_.size());
    for (const auto& entry : regions_) result.push_back(entry.second);
    return result;
}

bool Pager::assignment(PageId page_id, PageAssignment& out) const {
    auto entry = find_page(page_id);
    if (!entry) return false;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->retired) return false;
    out.page_id = page_id;
    out.region_id = entry->page.region_id;
    out.owner_id = entry->page.owner_id;
    out.region_offset = page_id - entry->region.first_page;
    return true;
}
