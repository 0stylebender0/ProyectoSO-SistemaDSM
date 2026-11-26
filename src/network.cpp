#include "network.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <algorithm>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
    MessageHeader make_header(MessageType type, uint64_t message_id,
                              NodeId sender, NodeId receiver) {
        MessageHeader header{};
        header.magic = DSM_MAGIC;
        header.version = DSM_PROTOCOL_VERSION;
        header.type = static_cast<uint16_t>(type);
        header.message_id = message_id;
        header.sender_id = sender;
        header.receiver_id = receiver;
        return header;
    }

    bool socket_address(int fd, bool local, std::string& out, std::string& error) {
        sockaddr_in address{};
        socklen_t size = sizeof(address);
        const int result = local ? getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) :
                                   getpeername(fd, reinterpret_cast<sockaddr*>(&address), &size);
        char text[INET_ADDRSTRLEN]{};
        if (result == -1 || inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) == nullptr) {
            error = std::string("no se pudo obtener la direccion TCP: ") + std::strerror(errno);
            return false;
        }
        out = text;
        return true;
    }

    bool parse_bootstrap(const std::string& text, std::string& host,
                         uint16_t& port, std::string& error) {
        const std::size_t colon = text.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == text.size()) {
            error = "bootstrap debe tener el formato host:puerto";
            return false;
        }
        uint64_t value = 0;
        for (std::size_t i = colon + 1; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') {
                error = "puerto bootstrap invalido";
                return false;
            }
            const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
            if (value > (65535 - digit) / 10) {
                error = "puerto bootstrap fuera de rango";
                return false;
            }
            value = value * 10 + digit;
        }
        if (value == 0) {
            error = "puerto bootstrap fuera de rango";
            return false;
        }
        host = text.substr(0, colon);
        port = static_cast<uint16_t>(value);
        return true;
    }
}

NetworkManager::NetworkManager(const Config& config, NodeId node_id)
    : config_(config), node_id_hint_(node_id) {}

NetworkManager::~NetworkManager() {
    stop();
    pthread_cond_destroy(&ready_);
    pthread_cond_destroy(&page_ready_);
    pthread_mutex_destroy(&mutex_);
}

bool NetworkManager::start(std::string& error) {
    if (listen_fd_ != -1) {
        error = "la red ya esta iniciada";
        return false;
    }
    if (config_.worker_count == 0 || config_.max_connections == 0 ||
        config_.max_connections > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        error = "worker_count y max_connections deben ser positivos y max_connections debe caber en int";
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.tcp_port);
    if (inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
        error = "bind_address debe ser una direccion IPv4 valida";
        return false;
    }

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1) {
        error = std::string("socket fallo: ") + std::strerror(errno);
        return false;
    }

    const int reuse = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1 ||
        bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 ||
        listen(listen_fd_, static_cast<int>(config_.max_connections)) == -1) {
        error = std::string("no se pudo iniciar la escucha TCP: ") + std::strerror(errno);
        stop();
        return false;
    }

    socklen_t address_size = sizeof(address);
    if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &address_size) == -1) {
        error = std::string("getsockname fallo: ") + std::strerror(errno);
        stop();
        return false;
    }
    port_ = ntohs(address.sin_port);

    if (node_id_hint_ != 0) {
        node_id_ = node_id_hint_;
    } else if (!generate_node_id(node_id_, error)) {
        stop();
        return false;
    }

    const uint32_t interval = config_.heartbeat_interval_ms;
    const uint32_t timeout = config_.heartbeat_timeout_ms;
    const uint32_t unavailable_after =
        timeout > std::numeric_limits<uint32_t>::max() - interval
            ? std::numeric_limits<uint32_t>::max() : timeout + interval;
    heartbeat_ = std::make_unique<HeartbeatManager>(
        config_.bind_address, config_.heartbeat_port, interval, timeout,
        unavailable_after);
    heartbeat_->set_peer_provider([this] { return node_table_.snapshot(); });
    heartbeat_->set_transition_handler(
        [this](const NodeInfo& node, const AvailabilityTransition& transition) {
            switch (transition.current) {
                case AvailabilityState::SUSPECT:
                    history_.record(node, HistoryEvent::NODE_SUSPECT);
                    break;
                case AvailabilityState::UNAVAILABLE:
                    history_.record(node, HistoryEvent::NODE_UNAVAILABLE);
                    break;
                case AvailabilityState::ACTIVE:
                    history_.record(node, HistoryEvent::NODE_RECOVERED);
                    break;
            }
            std::cout << "[" << config_.node_name << "] nodo " << node.node_name
                      << ' ' << availability_state_name(transition.current)
                      << " (heartbeat)\n" << std::flush;
        });
    if (!heartbeat_->start(node_id_, error)) {
        heartbeat_.reset();
        stop();
        return false;
    }

    NodeInfo self;
    self.id = node_id_;
    self.node_name = config_.node_name;
    self.ip_address = config_.bind_address == "0.0.0.0" ? "127.0.0.1" : config_.bind_address;
    self.tcp_port = port_;
    self.heartbeat_port = heartbeat_->port();
    self.memory_size = config_.memory_size;
    self.page_size = config_.page_size;
    if (!node_table_.add(self, error)) {
        stop();
        return false;
    }

    history_.record(self, HistoryEvent::LOCAL_START);
    pthread_mutex_lock(&mutex_);
    running_ = true;
    pthread_mutex_unlock(&mutex_);

    for (uint32_t i = 0; i < config_.worker_count; ++i) {
        pthread_t worker{};
        const int result = pthread_create(&worker, nullptr, worker_entry, this);
        if (result != 0) {
            error = std::string("no se pudo crear un worker: ") + std::strerror(result);
            stop();
            return false;
        }
        workers_.push_back(worker);
    }

    for (uint32_t i = 0; i < config_.worker_count; ++i) {
        pthread_t worker{};
        const int result = pthread_create(&worker, nullptr, page_worker_entry, this);
        if (result != 0) {
            error = std::string("no se pudo crear worker de paginas: ") + std::strerror(result);
            stop();
            return false;
        }
        page_workers_.push_back(worker);
    }

    const int result = pthread_create(&listener_, nullptr, listener_entry, this);
    if (result != 0) {
        error = std::string("no se pudo crear el listener: ") + std::strerror(result);
        stop();
        return false;
    }
    listener_started_ = true;
    std::string seeds = config_.bootstrap_nodes;
    for (const auto& address : config_.node_addresses) {
        if (!seeds.empty()) seeds += ",";
        seeds += address;
    }
    if (!seeds.empty()) {
        std::size_t begin = 0;
        bool joined = false;
        while (!joined && begin <= seeds.size()) {
            const std::size_t comma = seeds.find(',', begin);
            const std::string seed = seeds.substr(
                begin, comma == std::string::npos ? std::string::npos : comma - begin);
            if (!seed.empty()) joined = join(seed, error);
            if (comma == std::string::npos) break;
            begin = comma + 1;
        }
        if (!joined) {
            stop();
            return false;
        }
    }
    return true;
}

