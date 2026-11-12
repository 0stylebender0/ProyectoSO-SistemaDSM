#include "network.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
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
        uint32_t value = 0;
        for (std::size_t i = colon + 1; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') {
                error = "puerto bootstrap invalido";
                return false;
            }
            value = value * 10 + static_cast<uint32_t>(text[i] - '0');
            if (value > 65535) {
                error = "puerto bootstrap fuera de rango";
                return false;
            }
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

NetworkManager::NetworkManager(const Config& config) : config_(config) {}

NetworkManager::~NetworkManager() {
    stop();
    pthread_cond_destroy(&ready_);
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

    if (!generate_node_id(node_id_, error)) {
        stop();
        return false;
    }
    NodeInfo self;
    self.id = node_id_;
    self.node_name = config_.node_name;
    self.ip_address = config_.bind_address == "0.0.0.0" ? "127.0.0.1" : config_.bind_address;
    self.tcp_port = port_;
    self.heartbeat_port = config_.heartbeat_port;
    self.memory_size = config_.memory_size;
    self.page_size = config_.page_size;
    if (!node_table_.add(self, error)) {
        stop();
        return false;
    }

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

    const int result = pthread_create(&listener_, nullptr, listener_entry, this);
    if (result != 0) {
        error = std::string("no se pudo crear el listener: ") + std::strerror(result);
        stop();
        return false;
    }
    listener_started_ = true;
    if (!config_.bootstrap_nodes.empty() && !join(config_.bootstrap_nodes, error)) {
        stop();
        return false;
    }
    return true;
}

void NetworkManager::stop() {
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

        handle_connection(fd);

        pthread_mutex_lock(&mutex_);
        connections_.erase(fd);
        close(fd);
        pthread_mutex_unlock(&mutex_);
    }
}

void NetworkManager::handle_connection(int fd) {
    MessageHeader request{};
    std::vector<uint8_t> payload;
    std::string error;

    while (recv_message(fd, request, payload, error)) {
        if (request.receiver_id != 0 && request.receiver_id != node_id_) {
            if (!send_error(fd, request, ErrorCode::UNKNOWN_NODE, error)) return;
            continue;
        }
        if (request.type == static_cast<uint16_t>(MessageType::JOIN_REQUEST)) {
            if (!handle_join(fd, request, payload, error)) return;
            continue;
        }
        if (request.type == static_cast<uint16_t>(MessageType::NODE_ANNOUNCE)) {
            std::vector<NodeInfo> announced;
            bool inserted = false;
            if (!deserialize_nodes(payload, announced, error) || announced.size() != 1) {
                if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return;
            } else if (announced[0].page_size != config_.page_size) {
                if (!send_error(fd, request, ErrorCode::PROTOCOL_MISMATCH, error)) return;
            } else if (!node_table_.add(announced[0], error, &inserted)) {
                if (!send_error(fd, request, ErrorCode::INVALID_STATE, error)) return;
            } else if (inserted) {
                announce(announced[0], nodes(), request.sender_id);
            }
            continue;
        }

        MessageHeader response = make_header(MessageType::STATUS_RESPONSE,
            request.message_id, node_id_, request.sender_id);

        std::vector<uint8_t> reply;
        if (request.type == static_cast<uint16_t>(MessageType::STATUS_REQUEST) &&
            payload.empty()) {
            response.type = static_cast<uint16_t>(MessageType::STATUS_RESPONSE);
            const std::string status = "node_name=" + config_.node_name +
                "\ntcp_port=" + std::to_string(port_) +
                "\nworker_count=" + std::to_string(config_.worker_count) + "\n";
            reply.assign(status.begin(), status.end());
        } else if (request.type == static_cast<uint16_t>(MessageType::NODE_LIST_REQUEST) &&
                   payload.empty()) {
            response.type = static_cast<uint16_t>(MessageType::NODE_LIST_RESPONSE);
            if (!serialize_nodes(nodes(), reply, error)) {
                if (!send_error(fd, request, ErrorCode::OUT_OF_MEMORY, error)) return;
                continue;
            }
        } else {
            if (!send_error(fd, request, ErrorCode::INVALID_MESSAGE, error)) return;
            continue;
        }

        if (!send_message(fd, response, reply, error)) return;
    }
}

NodeInfo NetworkManager::local_node() const {
    NodeInfo node;
    node_table_.find(node_id_, node);
    return node;
}

int NetworkManager::connect_to(const std::string& host, uint16_t port, std::string& error) {
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
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
            connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
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
