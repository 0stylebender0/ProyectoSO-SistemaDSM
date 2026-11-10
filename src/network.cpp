#include "network.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>

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
    return true;
}

void NetworkManager::stop() {
    pthread_mutex_lock(&mutex_);
    running_ = false;
    // Desbloquea recv/send antes de esperar a los workers.
    for (int fd : connections_) {
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

    // El listener ya termino y no puede usar un descriptor que el SO reutilice.
    if (listen_fd_ != -1) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    port_ = 0;
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
        MessageHeader response{};
        response.magic = DSM_MAGIC;
        response.version = DSM_PROTOCOL_VERSION;
        response.message_id = request.message_id;
        response.receiver_id = request.sender_id;

        std::vector<uint8_t> reply;
        if (request.type == static_cast<uint16_t>(MessageType::STATUS_REQUEST) &&
            payload.empty()) {
            response.type = static_cast<uint16_t>(MessageType::STATUS_RESPONSE);
            const std::string status = "node_name=" + config_.node_name +
                "\ntcp_port=" + std::to_string(port_) +
                "\nworker_count=" + std::to_string(config_.worker_count) + "\n";
            reply.assign(status.begin(), status.end());
        } else {
            response.type = static_cast<uint16_t>(MessageType::ERROR_RESPONSE);
            const uint16_t code = static_cast<uint16_t>(ErrorCode::INVALID_MESSAGE);
            reply = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code & 0xFF)};
        }

        if (!send_message(fd, response, reply, error)) return;
    }
}
