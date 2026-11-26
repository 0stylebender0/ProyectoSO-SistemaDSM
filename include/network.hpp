#pragma once

#include "config.hpp"
#include "connection_history.hpp"
#include "heartbeat.hpp"
#include "node.hpp"
#include "protocol.hpp"
#include "statistics.hpp"

#include <pthread.h>

#include <functional>
#include <memory>
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
    using InvalidationHandler = std::function<bool(
        const InvalidateMessage&, NodeId, PageData&, std::string&)>;
    using PageReleaseHandler = std::function<bool(const PageData&, NodeId, std::string&)>;
    using RegionListHandler = std::function<bool(
        std::vector<RegionDescriptor>&, std::string&)>;
    using StatusHandler = std::function<std::string(uint8_t)>;

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
    void set_invalidation_handler(InvalidationHandler handler) {
        invalidation_handler_ = std::move(handler);
    }
    void set_page_release_handler(PageReleaseHandler handler) {
        page_release_handler_ = std::move(handler);
    }
    bool release_page(const NodeInfo& owner, const PageData& data, std::string& error);
    void set_region_list_handler(RegionListHandler handler) {
        region_list_handler_ = std::move(handler);
    }
    void set_status_handler(StatusHandler handler) {
        status_handler_ = std::move(handler);
    }
    bool invalidate(const NodeInfo& target, const InvalidateMessage& request,
                    PageData& writeback, std::string& error);
    uint32_t request_timeout_ms() const { return config_.request_timeout_ms; }

    bool find_node(NodeId id, NodeInfo& out) const;
    bool fetch_page(const NodeInfo& owner, const PageRequest& request,
                    PageData& response, std::string& error);
    bool query_region(const NodeInfo& owner, RegionId region_id,
                      RegionDescriptor& out, std::string& error);
    bool query_regions(const NodeInfo& owner,
                       std::vector<RegionDescriptor>& out, std::string& error);
    // Devuelve texto vacio si la consulta falla; error contiene la causa.
    std::string query_status(const NodeInfo& node, uint8_t flags, std::string& error);

    // Disponibilidad observada por el heartbeat. Un nodo desconocido se
    // considera disponible; solo UNAVAILABLE bloquea nuevas operaciones.
    AvailabilityState availability(NodeId id) const;
    bool node_available(NodeId id) const;
    std::vector<HeartbeatSnapshot> availability_snapshot() const;
    std::vector<NodeInfo> active_nodes() const;
    uint16_t heartbeat_port() const;

    uint16_t port() const { return port_; }
    NodeId node_id() const { return node_id_; }
    std::vector<NodeInfo> nodes() const { return node_table_.snapshot(); }
    std::vector<HistoryEntry> history() const { return history_.snapshot(); }
    StatisticsSnapshot statistics() const { return traffic_.snapshot(); }

private:
    static void* listener_entry(void* context);
    static void* worker_entry(void* context);
    static void* page_worker_entry(void* context);

    void listen_loop();
    void worker_loop();
    bool handle_connection(int fd);
    void page_worker_loop();
    bool handle_join(int fd, const MessageHeader& request,
                     const std::vector<uint8_t>& payload, std::string& error);
    bool send_error(int fd, const MessageHeader& request, ErrorCode code, std::string& error);
    void announce(const NodeInfo& node, const std::vector<NodeInfo>& targets, NodeId except = 0);
    void leave();
    int connect_to(const std::string& host, uint16_t port, std::string& error,
                   IODeadline deadline = IODeadline::max());
    void close_outgoing(int fd);
    NodeInfo local_node() const;
    bool trusted_sender(int fd, NodeId sender, std::string& error) const;
    bool require_available(const NodeInfo& node, std::string& error) const;

    Config config_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    NodeId node_id_hint_ = 0;
    NodeId node_id_ = 0;
    NodeTable node_table_;
    ConnectionHistory history_;
    std::unique_ptr<HeartbeatManager> heartbeat_;
    PageRequestHandler page_request_handler_;
    RegionInfoHandler region_info_handler_;
    InvalidationHandler invalidation_handler_;
    PageReleaseHandler page_release_handler_;
    RegionListHandler region_list_handler_;
    StatusHandler status_handler_;
    Statistics traffic_;
    std::atomic<uint64_t> next_message_{1};
    bool running_ = false;
    bool listener_started_ = false;
    pthread_t listener_{};
    std::vector<pthread_t> workers_;
    std::vector<pthread_t> page_workers_;
    struct PageJob {
        int fd;
        MessageHeader header;
        PageRequest request;
        PageData release;
    };
    std::queue<PageJob> page_jobs_;
    pthread_cond_t page_ready_ = PTHREAD_COND_INITIALIZER;
    std::queue<int> pending_;
    std::set<int> connections_;
    std::set<int> outgoing_;
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t ready_ = PTHREAD_COND_INITIALIZER;
};
