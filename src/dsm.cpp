#include "dsm.hpp"

#include "config.hpp"
#include "dsm_node.hpp"
#include "signal_handler.hpp"
#include "status_report.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace dsm_detail {
    struct RegionState {
        DSMRegionId id;
        std::size_t size;
        void* address;
        mutable std::shared_mutex mutex;
        std::atomic<bool> active{true};
        RegionState(DSMRegionId region_id, std::size_t length, void* base)
            : id(region_id), size(length), address(base) {}
    };
}

namespace {
    thread_local DSMError error_code = DSMError::SUCCESS;
    thread_local std::string error_message;

    DSMError result(DSMError code, std::string message = {}) {
        error_code = code;
        error_message = std::move(message);
        return code;
    }

    bool failed(DSMError code, std::string message) {
        result(code, std::move(message));
        return false;
    }

    bool succeeded() {
        result(DSMError::SUCCESS);
        return true;
    }

    struct Runtime {
        Config config;
        std::thread::id owner = std::this_thread::get_id();
        std::unique_ptr<MemoryManager> memory;
        std::unique_ptr<NetworkManager> network;
        std::unique_ptr<DSMNode> node;
        std::unique_ptr<SignalHandler> faults;
        std::mutex regions_mutex;
        std::unordered_map<DSMRegionId, std::shared_ptr<dsm_detail::RegionState>> regions;
        std::size_t registered_threads = 0;
        bool closed = false;

        ~Runtime() {
            close();
            // close() puede fallar y dejar la red activa: detenerla antes de destruir node.
            if (network) network->stop();
            if (faults) faults->stop();
        }

        DSMError start(const std::string& path, std::string& error) {
            if (!load_config(path, config, error)) return DSMError::INITIALIZATION_ERROR;
            NodeId id;
            if (!generate_node_id(id, error)) return DSMError::INITIALIZATION_ERROR;
            memory = std::make_unique<MemoryManager>(config.memory_size, config.page_size);
            if (!memory->reserve(error)) return DSMError::OUT_OF_MEMORY;
            network = std::make_unique<NetworkManager>(config, id);
            node = std::make_unique<DSMNode>(*memory, *network, id);
            faults = std::make_unique<SignalHandler>(*memory,
                [this](PageId page, bool write, std::string& why) { return node->resolve_fault(page, write, why); });
            if (!faults->start(error)) return DSMError::INITIALIZATION_ERROR;
            if (!network->start(error)) return DSMError::NETWORK_ERROR;
            return DSMError::SUCCESS;
        }

        DSMError close(std::string* error = nullptr) {
            if (closed) return DSMError::SUCCESS;
            DSMError code = DSMError::SUCCESS;
            if (node) {
                auto entries = node->pager().region_snapshot();
                std::stable_sort(entries.begin(), entries.end(), [this](const RegionInfo& a, const RegionInfo& b) {
                    return (a.descriptor.owner_id != node->pager().local_id()) >
                           (b.descriptor.owner_id != node->pager().local_id());
                });
                for (const auto& entry : entries) {
                    std::shared_ptr<dsm_detail::RegionState> state;
                    {
                        std::lock_guard<std::mutex> lock(regions_mutex);
                        const auto found = regions.find(entry.descriptor.region_id);
                        if (found != regions.end()) state = found->second;
                    }
                    std::unique_lock<std::shared_mutex> region_lock;
                    if (state) region_lock = std::unique_lock<std::shared_mutex>(state->mutex);
                    std::string why;
                    if (!node->pager().release_region(entry.descriptor.region_id, why)) {
                        code = DSMError::NETWORK_ERROR;
                        if (error && error->empty()) *error = why;
                    }
                }
            }
            if (code != DSMError::SUCCESS) return code;
            {
                std::lock_guard<std::mutex> lock(regions_mutex);
                for (const auto& region : regions) region.second->active = false;
                regions.clear();
            }
            if (network) network->stop();
            if (faults) faults->stop();
            closed = true;
            return DSMError::SUCCESS;
        }

        std::shared_ptr<dsm_detail::RegionState> region_state(DSMRegionId id) {
            RegionInfo info;
            if (!node->pager().region_info(id, info)) return {};
            std::lock_guard<std::mutex> lock(regions_mutex);
            auto& state = regions[id];
            if (!state) state = std::make_shared<dsm_detail::RegionState>(
                id, info.descriptor.size, memory->page_address(info.first_page));
            return state;
        }
    };

    std::mutex api_mutex;
    std::condition_variable api_ready;
    std::shared_ptr<Runtime> instance;
    bool lifecycle_busy = false;
    std::size_t active_calls = 0;

