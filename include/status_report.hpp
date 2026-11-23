#pragma once

#include "connection_history.hpp"
#include "network.hpp"
#include "pager.hpp"
#include "protocol.hpp"
#include "statistics.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

// Formato de texto compartido por la consola local y las consultas STATUS.
// Se mantiene en un encabezado para no duplicar la representacion entre el
// nodo, dsm_status y las pruebas.
namespace status_report {

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
                            uint8_t flags) {
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
    return result;
}

}  // namespace status_report
