#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using DSMRegionId = uint64_t;
using DSMNodeId = uint64_t;

enum class DSMAccess { READ, WRITE, READ_WRITE };

enum class DSMError {
    SUCCESS,
    INITIALIZATION_ERROR,
    OUT_OF_MEMORY,
    INVALID_REGION,
    INVALID_ACCESS,
    NETWORK_ERROR,
    NODE_UNAVAILABLE,
    PAGE_FAULT_ERROR,
    INTERNAL_ERROR,
    NOT_INITIALIZED,
    ALREADY_INITIALIZED,
    BUSY,
    INVALID_THREAD
};

struct DSMStatistics {
    uint64_t pages_owned = 0;
    uint64_t pages_shared = 0;
    uint64_t page_faults = 0;
    uint64_t coalesced_faults = 0;
    uint64_t page_requests = 0;
    uint64_t page_transfers = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    uint64_t invalidations_sent = 0;
    uint64_t invalidations_received = 0;
};

struct DSMNodeInfo {
    DSMNodeId id = 0;
    std::string node_name;
    std::string ip_address;
    uint16_t tcp_port = 0;
    uint16_t heartbeat_port = 0;
    uint64_t memory_size = 0;
    uint32_t page_size = 0;
};

namespace dsm_detail { struct RegionState; }

class DSMRegion {
public:
    DSMRegion() = default;
    DSMRegionId get_id() const;
    std::size_t get_size() const;
    void* data();
    const void* data() const;
    bool valid() const;

    template <typename T> T* as() { return static_cast<T*>(data()); }
    template <typename T> const T* as() const { return static_cast<const T*>(data()); }

private:
    friend class DSM;
    explicit DSMRegion(std::shared_ptr<dsm_detail::RegionState> state);
    std::shared_ptr<dsm_detail::RegionState> state_;
};

// Cada hilo adicional mantiene esta guardia mientras accede a memoria DSM.
class DSMThread {
public:
    DSMThread();
    ~DSMThread();
    DSMThread(const DSMThread&) = delete;
    DSMThread& operator=(const DSMThread&) = delete;
    bool valid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class DSM {
public:
    DSM() = delete;
    // initialize y shutdown se ejecutan en el mismo hilo; SIGUSR1 queda reservado.
    static DSMError initialize(const std::string& config_path);
    static DSMRegion create_region(std::size_t size, DSMAccess access = DSMAccess::READ_WRITE);
    static DSMRegion attach_region(DSMRegionId region_id, DSMNodeId owner_id);
    // La aplicacion debe detener los accesos a la region antes de liberarla.
    static DSMError release_region(DSMRegionId region_id);
    static DSMError shutdown();
    static DSMStatistics get_statistics();
    static DSMNodeInfo get_local_node();
    static std::vector<DSMNodeInfo> get_nodes();
    // El ultimo error pertenece al hilo que realizo la llamada.
    static DSMError last_error();
    static std::string last_error_message();
};
