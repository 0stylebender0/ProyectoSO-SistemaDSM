#include "node.hpp"

#include <arpa/inet.h>
#include <sys/random.h>

#include <cerrno>
#include <cstring>

namespace {
    bool same_node(const NodeInfo& a, const NodeInfo& b) {
        return a.id == b.id && a.node_name == b.node_name &&
               a.ip_address == b.ip_address && a.tcp_port == b.tcp_port &&
               a.heartbeat_port == b.heartbeat_port &&
               a.memory_size == b.memory_size && a.page_size == b.page_size;
    }
}

bool generate_node_id(NodeId& out, std::string& error) {
    do {
        NodeId value = 0;
        std::size_t received = 0;
        while (received < sizeof(value)) {
            const ssize_t count = getrandom(
                reinterpret_cast<uint8_t*>(&value) + received, sizeof(value) - received, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                error = "no se pudo generar NodeId";
                if (count < 0) error += std::string(": ") + std::strerror(errno);
                return false;
            }
            received += static_cast<std::size_t>(count);
        }
        out = value;
    } while (out == 0);
    return true;
}

bool validate_node(const NodeInfo& node, std::string& error) {
    in_addr address{};
    if (node.id == 0 || node.node_name.empty() || node.node_name.size() > 255 ||
        inet_pton(AF_INET, node.ip_address.c_str(), &address) != 1 ||
        address.s_addr == htonl(INADDR_ANY) || address.s_addr == htonl(INADDR_BROADCAST) ||
        node.tcp_port == 0 || node.heartbeat_port == 0 || node.memory_size == 0 ||
        node.page_size == 0 || (node.page_size & (node.page_size - 1)) != 0) {
        error = "informacion de nodo invalida";
        return false;
    }
    for (unsigned char c : node.node_name) {
        if (c < 32 || c == 127) {
            error = "node_name contiene caracteres de control";
            return false;
        }
    }
    return true;
}

bool NodeTable::add(const NodeInfo& node, std::string& error, bool* inserted) {
    if (inserted != nullptr) *inserted = false;
    if (!validate_node(node, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = nodes_.find(node.id);
    if (found != nodes_.end()) {
        if (same_node(found->second, node)) return true;
        error = "NodeId en conflicto: " + std::to_string(node.id);
        return false;
    }
    nodes_.emplace(node.id, node);
    if (inserted != nullptr) *inserted = true;
    return true;
}

bool NodeTable::merge(const std::vector<NodeInfo>& nodes, std::string& error) {
    std::map<NodeId, NodeInfo> incoming;
    for (const auto& node : nodes) {
        if (!validate_node(node, error)) return false;
        if (!incoming.emplace(node.id, node).second) {
            error = "NodeId repetido en la lista";
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : incoming) {
        const auto found = nodes_.find(entry.first);
        if (found != nodes_.end() && !same_node(found->second, entry.second)) {
            error = "NodeId en conflicto: " + std::to_string(entry.first);
            return false;
        }
    }
    // La lista se aplica completa para no dejar una union a medias por un conflicto.
    nodes_.insert(incoming.begin(), incoming.end());
    return true;
}

bool NodeTable::find(NodeId id, NodeInfo& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = nodes_.find(id);
    if (found == nodes_.end()) return false;
    out = found->second;
    return true;
}

std::vector<NodeInfo> NodeTable::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeInfo> result;
    for (const auto& entry : nodes_) result.push_back(entry.second);
    return result;
}

void NodeTable::update_address(NodeId id, const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = nodes_.find(id);
    if (found != nodes_.end()) found->second.ip_address = address;
}

void NodeTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    nodes_.clear();
}