void NetworkManager::stop() {
    leave();
    pthread_mutex_lock(&mutex_);
    running_ = false;
    // Desbloquea recv/send antes de esperar a los workers.
    for (int fd : connections_) {
        shutdown(fd, SHUT_RDWR);
    }
    for (int fd : outgoing_) {
        shutdown(fd, SHUT_RDWR);
    }
    pthread_cond_broadcast(&ready_);
    pthread_cond_broadcast(&page_ready_);
    pthread_mutex_unlock(&mutex_);

    if (listen_fd_ != -1) {
        shutdown(listen_fd_, SHUT_RDWR);
    }
    if (listener_started_) {
        pthread_join(listener_, nullptr);
        listener_started_ = false;
    }
    for (pthread_t worker : workers_) {
        pthread_join(worker, nullptr);
    }
    workers_.clear();
    for (pthread_t worker : page_workers_) pthread_join(worker, nullptr);
    page_workers_.clear();
    while (!page_jobs_.empty()) {
        close(page_jobs_.front().fd);
        page_jobs_.pop();
    }

    while (!pending_.empty()) {
        close(pending_.front());
        pending_.pop();
    }
    connections_.clear();
    outgoing_.clear();

    // El listener ya termino y no puede usar un descriptor que el SO reutilice.
    if (listen_fd_ != -1) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    port_ = 0;
    node_id_ = 0;
    if (heartbeat_) {
        heartbeat_->stop();
        heartbeat_.reset();
    }
    node_table_.clear();
}

void* NetworkManager::listener_entry(void* context) {
    static_cast<NetworkManager*>(context)->listen_loop();
    return nullptr;
}

void* NetworkManager::worker_entry(void* context) {
    static_cast<NetworkManager*>(context)->worker_loop();
    return nullptr;
}

void NetworkManager::listen_loop() {
    while (true) {
        const int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd == -1) {
            const int accept_error = errno;
            if (accept_error == EINTR) continue;
            pthread_mutex_lock(&mutex_);
            const bool running = running_;
            pthread_mutex_unlock(&mutex_);
            if (!running) return;
            std::cerr << "accept fallo: " << std::strerror(accept_error) << "\n";
            continue;
        }

        timeval timeout{};
        timeout.tv_sec = config_.request_timeout_ms / 1000;
        timeout.tv_usec = (config_.request_timeout_ms % 1000) * 1000;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
            close(fd);
            continue;
        }

        pthread_mutex_lock(&mutex_);
        if (!running_ || connections_.size() >= config_.max_connections) {
            const bool running = running_;
            pthread_mutex_unlock(&mutex_);
            close(fd);
            if (!running) return;
            continue;
        }
        connections_.insert(fd);
        pending_.push(fd);
        pthread_cond_signal(&ready_);
        pthread_mutex_unlock(&mutex_);
    }
}

void NetworkManager::worker_loop() {
    while (true) {
        pthread_mutex_lock(&mutex_);
        while (running_ && pending_.empty()) {
            pthread_cond_wait(&ready_, &mutex_);
        }
        if (!running_) {
            pthread_mutex_unlock(&mutex_);
            return;
        }
        const int fd = pending_.front();
        pending_.pop();
        pthread_mutex_unlock(&mutex_);

        if (handle_connection(fd)) continue;

        pthread_mutex_lock(&mutex_);
        connections_.erase(fd);
        close(fd);
        pthread_mutex_unlock(&mutex_);
    }
}

