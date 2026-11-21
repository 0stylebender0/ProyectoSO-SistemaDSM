#pragma once

#include <cstdint>
#include <mutex>

struct StatisticsSnapshot {
    uint64_t page_faults = 0;
    uint64_t coalesced_faults = 0;
    uint64_t page_requests = 0;
    uint64_t page_transfers = 0;
    uint64_t invalidations_sent = 0;
    uint64_t invalidations_received = 0;
};

class Statistics {
public:
    void fault() { std::lock_guard<std::mutex> lock(mutex_); ++values_.page_faults; }
    void coalesced() { std::lock_guard<std::mutex> lock(mutex_); ++values_.coalesced_faults; }
    void request() { std::lock_guard<std::mutex> lock(mutex_); ++values_.page_requests; }
    void transfer() { std::lock_guard<std::mutex> lock(mutex_); ++values_.page_transfers; }
    void invalidation_sent() { std::lock_guard<std::mutex> lock(mutex_); ++values_.invalidations_sent; }
    void invalidation_received() { std::lock_guard<std::mutex> lock(mutex_); ++values_.invalidations_received; }

    StatisticsSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return values_;
    }

private:
    mutable std::mutex mutex_;
    StatisticsSnapshot values_;
};
