#pragma once

#include "connection_history.hpp"
#include "network.hpp"
#include "pager.hpp"
#include "protocol.hpp"
#include "statistics.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace status_report {

struct NodeReport {
    NodeInfo node;
    std::string text;
    std::string error;
    std::map<std::string, uint64_t> values;
};

struct GlobalReport {
    std::vector<NodeReport> nodes;
    std::map<std::string, uint64_t> totals;
    std::size_t responded = 0;
    bool partial() const { return responded != nodes.size(); }
};

using StatusQuery = std::function<std::string(const NodeInfo&, uint8_t, std::string&)>;
GlobalReport collect(const std::vector<NodeInfo>& nodes, uint8_t flags, const StatusQuery& query);
std::string global(const GlobalReport& report, uint8_t flags);

inline std::string memory(const MemoryManager& memory, const Pager& pager, uint64_t configured) {
    uint64_t used = 0;
    for (const auto& region : pager.region_snapshot()) used += region.page_count;
    std::ostringstream out;
    out << "memory_configured=" << configured << "\n"
        << "memory_reserved=" << memory.size() << "\n"
        << "memory_pages_total=" << memory.page_count() << "\n"
        << "memory_pages_used=" << used << "\n"
        << "memory_pages_free=" << memory.page_count() - used << "\n";
    return out.str();
}

inline std::string history(const std::vector<HistoryEntry>& entries) {
    std::ostringstream out;
    out << "history_count=" << entries.size() << "\n";

    std::string sequence;
    std::string previous;
    for (const auto& entry : entries) {
        if (entry.node.node_name == previous) continue;
        if (!sequence.empty()) sequence += ",";
        sequence += entry.node.node_name;
        previous = entry.node.node_name;
    }
    out << "history_nodes=" << sequence << "\n";

    for (const auto& entry : entries) {
        out << "history=" << entry.timestamp_ms << ","
            << history_event_name(entry.event) << ","
            << entry.node.id << ","
            << entry.node.node_name << ","
            << entry.node.ip_address << ":" << entry.node.tcp_port << "\n";
    }
    return out.str();
}

inline std::string pages(const Pager& pager) {
    auto entries = pager.page_snapshot();
    std::sort(entries.begin(), entries.end(),
              [](const PageInfo& a, const PageInfo& b) {
                  return a.page_id < b.page_id;
              });

    std::ostringstream out;
    out << "pages_total=" << entries.size() << "\n";
    uint64_t invalid = 0, shared = 0, modified = 0;
    for (const auto& page : entries) {
        if (page.state == MSIState::INVALID) ++invalid;
        if (page.state == MSIState::SHARED) ++shared;
        if (page.state == MSIState::MODIFIED) ++modified;
    }
    out << "pages_state_invalid=" << invalid << "\n"
        << "pages_state_shared=" << shared << "\n"
        << "pages_state_modified=" << modified << "\n";
    for (const auto& page : entries) {
        out << "page=" << page.page_id
            << ",region=" << page.region_id
            << ",owner=" << page.owner_id
            << ",state=" << msi_state_name(page.state)
            << ",present=" << (page.locally_present ? "si" : "no")
            << ",version=" << page.version
            << ",sharers=";
        bool first = true;
        for (NodeId sharer : page.sharers) {
            if (!first) out << ";";
            out << sharer;
            first = false;
        }
        out << "\n";
    }
    return out.str();
}

inline StatisticsSnapshot combined_statistics(const Pager& pager,
                                              const NetworkManager& network) {
    StatisticsSnapshot result = pager.statistics();
    const StatisticsSnapshot traffic = network.statistics();
    result.bytes_sent += traffic.bytes_sent;
    result.bytes_received += traffic.bytes_received;
    return result;
}

inline std::string statistics(const Pager& pager, const NetworkManager& network) {
    const StatisticsSnapshot values = combined_statistics(pager, network);
    std::ostringstream out;
    out << "pages_owned=" << values.pages_owned << "\n"
        << "pages_shared=" << values.pages_shared << "\n"
        << "page_faults=" << values.page_faults << "\n"
        << "coalesced_faults=" << values.coalesced_faults << "\n"
        << "page_requests=" << values.page_requests << "\n"
        << "page_transfers=" << values.page_transfers << "\n"
        << "bytes_sent=" << values.bytes_sent << "\n"
        << "bytes_received=" << values.bytes_received << "\n"
        << "invalidations_sent=" << values.invalidations_sent << "\n"
        << "invalidations_received=" << values.invalidations_received << "\n";
    return out.str();
}

inline std::string sections(const Pager& pager, const NetworkManager& network,
                            const MemoryManager& manager, uint8_t flags) {
    std::string result;
    if ((flags & STATUS_FLAG_HISTORY) != 0) {
        result += history(network.history());
    }
    if ((flags & STATUS_FLAG_PAGES) != 0) {
        result += pages(pager);
    }
    if ((flags & STATUS_FLAG_STATS) != 0) {
        result += statistics(pager, network);
    }
    if ((flags & STATUS_FLAG_MEMORY) != 0) {
        NodeInfo self;
        network.find_node(network.node_id(), self);
        result += memory(manager, pager, self.memory_size);
    }
    return result;
}

}  // namespace status_report
