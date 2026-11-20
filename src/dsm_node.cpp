#include "dsm_node.hpp"

DSMNode::DSMNode(MemoryManager& memory, NetworkManager& network, NodeId local_id)
    : memory_(memory), network_(network), pager_(memory, local_id) {}