bool NetworkManager::handle_connection(int fd) {
    MessageHeader request{};
    std::vector<uint8_t> payload;
    std::string error;

    while (recv_message(fd, request, payload, error)) {
        if (request.receiver_id != 0 && request.receiver_id != node_id_) {
            if (!send_error(fd, request, ErrorCode::UNKNOWN_NODE, error)) return false;
            continue;
        }
        if (request.type == static_cast<uint16_t>(MessageType::JOIN_REQUEST)) {
            if (!handle_join(fd, request, payload, error)) return false;
            continue;
        }
        if (request.type == static_cast<uint16_t>(MessageType::NODE_LEAVE)) {
            if (!payload.empty() || !trusted_sender(fd, request.sender_id, error)) {
                if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return false;
                continue;
            }
            node_table_.remove(request.sender_id);
            if (heartbeat_) heartbeat_->forget(request.sender_id);
            const MessageHeader response = make_header(MessageType::NODE_LEAVE_ACK,
                request.message_id, node_id_, request.sender_id);
            if (!send_message(fd, response, {}, error)) return false;
            continue;
        }
        if (request.type == static_cast<uint16_t>(MessageType::NODE_ANNOUNCE)) {
            std::vector<NodeInfo> announced;
            bool inserted = false;
            if (!trusted_sender(fd, request.sender_id, error) ||
                !deserialize_nodes(payload, announced, error) || announced.size() != 1) {
                if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return false;
            } else if (announced[0].page_size != config_.page_size) {
                if (!send_error(fd, request, ErrorCode::PROTOCOL_MISMATCH, error)) return false;
            } else if (!node_table_.add(announced[0], error, &inserted)) {
                if (!send_error(fd, request, ErrorCode::INVALID_STATE, error)) return false;
            } else {
                history_.record(announced[0], HistoryEvent::ANNOUNCE_RECEIVED);
                if (heartbeat_) heartbeat_->observe(announced[0]);
                if (inserted) announce(announced[0], nodes(), request.sender_id);
            }
            continue;
        }
        if (request.type == static_cast<uint16_t>(MessageType::INVALIDATE)) {
            InvalidateMessage message;
            if (!trusted_sender(fd, request.sender_id, error) ||
                !deserialize_invalidate(payload, message, error)) {
                if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return false;
                continue;
            }
            PageData writeback;
            if (!invalidation_handler_ ||
                !invalidation_handler_(message, request.sender_id, writeback, error)) {
                if (!send_error(fd, request, ErrorCode::INVALID_STATE, error)) return false;
                continue;
            }
            std::vector<uint8_t> reply;
            if (!writeback.data.empty()) {
                if (!serialize_page_data(writeback, reply, error)) return false;
                const MessageHeader data = make_header(MessageType::PAGE_DATA,
                    request.message_id, node_id_, request.sender_id);
                if (!send_message(fd, data, reply, error)) return false;
                traffic_.bytes_sent(writeback.data.size());
            }
            if (!serialize_invalidate(message, reply, error)) return false;
            const MessageHeader ack = make_header(MessageType::INVALIDATE_ACK,
                request.message_id, node_id_, request.sender_id);
            if (!send_message(fd, ack, reply, error)) return false;
            continue;
        }
        if (request.type == static_cast<uint16_t>(MessageType::PAGE_RELEASE)) {
            PageData data;
            if (!trusted_sender(fd, request.sender_id, error) ||
                !deserialize_page_data(payload, data, error)) {
                if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return false;
                continue;
            }
            pthread_mutex_lock(&mutex_);
            if (!running_) { pthread_mutex_unlock(&mutex_); return false; }
            page_jobs_.push({fd, request, {}, std::move(data)});
            pthread_cond_signal(&page_ready_);
            pthread_mutex_unlock(&mutex_);
            return true;
        }
        if (request.type == static_cast<uint16_t>(MessageType::PAGE_REQUEST)) {
            PageRequest page_request;
            if (!trusted_sender(fd, request.sender_id, error) ||
                !deserialize_page_request(payload, page_request, error)) {
                if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return false;
                continue;
            }
            if (!page_request_handler_) {
                if (!send_error(fd, request, ErrorCode::PAGE_NOT_FOUND, error)) return false;
                continue;
            }
            pthread_mutex_lock(&mutex_);
            if (!running_) {
                pthread_mutex_unlock(&mutex_);
                return false;
            }
            page_jobs_.push({fd, request, page_request, {}});
            pthread_cond_signal(&page_ready_);
            pthread_mutex_unlock(&mutex_);
            return true;
        }
        if (request.type == static_cast<uint16_t>(MessageType::REGION_INFO_REQUEST)) {
            RegionInfoRequest info_request;
            if (!deserialize_region_info_request(payload, info_request, error)) {
                if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return false;
                continue;
            }
            if (!region_info_handler_) {
                if (!send_error(fd, request, ErrorCode::PAGE_NOT_FOUND, error)) return false;
                continue;
            }
            RegionDescriptor descriptor;
            if (!region_info_handler_(info_request.region_id, descriptor, error)) {
                if (!send_error(fd, request, ErrorCode::PAGE_NOT_FOUND, error)) return false;
                continue;
            }
            RegionInfoResponse info_response;
            info_response.region_id = descriptor.region_id;
            info_response.owner_id = descriptor.owner_id;
            info_response.size = descriptor.size;
            info_response.page_size = descriptor.page_size;
            std::vector<uint8_t> reply;
            if (!serialize_region_info_response(info_response, reply, error)) {
                if (!send_error(fd, request, ErrorCode::INVALID_STATE, error)) return false;
                continue;
            }
            const MessageHeader response = make_header(MessageType::REGION_INFO_RESPONSE,
                request.message_id, node_id_, request.sender_id);
            if (!send_message(fd, response, reply, error)) return false;
            continue;
        }
        if (request.type == static_cast<uint16_t>(MessageType::REGION_LIST_REQUEST)) {
            if (!payload.empty()) {
                if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return false;
                continue;
            }
            if (!region_list_handler_) {
                if (!send_error(fd, request, ErrorCode::PAGE_NOT_FOUND, error)) return false;
                continue;
            }
            std::vector<RegionDescriptor> regions;
            if (!region_list_handler_(regions, error)) {
                if (!send_error(fd, request, ErrorCode::INVALID_STATE, error)) return false;
                continue;
            }
            std::vector<uint8_t> reply;
            if (!serialize_regions(regions, reply, error)) {
                if (!send_error(fd, request, ErrorCode::INVALID_STATE, error)) return false;
                continue;
            }
            const MessageHeader response = make_header(MessageType::REGION_LIST_RESPONSE,
                request.message_id, node_id_, request.sender_id);
            if (!send_message(fd, response, reply, error)) return false;
            continue;
        }

        MessageHeader response = make_header(MessageType::STATUS_RESPONSE,
            request.message_id, node_id_, request.sender_id);

        std::vector<uint8_t> reply;
        if (request.type == static_cast<uint16_t>(MessageType::STATUS_REQUEST) &&
            (payload.empty() || payload.size() == 1)) {
            const uint8_t flags = payload.empty() ? 0 : payload[0];
            if ((flags & static_cast<uint8_t>(~STATUS_FLAG_ALL)) != 0) {
                if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return false;
                continue;
            }
            if (flags != 0 && !status_handler_) {
                if (!send_error(fd, request, ErrorCode::INVALID_STATE, error)) return false;
                continue;
            }
            response.type = static_cast<uint16_t>(MessageType::STATUS_RESPONSE);
            std::string status = "node_name=" + config_.node_name +
                "\nnode_id=" + std::to_string(node_id_) +
                "\ntcp_port=" + std::to_string(port_) +
                "\nworker_count=" + std::to_string(config_.worker_count) + "\n";
            if (flags != 0) status += status_handler_(flags);
            reply.assign(status.begin(), status.end());
        } else if (request.type == static_cast<uint16_t>(MessageType::NODE_LIST_REQUEST) &&
                   payload.empty()) {
            response.type = static_cast<uint16_t>(MessageType::NODE_LIST_RESPONSE);
            if (!serialize_nodes(nodes(), reply, error)) {
                if (!send_error(fd, request, ErrorCode::OUT_OF_MEMORY, error)) return false;
                continue;
            }
        } else {
            if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return false;
            continue;
        }

        if (!send_message(fd, response, reply, error)) return false;
    }
    return false;
}

