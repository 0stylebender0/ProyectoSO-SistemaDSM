#pragma once

#include "memory_manager.hpp"
#include "network.hpp"
#include "pager.hpp"

#include <string>

// Conecta memoria, pager y red sin dependencias circulares.
class DSMNode {
public:
    DSMNode(MemoryManager& memory, NetworkManager& network, NodeId local_id);

    DSMNode(const DSMNode&) = delete;
    DSMNode& operator=(const DSMNode&) = delete;

    Pager& pager() { return pager_; }
    const Pager& pager() const { return pager_; }

    NetworkManager& network() { return network_; }
    MemoryManager& memory() { return memory_; }

    bool create_region(uint64_t size, RegionId& region_id, std::string& error) {
        return pager_.create_region(size, region_id, error);
    }

    bool attach_region(const RegionDescriptor& descriptor, std::string& error) {
        return pager_.attach_region(descriptor, error);
    }

    // Consulta REGION_INFO al dueno y adjunta la region en este nodo.
    bool attach_region_from(NodeId owner_id, RegionId region_id,
                            std::string& error);

    bool resolve_fault(PageId page_id, bool write, std::string& error) {
        return pager_.resolve_fault(page_id, write, error);
    }

private:
    MemoryManager& memory_;
    NetworkManager& network_;
    Pager pager_;
};
