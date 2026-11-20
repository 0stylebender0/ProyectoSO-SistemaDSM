#pragma once

#include "memory_manager.hpp"
#include "page.hpp"

#include <mutex>
#include <string>
#include <vector>

class Pager {
public:
    Pager(MemoryManager& memory, NodeId local_id);

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    bool create_region(uint64_t size, RegionId& region_id, std::string& error);
    bool attach_region(const RegionDescriptor& descriptor, std::string& error);
    bool resolve_fault(PageId page_id, bool write, std::string& error);

    bool page_info(PageId page_id, PageInfo& out) const;
    std::vector<PageInfo> page_snapshot() const;

    bool region_info(RegionId region_id, RegionInfo& out) const;
    std::vector<RegionInfo> region_snapshot() const;

    bool assignment(PageId page_id, PageAssignment& out) const;

    NodeId local_id() const { return local_id_; }
    uint32_t page_size() const { return memory_.page_size(); }

private:
    bool attach_locked(const RegionDescriptor& descriptor, uint64_t first_page,
                       std::string& error);

    MemoryManager& memory_;
    NodeId local_id_;

    // El hito 2.3 solo tiene un FaultWorker y no hace red bajo el lock.
    // En 2.6 este mutex se sustituye por bloqueos por pagina.
    mutable std::mutex mutex_;
    LocalPageTable pages_;
    RegionTable regions_;
    uint64_t next_free_page_ = 0;
};