void* NetworkManager::page_worker_entry(void* context) {
    static_cast<NetworkManager*>(context)->page_worker_loop();
    return nullptr;
}

void NetworkManager::page_worker_loop() {
    while (true) {
        pthread_mutex_lock(&mutex_);
        while (running_ && page_jobs_.empty()) pthread_cond_wait(&page_ready_, &mutex_);
        if (!running_) {
            pthread_mutex_unlock(&mutex_);
            return;
        }
        const PageJob job = page_jobs_.front();
        page_jobs_.pop();
        pthread_mutex_unlock(&mutex_);

        // Las transferencias no ocupan los workers que reciben INVALIDATE y JOIN.
        PageData data;
        std::vector<uint8_t> payload;
        std::string error;
        if (job.header.type == static_cast<uint16_t>(MessageType::PAGE_RELEASE)) {
            RegionDescriptor descriptor;
            if (!region_info_handler_ || !region_info_handler_(job.release.region_id, descriptor, error)) {
                send_error(job.fd, job.header, ErrorCode::PAGE_NOT_FOUND, error);
            } else if (!page_release_handler_ ||
                       !page_release_handler_(job.release, job.header.sender_id, error)) {
                send_error(job.fd, job.header, ErrorCode::INVALID_STATE, error);
            } else {
                traffic_.bytes_received(job.release.data.size());
                const MessageHeader ack = make_header(MessageType::PAGE_RELEASE,
                    job.header.message_id, node_id_, job.header.sender_id);
                send_message(job.fd, ack, {}, error);
            }
        } else if (!page_request_handler_(job.request, job.header.sender_id, data, error)) {
            send_error(job.fd, job.header, ErrorCode::INVALID_STATE, error);
        } else if (!serialize_page_data(data, payload, error)) {
            send_error(job.fd, job.header, ErrorCode::INVALID_STATE, error);
        } else {
            const MessageHeader response = make_header(MessageType::PAGE_DATA,
                job.header.message_id, node_id_, job.header.sender_id);
            if (send_message(job.fd, response, payload, error)) {
                traffic_.bytes_sent(data.data.size());
            }
        }
        pthread_mutex_lock(&mutex_);
        connections_.erase(job.fd);
        close(job.fd);
        pthread_mutex_unlock(&mutex_);
    }
}

NodeInfo NetworkManager::local_node() const {
    NodeInfo node;
    node_table_.find(node_id_, node);
    return node;
}

bool NetworkManager::find_node(NodeId id, NodeInfo& out) const {
    return node_table_.find(id, out);
}

AvailabilityState NetworkManager::availability(NodeId id) const {
    return heartbeat_ ? heartbeat_->state(id) : AvailabilityState::ACTIVE;
}

bool NetworkManager::node_available(NodeId id) const {
    return heartbeat_ ? heartbeat_->available(id) : true;
}

std::vector<HeartbeatSnapshot> NetworkManager::availability_snapshot() const {
    return heartbeat_ ? heartbeat_->snapshot() : std::vector<HeartbeatSnapshot>{};
}

std::vector<NodeInfo> NetworkManager::active_nodes() const {
    std::vector<NodeInfo> result;
    for (const auto& node : node_table_.snapshot()) {
        if (node.id == node_id_ || node_available(node.id)) result.push_back(node);
    }
    return result;
}