    // El contador conserva el runtime sin mantener api_mutex durante la red.
    struct Call {
        std::shared_ptr<Runtime> runtime;
        Call() {
            std::lock_guard<std::mutex> lock(api_mutex);
            if (lifecycle_busy) result(DSMError::BUSY, "el ciclo de vida DSM esta cambiando");
            else if (!instance) result(DSMError::NOT_INITIALIZED, "DSM no esta inicializado");
            else { runtime = instance; ++active_calls; }
        }
        ~Call() {
            if (!runtime) return;
            std::lock_guard<std::mutex> lock(api_mutex);
            --active_calls;
            api_ready.notify_all();
        }
    };

    struct Registration {
        std::shared_ptr<Runtime> runtime;
        std::unique_ptr<FaultThread> thread;
        bool counted = false;
        ~Registration() {
            thread.reset();
            if (!counted) return;
            std::lock_guard<std::mutex> lock(api_mutex);
            --runtime->registered_threads;
        }
    };
    thread_local std::weak_ptr<Registration> registration;

    DSMNodeInfo public_node(const NodeInfo& node) {
        return {node.id, node.node_name, node.ip_address, node.tcp_port,
                node.heartbeat_port, node.memory_size, node.page_size};
    }

    DSMRegionInfo public_region(const RegionDescriptor& descriptor) {
        return {descriptor.region_id, descriptor.owner_id,
                static_cast<std::size_t>(descriptor.size), descriptor.page_size};
    }

    std::vector<DSMRegionInfo> public_regions(std::vector<RegionDescriptor> descriptors) {
        std::sort(descriptors.begin(), descriptors.end(),
                  [](const RegionDescriptor& a, const RegionDescriptor& b) {
                      return a.region_id < b.region_id;
                  });
        std::vector<DSMRegionInfo> regions;
        regions.reserve(descriptors.size());
        for (const auto& descriptor : descriptors) regions.push_back(public_region(descriptor));
        return regions;
    }
}

DSMRegion::DSMRegion(std::shared_ptr<dsm_detail::RegionState> state) : state_(std::move(state)) {}
DSMRegionId DSMRegion::get_id() const { return state_ ? state_->id : 0; }
std::size_t DSMRegion::get_size() const { return state_ ? state_->size : 0; }
bool DSMRegion::valid() const { return state_ && state_->active.load(); }

bool DSMRegion::read(std::size_t offset, void* buffer, std::size_t size) const {
    if (!state_) return failed(DSMError::INVALID_REGION, "region no inicializada");
    if (size > state_->size || offset > state_->size - size || (size != 0 && buffer == nullptr)) {
        return failed(DSMError::INVALID_REGION, "rango de lectura fuera de la region");
    }
    FaultAccess access;
    if (!begin_fault_access(access)) {
        return failed(DSMError::PAGE_FAULT_ERROR, "ya hay un acceso DSM activo en este hilo");
    }
    state_->mutex.lock_shared();
    if (sigsetjmp(access.jump, 1) != 0) {
        state_->mutex.unlock_shared();
        const std::string why = access.message();
        end_fault_access();
        return failed(DSMError::PAGE_FAULT_ERROR, why);
    }
    if (!state_->active.load()) {
        state_->mutex.unlock_shared();
        end_fault_access();
        return failed(DSMError::INVALID_REGION, "region liberada");
    }
    std::memcpy(buffer, static_cast<const char*>(state_->address) + offset, size);
    state_->mutex.unlock_shared();
    end_fault_access();
    return succeeded();
}

bool DSMRegion::write(std::size_t offset, const void* buffer, std::size_t size) {
    if (!state_) return failed(DSMError::INVALID_REGION, "region no inicializada");
    if (size > state_->size || offset > state_->size - size || (size != 0 && buffer == nullptr)) {
        return failed(DSMError::INVALID_REGION, "rango de escritura fuera de la region");
    }
    FaultAccess access;
    if (!begin_fault_access(access)) {
        return failed(DSMError::PAGE_FAULT_ERROR, "ya hay un acceso DSM activo en este hilo");
    }
    state_->mutex.lock_shared();
    if (sigsetjmp(access.jump, 1) != 0) {
        state_->mutex.unlock_shared();
        const std::string why = access.message();
        end_fault_access();
        return failed(DSMError::PAGE_FAULT_ERROR, why);
    }
    if (!state_->active.load()) {
        state_->mutex.unlock_shared();
        end_fault_access();
        return failed(DSMError::INVALID_REGION, "region liberada");
    }
    std::memcpy(static_cast<char*>(state_->address) + offset, buffer, size);
    state_->mutex.unlock_shared();
    end_fault_access();
    return succeeded();
}

struct DSMThread::Impl { std::shared_ptr<Registration> registration; };

