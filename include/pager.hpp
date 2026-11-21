#pragma once

#include "memory_manager.hpp"
#include "page.hpp"
#include "protocol.hpp"
#include "statistics.hpp"

#include <functional>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

struct RemotePageQuery {
    PageId page_id = 0;
    RegionId region_id = 0;
    uint64_t region_offset = 0;
    NodeId owner_id = 0;
    MSIState current_state = MSIState::INVALID;
    uint64_t known_version = 0;
    bool write = false;
};

using PageFetcher =
    std::function<bool(const RemotePageQuery&, PageData&, std::string&)>;
using PageInvalidator = std::function<bool(
    NodeId, const InvalidateMessage&, PageData&, std::string&)>;

class Pager {
public:
    Pager(MemoryManager& memory, NodeId local_id, uint32_t timeout_ms = 5000);

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    // Se configura antes de que el FaultWorker pueda atender un fallo remoto.
    void set_page_fetcher(PageFetcher fetcher);
    void set_page_invalidator(PageInvalidator invalidator);

    bool create_region(uint64_t size, RegionId& region_id, std::string& error);
    bool attach_region(const RegionDescriptor& descriptor, std::string& error);
    bool resolve_fault(PageId page_id, bool write, std::string& error);

    bool serve_page_request(const PageRequest& request, NodeId requester,
                            PageData& response, std::string& error);
    bool invalidate_page(const InvalidateMessage& message, NodeId sender,
                         PageData& writeback, std::string& error);
    bool control_info(PageId page_id, GlobalPageControl& out) const;
    StatisticsSnapshot statistics() const { return statistics_.snapshot(); }

    bool page_info(PageId page_id, PageInfo& out) const;
    std::vector<PageInfo> page_snapshot() const;

    bool region_info(RegionId region_id, RegionInfo& out) const;
    std::vector<RegionInfo> region_snapshot() const;

    bool assignment(PageId page_id, PageAssignment& out) const;

    NodeId local_id() const { return local_id_; }
    uint32_t page_size() const { return memory_.page_size(); }

private:
    struct PageEntry;
    std::shared_ptr<PageEntry> find_page(PageId page_id) const;
    std::shared_ptr<PageEntry> find_region_page(RegionId region_id, uint64_t offset) const;
    bool attach_locked(const RegionDescriptor& descriptor, uint64_t first_page,
                       std::string& error);
    bool serve_locked(const PageRequest& request, NodeId requester, PageEntry& entry,
                      PageData& response, std::unique_lock<std::mutex>& lock,
                      std::string& error);

    struct PageSync {
        uint64_t epoch = 0;
        uint64_t invalidation_version = 0;
        NodeId grant_recipient = 0;
        PageData writeback;
    };

    struct PendingFault {
        std::condition_variable ready;
        bool done = false;
        bool success = false;
        std::string error;
    };

    struct PageEntry {
        mutable std::mutex mutex;
        std::condition_variable ready;
        PageInfo page;
        RegionInfo region;
        PageSync sync;
        GlobalPageControl control;
        std::vector<uint8_t> backing;
        std::shared_ptr<PendingFault> pending;
    };

    MemoryManager& memory_;
    NodeId local_id_;
    PageFetcher fetcher_;
    PageInvalidator invalidator_;
    uint32_t timeout_ms_;

    // El mutex de tablas se libera antes de tomar el de una pagina.
    // Ningun mutex del Pager se mantiene durante llamadas de red.
    mutable std::mutex mutex_;
    std::unordered_map<PageId, std::shared_ptr<PageEntry>> pages_;
    RegionTable regions_;
    Statistics statistics_;
    uint64_t next_free_page_ = 0;
};
