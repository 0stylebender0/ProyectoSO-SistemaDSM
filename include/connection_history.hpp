#pragma once

#include "node.hpp"

#include <chrono>
#include <mutex>
#include <vector>

enum class HistoryEvent { LOCAL_START, JOIN_SENT, JOIN_ACCEPTED, NODE_DISCOVERED, ANNOUNCE_RECEIVED };

struct HistoryEntry {
    NodeInfo node;
    uint64_t timestamp_ms;
    HistoryEvent event;
};

class ConnectionHistory {
public:
    void record(const NodeInfo& node, HistoryEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        entries_.push_back({node, static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count()), event});
    }

    std::vector<HistoryEntry> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<HistoryEntry> entries_;
};
