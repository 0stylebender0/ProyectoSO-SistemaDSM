#pragma once

#include "config.hpp"
#include "node.hpp"
#include "protocol.hpp"

#include <pthread.h>

#include <functional>
#include <queue>
#include <atomic>
#include <set>
#include <string>
#include <utility>
#include <vector>

class NetworkManager {
public:
    using PageRequestHandler = std::function<bool(
        const PageRequest&, NodeId, PageData&, std::string&)>;
    using RegionInfoHandler = std::function<bool(
        RegionId, RegionDescriptor&, std::string&)>;

    explicit NetworkManager(const Config& config, NodeId node_id = 0);
    ~NetworkManager();

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    bool start(std::string& error);
    bool join(const std::string& bootstrap, std::string& error);
    void stop();

    // Deben configurarse antes de start(): los workers los consultan.
    void set_page_request_handler(PageRequestHandler handler) {
        page_request_handler_ = std::move(handler);
    }
    void set_region_info_handler(RegionInfoHandler handler) {
        region_info_handler_ = std::move(handler);
    }

    bool find_node(NodeId id, NodeInfo& out) const;
    bool fetch_page(const NodeInfo& owner, const PageRequest& request,
                    PageData& response, std::string& error);
    bool query_region(const NodeInfo& owner, RegionId region_id,
                      RegionDescriptor& out, std::string& error);

    uint16_t port() const { return port_; }
    NodeId node_id() const { return node_id_; }
    std::vector<NodeInfo> nodes() const { return node_table_.snapshot(); }

private:
    static void* listener_entry(void* context);
    static void* worker_entry(void* context);

    void listen_loop();
    void worker_loop();
    void handle_connection(int fd);
    bool handle_join(int fd, const MessageHeader& request,
                     const std::vector<uint8_t>& payload, std::string& error);
    bool send_error(int fd, const MessageHeader& request, ErrorCode code, std::string& error);
    void announce(const NodeInfo& node, const std::vector<NodeInfo>& targets, NodeId except = 0);
    void leave();
    int connect_to(const std::string& host, uint16_t port, std::string& error);
    void close_outgoing(int fd);
    NodeInfo local_node() const;

    Config config_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    NodeId node_id_hint_ = 0;
    NodeId node_id_ = 0;
    NodeTable node_table_;
    PageRequestHandler page_request_handler_;
    RegionInfoHandler region_info_handler_;
    std::atomic<uint64_t> next_message_{1};
    bool running_ = false;
    bool listener_started_ = false;
    pthread_t listener_{};
    std::vector<pthread_t> workers_;
    std::queue<int> pending_;
    std::set<int> connections_;
    std::set<int> outgoing_;
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t ready_ = PTHREAD_COND_INITIALIZER;
};