DSMThread::DSMThread() {
    Call call;
    if (!call.runtime) return;
    auto current = registration.lock();
    if (!current) {
        current = std::make_shared<Registration>();
        current->runtime = call.runtime;
        if (std::this_thread::get_id() != call.runtime->owner) {
            current->thread = std::make_unique<FaultThread>();
            std::string error;
            if (!current->thread->prepare(error)) {
                result(DSMError::PAGE_FAULT_ERROR, error);
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(api_mutex);
            ++call.runtime->registered_threads;
            current->counted = true;
        }
        registration = current;
    }
    impl_ = std::make_unique<Impl>();
    impl_->registration = std::move(current);
    result(DSMError::SUCCESS);
}

DSMThread::~DSMThread() = default;
bool DSMThread::valid() const { return impl_ != nullptr; }

DSMError DSM::initialize(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(api_mutex);
        if (lifecycle_busy) return result(DSMError::BUSY, "el ciclo de vida DSM esta cambiando");
        if (instance) return result(DSMError::ALREADY_INITIALIZED, "DSM ya esta inicializado");
        lifecycle_busy = true;
    }
    std::shared_ptr<Runtime> fresh;
    std::string error;
    DSMError code;
    try {
        fresh = std::make_shared<Runtime>();
        code = fresh->start(path, error);
    } catch (const std::bad_alloc&) {
        code = DSMError::OUT_OF_MEMORY;
        error = "no hay memoria para inicializar DSM";
    } catch (const std::exception& exception) {
        code = DSMError::INITIALIZATION_ERROR;
        error = exception.what();
    }
    if (code != DSMError::SUCCESS && fresh) fresh->close();
    {
        std::lock_guard<std::mutex> lock(api_mutex);
        if (code == DSMError::SUCCESS) instance = std::move(fresh);
        lifecycle_busy = false;
    }
    return result(code, error);
}

DSMRegion DSM::create_region(std::size_t size, DSMAccess access) {
    Call call;
    if (!call.runtime) return {};
    if (access != DSMAccess::READ_WRITE) {
        result(DSMError::INVALID_ACCESS, "esta version solo admite DSMAccess::READ_WRITE");
        return {};
    }
    if (size == 0) { result(DSMError::INVALID_REGION, "el tamano debe ser mayor que cero"); return {}; }
    RegionId id;
    std::string error;
    if (!call.runtime->node->create_region(size, id, error)) {
        result(DSMError::OUT_OF_MEMORY, error);
        return {};
    }
    auto state = call.runtime->region_state(id);
    result(DSMError::SUCCESS);
    return DSMRegion(std::move(state));
}

DSMRegion DSM::attach_region(DSMRegionId id, DSMNodeId owner) {
    Call call;
    if (!call.runtime) return {};
    if (id == 0 || owner == 0) { result(DSMError::INVALID_REGION, "identificadores invalidos"); return {}; }
    RegionInfo info;
    if (call.runtime->node->pager().region_info(id, info)) {
        if (info.descriptor.owner_id != owner) {
            result(DSMError::INVALID_REGION, "la region tiene otro dueno"); return {};
        }
    } else {
        NodeInfo peer;
        if (!call.runtime->network->find_node(owner, peer)) {
            result(DSMError::NODE_UNAVAILABLE, "nodo dueno desconocido"); return {};
        }
        std::string error;
        if (!call.runtime->node->attach_region_from(owner, id, error)) {
            result(DSMError::NETWORK_ERROR, error); return {};
        }
    }
    auto state = call.runtime->region_state(id);
    result(DSMError::SUCCESS);
    return DSMRegion(std::move(state));
}

DSMError DSM::release_region(DSMRegionId id) {
    Call call;
    if (!call.runtime) return last_error();
    RegionInfo info;
    if (!call.runtime->node->pager().region_info(id, info)) {
        return result(DSMError::INVALID_REGION, "region desconocida o liberada");
    }
    std::shared_ptr<dsm_detail::RegionState> state;
    {
        std::lock_guard<std::mutex> lookup(call.runtime->regions_mutex);
        const auto found = call.runtime->regions.find(id);
        if (found != call.runtime->regions.end()) state = found->second;
    }
    std::unique_lock<std::shared_mutex> region_lock;
    if (state) region_lock = std::unique_lock<std::shared_mutex>(state->mutex);
    std::string error;
    if (!call.runtime->node->pager().release_region(id, error)) {
        return result(error == "region en uso" || error == "region en liberacion" ?
                      DSMError::BUSY : DSMError::NETWORK_ERROR, error);
    }
    std::lock_guard<std::mutex> lock(call.runtime->regions_mutex);
    const auto found = call.runtime->regions.find(id);
    if (found != call.runtime->regions.end()) {
        found->second->active = false;
        call.runtime->regions.erase(found);
    }
    return result(DSMError::SUCCESS);
}

