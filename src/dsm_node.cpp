#include "dsm_node.hpp"

#include <utility>

DSMNode::DSMNode(MemoryManager& memory, NetworkManager& network, NodeId local_id)
    : memory_(memory), network_(network), pager_(memory, local_id) {
    pager_.set_page_fetcher(
        [this](const RemotePageQuery& query, PageData& response, std::string& error) {
            NodeInfo owner;
            if (!network_.find_node(query.owner_id, owner)) {
                error = "nodo dueno desconocido: " + std::to_string(query.owner_id);
                return false;
            }
            PageRequest request;
            request.page_id = query.page_id;
            request.region_id = query.region_id;
            request.region_offset = query.region_offset;
            request.access_type = query.write ? 1 : 0;
            request.current_state = static_cast<uint8_t>(query.current_state);
            request.known_version = query.known_version;
            return network_.fetch_page(owner, request, response, error);
        });

    network_.set_page_request_handler(
        [this](const PageRequest& request, NodeId requester,
               PageData& response, std::string& error) {
            return pager_.serve_page_request(request, requester, response, error);
        });

    network_.set_region_info_handler(
        [this](RegionId region_id, RegionDescriptor& descriptor, std::string& error) {
            RegionInfo info;
            if (!pager_.region_info(region_id, info)) {
                error = "region desconocida: " + std::to_string(region_id);
                return false;
            }
            descriptor = info.descriptor;
            return true;
        });
}

bool DSMNode::attach_region_from(NodeId owner_id, RegionId region_id,
                                 std::string& error) {
    NodeInfo owner;
    if (!network_.find_node(owner_id, owner)) {
        error = "nodo dueno desconocido: " + std::to_string(owner_id);
        return false;
    }
    RegionDescriptor descriptor;
    if (!network_.query_region(owner, region_id, descriptor, error)) {
        return false;
    }
    if (descriptor.owner_id != owner_id) {
        error = "REGION_INFO_RESPONSE con dueno inesperado";
        return false;
    }
    return pager_.attach_region(descriptor, error);
}
