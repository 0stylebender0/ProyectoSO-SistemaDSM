#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using NodeId = uint64_t;

struct NodeInfo {
    NodeId id = 0;
    std::string node_name;
    std::string ip_address;
    uint16_t tcp_port = 0;
    uint16_t heartbeat_port = 0;
    uint64_t memory_size = 0;
    uint32_t page_size = 0;
};

bool generate_node_id(NodeId& out, std::string& error);
bool validate_node(const NodeInfo& node, std::string& error);

class NodeTable {
public:
    bool add(const NodeInfo& node, std::string& error, bool* inserted = nullptr);
    bool merge(const std::vector<NodeInfo>& nodes, std::string& error);
    bool find(NodeId id, NodeInfo& out) const;
    std::vector<NodeInfo> snapshot() const;
    void update_address(NodeId id, const std::string& address);
    void clear();

private:
    std::map<NodeId, NodeInfo> nodes_;
    mutable std::mutex mutex_;
};