uint16_t NetworkManager::heartbeat_port() const {
    return heartbeat_ ? heartbeat_->port() : config_.heartbeat_port;
}

bool NetworkManager::trusted_sender(int fd, NodeId sender, std::string& error) const {
    NodeInfo peer;
    if (sender == 0 || sender == node_id_ || !node_table_.find(sender, peer)) {
        error = "remitente desconocido: " + std::to_string(sender);
        return false;
    }
    std::string address;
    if (!socket_address(fd, false, address, error)) return false;
    if (address != peer.ip_address) {
        error = "la direccion TCP no corresponde al remitente " + std::to_string(sender);
        return false;
    }
    return true;
}

bool NetworkManager::require_available(const NodeInfo& node, std::string& error) const {
    if (node_available(node.id)) return true;
    error = "nodo " + node.node_name + " (" + std::to_string(node.id) +
            ") no disponible segun heartbeat";
    return false;
}

bool NetworkManager::fetch_page(const NodeInfo& owner, const PageRequest& request,
                                PageData& response, std::string& error) {
    if (owner.id == 0 || owner.id == node_id_) {
        error = "destino de PAGE_REQUEST invalido";
        return false;
    }
    if (!require_available(owner, error)) return false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(config_.request_timeout_ms);
    const int fd = connect_to(owner.ip_address, owner.tcp_port, error, deadline);
    if (fd == -1) return false;

    const uint64_t message_id = next_message_++;
    const MessageHeader outgoing = make_header(MessageType::PAGE_REQUEST,
        message_id, node_id_, owner.id);
    std::vector<uint8_t> payload;
    MessageHeader incoming{};
    std::vector<uint8_t> reply;
    bool ok = serialize_page_request(request, payload, error) &&
              send_message(fd, outgoing, payload, error, deadline) &&
              recv_message(fd, incoming, reply, error, deadline);
    if (ok) {
        if (incoming.type == static_cast<uint16_t>(MessageType::ERROR_RESPONSE)) {
            error = "el dueno rechazo PAGE_REQUEST";
            if (reply.size() == 2) {
                error += " (codigo " +
                    std::to_string((reply[0] << 8) | reply[1]) + ")";
            }
            ok = false;
        } else if (incoming.type != static_cast<uint16_t>(MessageType::PAGE_DATA) ||
                   incoming.message_id != message_id ||
                   incoming.sender_id != owner.id ||
                   incoming.receiver_id != node_id_) {
            error = "PAGE_DATA invalida";
            ok = false;
        } else {
            ok = deserialize_page_data(reply, response, error);
            if (ok) traffic_.bytes_received(response.data.size());
        }
    }
    close_outgoing(fd);
    return ok;
}

bool NetworkManager::release_page(const NodeInfo& owner, const PageData& data, std::string& error) {
    if (!require_available(owner, error)) return false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(config_.request_timeout_ms);
    std::vector<uint8_t> payload;
    if (!serialize_page_data(data, payload, error)) return false;
    const int fd = connect_to(owner.ip_address, owner.tcp_port, error, deadline);
    if (fd == -1) return false;
    const MessageHeader request = make_header(MessageType::PAGE_RELEASE, next_message_++, node_id_, owner.id);
    MessageHeader response{};
    std::vector<uint8_t> reply;
    bool ok = send_message(fd, request, payload, error, deadline) &&
              recv_message(fd, response, reply, error, deadline);
    if (ok) {
        ok = response.message_id == request.message_id && response.sender_id == owner.id &&
             response.receiver_id == node_id_ &&
             ((response.type == static_cast<uint16_t>(MessageType::PAGE_RELEASE) && reply.empty()) ||
              (response.type == static_cast<uint16_t>(MessageType::ERROR_RESPONSE) &&
               reply == std::vector<uint8_t>({0, static_cast<uint8_t>(ErrorCode::PAGE_NOT_FOUND)})));
        if (!ok) error = "PAGE_RELEASE rechazado o sin confirmacion valida";
        else traffic_.bytes_sent(data.data.size());
    }
    close_outgoing(fd);
    return ok;
}

bool NetworkManager::invalidate(const NodeInfo& target, const InvalidateMessage& request,
                                 PageData& writeback, std::string& error) {
    writeback = PageData{};
    if (target.id == 0 || target.id == node_id_) {
        error = "destino de INVALIDATE invalido";
        return false;
    }
    if (!require_available(target, error)) return false;
    std::vector<uint8_t> payload;
    if (!serialize_invalidate(request, payload, error)) return false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(config_.request_timeout_ms);
    const int fd = connect_to(target.ip_address, target.tcp_port, error, deadline);
    if (fd == -1) return false;
    const MessageHeader outgoing = make_header(MessageType::INVALIDATE,
        next_message_++, node_id_, target.id);
    MessageHeader incoming{};
    std::vector<uint8_t> reply;
    auto receive = [&] {
        if (!recv_message(fd, incoming, reply, error, deadline)) return false;
        if (incoming.message_id != outgoing.message_id || incoming.sender_id != target.id ||
            incoming.receiver_id != node_id_) {
            error = "respuesta a INVALIDATE sin correlacion";
            return false;
        }
        return true;
    };
    bool ok = send_message(fd, outgoing, payload, error, deadline) && receive();
    if (ok && incoming.type == static_cast<uint16_t>(MessageType::PAGE_DATA)) {
        ok = deserialize_page_data(reply, writeback, error) &&
             writeback.page_id == request.page_id && writeback.region_id == request.region_id &&
             writeback.region_offset == request.region_offset && receive();
        if (ok && !writeback.data.empty()) traffic_.bytes_received(writeback.data.size());
    }
    if (ok && (incoming.type != static_cast<uint16_t>(MessageType::INVALIDATE_ACK) ||
               reply != payload)) {
        error = "INVALIDATE_ACK invalido o rechazado";
        ok = false;
    }
    close_outgoing(fd);
    return ok;
}

