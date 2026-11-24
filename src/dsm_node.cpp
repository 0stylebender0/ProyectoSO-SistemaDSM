#include "dsm_node.hpp"
#include "status_report.hpp"

#include <utility>

DSMNode::DSMNode(MemoryManager& memory, NetworkManager& network, NodeId local_id)
    : memory_(memory), network_(network), pager_(memory, local_id, network.request_timeout_ms()) {
    pager_.set_page_releaser([this](NodeId owner_id, const PageData& data, std::string& error) {
        NodeInfo owner;
        if (!network_.find_node(owner_id, owner)) {
            error = "nodo dueno desconocido al liberar la pagina";
            return false;
        }
        return network_.release_page(owner, data, error);
    });
    network_.set_page_release_handler([this](const PageData& data, NodeId sender, std::string& error) {
        return pager_.accept_page_release(data, sender, error);
    });
    pager_.set_page_invalidator(
        [this](NodeId target_id, const InvalidateMessage& request,
               PageData& writeback, std::string& error) {
            NodeInfo target;
            if (!network_.find_node(target_id, target)) {
                error = "nodo a invalidar desconocido: " + std::to_string(target_id);
                return false;
            }
            return network_.invalidate(target, request, writeback, error);
        });
    network_.set_invalidation_handler(
        [this](const InvalidateMessage& request, NodeId sender,
               PageData& writeback, std::string& error) {
            return pager_.invalidate_page(request, sender, writeback, error);
        });
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

    network_.set_region_list_handler(
        [this](std::vector<RegionDescriptor>& regions, std::string& error) {
            (void)error;
            regions.clear();
            for (const auto& info : pager_.region_snapshot()) {
                regions.push_back(info.descriptor);
            }
            return true;
        });

    network_.set_status_handler(
        [this](uint8_t flags) {
            return status_report::sections(pager_, network_, memory_, flags);
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