DSMError DSM::shutdown() {
    std::shared_ptr<Runtime> current;
    {
        std::unique_lock<std::mutex> lock(api_mutex);
        if (lifecycle_busy) return result(DSMError::BUSY, "el ciclo de vida DSM esta cambiando");
        if (!instance) return result(DSMError::SUCCESS);
        if (std::this_thread::get_id() != instance->owner) {
            return result(DSMError::INVALID_THREAD, "shutdown debe ejecutarse en el hilo de initialize");
        }
        if (instance->registered_threads != 0) return result(DSMError::BUSY, "hay guardias DSMThread activas");
        lifecycle_busy = true;
        if (!api_ready.wait_for(lock, std::chrono::milliseconds(instance->config.request_timeout_ms),
                                [] { return active_calls == 0; })) {
            lifecycle_busy = false;
            return result(DSMError::BUSY, "hay llamadas DSM pendientes");
        }
        // Una llamada de registro que ya estaba activa pudo crear una guardia.
        if (instance->registered_threads != 0) {
            lifecycle_busy = false;
            return result(DSMError::BUSY, "hay guardias DSMThread activas");
        }
        current = std::move(instance);
    }
    std::string error;
    const auto code = current->close(&error);
    {
        std::lock_guard<std::mutex> lock(api_mutex);
        if (code != DSMError::SUCCESS) instance = std::move(current);
        lifecycle_busy = false;
    }
    return result(code, error);
}

DSMStatistics DSM::get_statistics() {
    Call call;
    if (!call.runtime) return {};
    const auto stats = status_report::combined_statistics(call.runtime->node->pager(), *call.runtime->network);
    result(DSMError::SUCCESS);
    return {stats.pages_owned, stats.pages_shared, stats.page_faults, stats.coalesced_faults,
            stats.page_requests, stats.page_transfers, stats.bytes_sent, stats.bytes_received,
            stats.invalidations_sent, stats.invalidations_received};
}

DSMNodeInfo DSM::get_local_node() {
    Call call;
    if (!call.runtime) return {};
    NodeInfo node;
    call.runtime->network->find_node(call.runtime->network->node_id(), node);
    result(DSMError::SUCCESS);
    return public_node(node);
}

std::vector<DSMNodeInfo> DSM::get_nodes() {
    Call call;
    if (!call.runtime) return {};
    std::vector<DSMNodeInfo> nodes;
    for (const auto& node : call.runtime->network->nodes()) nodes.push_back(public_node(node));
    result(DSMError::SUCCESS);
    return nodes;
}

std::vector<DSMRegionInfo> DSM::get_regions() {
    Call call;
    if (!call.runtime) return {};
    std::vector<RegionDescriptor> descriptors;
    for (const auto& region : call.runtime->node->pager().region_snapshot()) {
        descriptors.push_back(region.descriptor);
    }
    result(DSMError::SUCCESS);
    return public_regions(std::move(descriptors));
}

std::vector<DSMRegionInfo> DSM::get_remote_regions(DSMNodeId owner_id) {
    Call call;
    if (!call.runtime) return {};
    if (owner_id == 0) {
        result(DSMError::INVALID_REGION, "el identificador del dueno no puede ser cero");
        return {};
    }
    if (owner_id == call.runtime->network->node_id()) {
        std::vector<RegionDescriptor> descriptors;
        for (const auto& region : call.runtime->node->pager().region_snapshot()) {
            if (region.descriptor.owner_id == owner_id) {
                descriptors.push_back(region.descriptor);
            }
        }
        result(DSMError::SUCCESS);
        return public_regions(std::move(descriptors));
    }
    NodeInfo owner;
    if (!call.runtime->network->find_node(owner_id, owner)) {
        result(DSMError::NODE_UNAVAILABLE, "nodo dueno desconocido");
        return {};
    }
    if (!call.runtime->network->node_available(owner_id)) {
        result(DSMError::NODE_UNAVAILABLE, "nodo dueno no disponible segun heartbeat");
        return {};
    }
    std::vector<RegionDescriptor> descriptors;
    std::string error;
    if (!call.runtime->network->query_regions(owner, descriptors, error)) {
        result(DSMError::NETWORK_ERROR, error);
        return {};
    }
    // La lista del par incluye regiones de otros duenos que ese nodo adjunto.
    descriptors.erase(std::remove_if(descriptors.begin(), descriptors.end(),
                                     [owner_id](const RegionDescriptor& descriptor) {
                                         return descriptor.owner_id != owner_id;
                                     }), descriptors.end());
    result(DSMError::SUCCESS);
    return public_regions(std::move(descriptors));
}

DSMError DSM::last_error() { return error_code; }
std::string DSM::last_error_message() { return error_message; }