bool NetworkManager::query_region(const NodeInfo& owner, RegionId region_id,
                                  RegionDescriptor& out, std::string& error) {
    if (owner.id == 0 || owner.id == node_id_) {
        error = "destino de REGION_INFO_REQUEST invalido";
        return false;
    }
    if (region_id == 0) {
        error = "REGION_INFO_REQUEST requiere region_id";
        return false;
    }
    if (!require_available(owner, error)) return false;
    const int fd = connect_to(owner.ip_address, owner.tcp_port, error);
    if (fd == -1) return false;

    RegionInfoRequest request;
    request.region_id = region_id;
    const uint64_t message_id = next_message_++;
    const MessageHeader outgoing = make_header(MessageType::REGION_INFO_REQUEST,
        message_id, node_id_, owner.id);
    std::vector<uint8_t> payload;
    MessageHeader incoming{};
    std::vector<uint8_t> reply;
    bool ok = serialize_region_info_request(request, payload, error) &&
              send_message(fd, outgoing, payload, error) &&
              recv_message(fd, incoming, reply, error);
    if (ok) {
        if (incoming.type == static_cast<uint16_t>(MessageType::ERROR_RESPONSE)) {
            error = "el dueno rechazo REGION_INFO_REQUEST";
            if (reply.size() == 2) {
                error += " (codigo " +
                    std::to_string((reply[0] << 8) | reply[1]) + ")";
            }
            ok = false;
        } else if (incoming.type != static_cast<uint16_t>(MessageType::REGION_INFO_RESPONSE) ||
                   incoming.message_id != message_id ||
                   incoming.sender_id != owner.id ||
                   incoming.receiver_id != node_id_) {
            error = "REGION_INFO_RESPONSE invalida";
            ok = false;
        } else {
            RegionInfoResponse info;
            ok = deserialize_region_info_response(reply, info, error);
            if (ok && (info.region_id != region_id || info.owner_id != owner.id)) {
                error = "REGION_INFO_RESPONSE no corresponde a la region solicitada";
                ok = false;
            }
            if (ok) {
                out.region_id = info.region_id;
                out.owner_id = info.owner_id;
                out.size = info.size;
                out.page_size = info.page_size;
            }
        }
    }
    close_outgoing(fd);
    return ok;
}

bool NetworkManager::query_regions(const NodeInfo& owner,
                                   std::vector<RegionDescriptor>& out,
                                   std::string& error) {
    if (owner.id == 0 || owner.id == node_id_) {
        error = "destino de REGION_LIST_REQUEST invalido";
        return false;
    }
    if (!require_available(owner, error)) return false;
    const int fd = connect_to(owner.ip_address, owner.tcp_port, error);
    if (fd == -1) return false;

    const uint64_t message_id = next_message_++;
    const MessageHeader outgoing = make_header(MessageType::REGION_LIST_REQUEST,
        message_id, node_id_, owner.id);
    MessageHeader incoming{};
    std::vector<uint8_t> reply;
    bool ok = send_message(fd, outgoing, {}, error) &&
              recv_message(fd, incoming, reply, error);
    if (ok) {
        if (incoming.type == static_cast<uint16_t>(MessageType::ERROR_RESPONSE)) {
            error = "el dueno rechazo REGION_LIST_REQUEST";
            if (reply.size() == 2) {
                error += " (codigo " +
                    std::to_string((reply[0] << 8) | reply[1]) + ")";
            }
            ok = false;
        } else if (incoming.type != static_cast<uint16_t>(MessageType::REGION_LIST_RESPONSE) ||
                   incoming.message_id != message_id ||
                   incoming.sender_id != owner.id ||
                   incoming.receiver_id != node_id_) {
            error = "REGION_LIST_RESPONSE invalida";
            ok = false;
        } else {
            ok = deserialize_regions(reply, out, error);
        }
    }
    close_outgoing(fd);
    return ok;
}

std::string NetworkManager::query_status(const NodeInfo& node, uint8_t flags, std::string& error) {
    error.clear();
    if (node.id == 0 || node.tcp_port == 0 || (flags & ~STATUS_FLAG_ALL) != 0) {
        error = "nodo o banderas STATUS invalidos";
        return {};
    }
    if (!node_available(node.id)) {
        error = "nodo no disponible segun heartbeat";
        return {};
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(config_.request_timeout_ms);
    const int fd = connect_to(node.ip_address, node.tcp_port, error, deadline);
    if (fd == -1) return {};
    const MessageHeader request = make_header(MessageType::STATUS_REQUEST,
        next_message_++, node_id_, node.id);
    const std::vector<uint8_t> payload = flags ? std::vector<uint8_t>{flags} : std::vector<uint8_t>{};
    MessageHeader response{};
    std::vector<uint8_t> reply;
    bool ok = send_message(fd, request, payload, error, deadline) &&
              recv_message(fd, response, reply, error, deadline);
    close_outgoing(fd);
    if (ok && (response.type != static_cast<uint16_t>(MessageType::STATUS_RESPONSE) ||
               response.message_id != request.message_id || response.sender_id != node.id ||
               response.receiver_id != node_id_ || reply.empty())) {
        error = "STATUS_RESPONSE invalida o consulta rechazada";
        ok = false;
    }
    if (!ok) return {};
    error.clear();
    return std::string(reply.begin(), reply.end());
}

void NetworkManager::leave() {
    pthread_mutex_lock(&mutex_);
    const bool running = running_;
    pthread_mutex_unlock(&mutex_);
    if (!running) return;

    // Los workers siguen activos para confirmar salidas simultaneas.
    for (const auto& peer : nodes()) {
        if (peer.id == node_id_) continue;
        std::string error;
        const int fd = connect_to(peer.ip_address, peer.tcp_port, error);
        bool confirmed = false;
        if (fd != -1) {
            const MessageHeader request = make_header(MessageType::NODE_LEAVE,
                next_message_++, node_id_, peer.id);
            MessageHeader response{};
            std::vector<uint8_t> payload;
            confirmed = send_message(fd, request, {}, error) &&
                        recv_message(fd, response, payload, error) &&
                        response.type == static_cast<uint16_t>(MessageType::NODE_LEAVE_ACK) &&
                        response.message_id == request.message_id &&
                        response.sender_id == peer.id && response.receiver_id == node_id_ &&
                        payload.empty();
            close_outgoing(fd);
        }
        if (!confirmed) {
            std::cerr << "[" << config_.node_name << "] no se confirmo la salida en "
                      << peer.node_name << "\n";
        }
    }
}

int NetworkManager::connect_to(const std::string& host, uint16_t port, std::string& error,
                                IODeadline deadline) {
    if (deadline == IODeadline::max()) {
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.request_timeout_ms);
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    const int result = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (result != 0) {
        error = std::string("no se pudo resolver bootstrap: ") + gai_strerror(result);
        return -1;
    }

    int connected = -1;
    error = "no se pudo conectar al nodo";
    for (const addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
            error = std::string("socket fallo: ") + std::strerror(errno);
            continue;
        }
        pthread_mutex_lock(&mutex_);
        const bool running = running_;
        if (running) outgoing_.insert(fd);
        pthread_mutex_unlock(&mutex_);
        if (!running) {
            close(fd);
            error = "la red esta detenida";
            break;
        }

        if (config_.bind_address != "0.0.0.0") {
            sockaddr_in source{};
            source.sin_family = AF_INET;
            inet_pton(AF_INET, config_.bind_address.c_str(), &source.sin_addr);
            if (bind(fd, reinterpret_cast<sockaddr*>(&source), sizeof(source)) == -1) {
                error = std::string("no se pudo enlazar la conexion saliente: ") + std::strerror(errno);
                close_outgoing(fd);
                continue;
            }
        }

        timeval timeout{};
        timeout.tv_sec = config_.request_timeout_ms / 1000;
        timeout.tv_usec = (config_.request_timeout_ms % 1000) * 1000;
        const int flags = fcntl(fd, F_GETFL);
        bool ok = flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
                  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
                  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
        if (ok && connect(fd, address->ai_addr, address->ai_addrlen) == -1) {
            ok = errno == EINPROGRESS;
            while (ok) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) { ok = false; errno = ETIMEDOUT; break; }
                pollfd output{fd, POLLOUT, 0};
                const int wait_ms = static_cast<int>(std::min<int64_t>(left, std::numeric_limits<int>::max()));
                const int ready = poll(&output, 1, wait_ms);
                if (ready < 0 && errno == EINTR) continue;
                int failure = 0;
                socklen_t size = sizeof(failure);
                ok = ready > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &size) == 0 && failure == 0;
                if (ready == 0) errno = ETIMEDOUT;
                else if (failure) errno = failure;
                break;
            }
        }
        if (ok && fcntl(fd, F_SETFL, flags) == 0) {
            connected = fd;
            break;
        }
        error = std::string("conexion fallo: ") + std::strerror(errno);
        close_outgoing(fd);
    }
    freeaddrinfo(addresses);
    return connected;
}

void NetworkManager::close_outgoing(int fd) {
    pthread_mutex_lock(&mutex_);
    outgoing_.erase(fd);
    close(fd);
    pthread_mutex_unlock(&mutex_);
}

bool NetworkManager::send_error(int fd, const MessageHeader& request,
                               ErrorCode code, std::string& error) {
    const MessageHeader response = make_header(MessageType::ERROR_RESPONSE,
        request.message_id, node_id_, request.sender_id);
    const uint16_t value = static_cast<uint16_t>(code);
    return send_message(fd, response,
        {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF)}, error);
}

bool NetworkManager::join(const std::string& bootstrap, std::string& error) {
    std::string host;
    uint16_t port = 0;
    if (!parse_bootstrap(bootstrap, host, port, error)) return false;
    const int fd = connect_to(host, port, error);
    if (fd == -1) return false;

    std::string address;
    if (!socket_address(fd, true, address, error)) {
        close_outgoing(fd);
        return false;
    }
    if (config_.bind_address == "0.0.0.0") node_table_.update_address(node_id_, address);

    const MessageHeader request = make_header(MessageType::JOIN_REQUEST,
        next_message_++, node_id_, 0);
    std::vector<uint8_t> payload;
    MessageHeader response{};
    std::vector<NodeInfo> bootstrap_node;
    std::vector<NodeInfo> participants;
    history_.record(local_node(), HistoryEvent::JOIN_SENT);
    bool ok = serialize_nodes({local_node()}, payload, error) &&
              send_message(fd, request, payload, error) &&
              recv_message(fd, response, payload, error);
    if (ok && response.type == static_cast<uint16_t>(MessageType::ERROR_RESPONSE)) {
        error = "bootstrap rechazo la incorporacion";
        if (payload.size() == 2) {
            error += " (codigo " + std::to_string((payload[0] << 8) | payload[1]) + ")";
        }
        ok = false;
    }
    if (ok) {
        ok = response.type == static_cast<uint16_t>(MessageType::JOIN_RESPONSE) &&
             response.message_id == request.message_id && response.receiver_id == node_id_ &&
             deserialize_nodes(payload, bootstrap_node, error) && bootstrap_node.size() == 1 &&
             bootstrap_node[0].id == response.sender_id && response.sender_id != node_id_ &&
             bootstrap_node[0].page_size == config_.page_size;
        if (!ok) error = "JOIN_RESPONSE invalida o incompatible";
    }
    if (ok) {
        ok = recv_message(fd, response, payload, error);
        if (ok) {
            ok = response.type == static_cast<uint16_t>(MessageType::NODE_LIST_RESPONSE) &&
                 response.message_id == request.message_id && response.receiver_id == node_id_ &&
                 response.sender_id == bootstrap_node[0].id &&
                 deserialize_nodes(payload, participants, error);
            if (!ok) error = "NODE_LIST_RESPONSE invalida";
        }
    }
    close_outgoing(fd);
    if (!ok) return false;

    bool contains_self = false;
    bool contains_bootstrap = false;
    for (const auto& node : participants) {
        if (node.page_size != config_.page_size) {
            error = "tamano de pagina incompatible con el sistema";
            return false;
        }
        contains_self |= node.id == node_id_;
        contains_bootstrap |= node.id == bootstrap_node[0].id;
    }
    if (!contains_self || !contains_bootstrap) {
        error = "la lista no incluye al nodo local y al bootstrap";
        return false;
    }
    NodeTable received;
    if (!received.merge(participants, error) || !received.add(bootstrap_node[0], error) ||
        !node_table_.merge(participants, error)) {
        return false;
    }
    for (const auto& node : participants) {
        history_.record(node, node.id == node_id_ ? HistoryEvent::JOIN_ACCEPTED : HistoryEvent::NODE_DISCOVERED);
        if (heartbeat_) heartbeat_->observe(node);
    }
    if (heartbeat_) heartbeat_->observe(bootstrap_node[0]);
    std::cout << "[" << config_.node_name << "] incorporado mediante " << bootstrap
              << ", nodos conocidos: " << nodes().size() << "\n" << std::flush;
    return true;
}

bool NetworkManager::handle_join(int fd, const MessageHeader& request,
                                 const std::vector<uint8_t>& payload, std::string& error) {
    std::vector<NodeInfo> incoming;
    if (!deserialize_nodes(payload, incoming, error) || incoming.size() != 1 ||
        incoming[0].id != request.sender_id || incoming[0].id == node_id_) {
        return send_error(fd, request, ErrorCode::INVALID_MESSAGE, error);
    }
    NodeInfo newcomer = incoming[0];
    if (newcomer.page_size != config_.page_size) {
        return send_error(fd, request, ErrorCode::PROTOCOL_MISMATCH, error);
    }
    std::string local_address;
    if (!socket_address(fd, false, newcomer.ip_address, error) ||
        !socket_address(fd, true, local_address, error)) return false;
    if (config_.bind_address == "0.0.0.0") node_table_.update_address(node_id_, local_address);
    if (!node_table_.add(newcomer, error)) {
        return send_error(fd, request, ErrorCode::INVALID_STATE, error);
    }

    history_.record(newcomer, HistoryEvent::JOIN_ACCEPTED);
    if (heartbeat_) heartbeat_->observe(newcomer);
    const auto participants = nodes();
    std::vector<uint8_t> reply;
    MessageHeader response = make_header(MessageType::JOIN_RESPONSE,
        request.message_id, node_id_, newcomer.id);
    if (!serialize_nodes({local_node()}, reply, error) ||
        !send_message(fd, response, reply, error)) return false;
    response.type = static_cast<uint16_t>(MessageType::NODE_LIST_RESPONSE);
    if (!serialize_nodes(participants, reply, error) ||
        !send_message(fd, response, reply, error)) return false;

    std::cout << "[" << config_.node_name << "] registrado " << newcomer.node_name
              << " (" << newcomer.id << "), nodos conocidos: "
              << participants.size() << "\n" << std::flush;
    announce(newcomer, participants);
    return true;
}

void NetworkManager::announce(const NodeInfo& node, const std::vector<NodeInfo>& targets,
                              NodeId except) {
    std::string error;
    std::vector<uint8_t> payload;
    if (!serialize_nodes({node}, payload, error)) return;
    // No espera confirmaciones entre workers: evita ciclos con un solo worker por nodo.
    for (const auto& target : targets) {
        if (target.id == node_id_ || target.id == node.id || target.id == except) continue;
        const int fd = connect_to(target.ip_address, target.tcp_port, error);
        bool sent = false;
        if (fd != -1) {
            const MessageHeader message = make_header(MessageType::NODE_ANNOUNCE,
                next_message_++, node_id_, target.id);
            sent = send_message(fd, message, payload, error);
            close_outgoing(fd);
        }
        if (!sent) {
            std::cerr << "[" << config_.node_name << "] no se pudo anunciar a "
                      << target.node_name << ": " << error << "\n";
        }
    }
}
