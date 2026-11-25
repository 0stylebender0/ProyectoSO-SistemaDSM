#include "heartbeat.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>

namespace {
    uint64_t monotonic_ms() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint64_t system_ms() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    bool same_endpoint(const sockaddr_in& a, const sockaddr_in& b) {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }
}

HeartbeatManager::HeartbeatManager(std::string bind_address, uint16_t port,
                                   uint32_t interval_ms, uint32_t suspect_after_ms,
                                   uint32_t unavailable_after_ms)
    : bind_address_(std::move(bind_address)), requested_port_(port) {
    interval_ms_ = interval_ms == 0 ? 1 : interval_ms;
    suspect_after_ms_ = suspect_after_ms == 0 ? interval_ms_ : suspect_after_ms;
    unavailable_after_ms_ = unavailable_after_ms > suspect_after_ms_
        ? unavailable_after_ms : suspect_after_ms_ + interval_ms_;
}

HeartbeatManager::~HeartbeatManager() {
    stop();
    pthread_mutex_destroy(&mutex_);
}

void HeartbeatManager::set_peer_provider(PeerProvider provider) {
    peer_provider_ = std::move(provider);
}

void HeartbeatManager::set_transition_handler(TransitionHandler handler) {
    transition_handler_ = std::move(handler);
}

bool HeartbeatManager::start(NodeId node_id, std::string& error) {
    if (node_id == 0) {
        error = "heartbeat requiere un node_id valido";
        return false;
    }

    pthread_mutex_lock(&mutex_);
    if (fd_ != -1) {
        pthread_mutex_unlock(&mutex_);
        error = "el heartbeat ya esta iniciado";
        return false;
    }
    pthread_mutex_unlock(&mutex_);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(requested_port_);
    const std::string bind_text = bind_address_.empty() ? "0.0.0.0" : bind_address_;
    if (inet_pton(AF_INET, bind_text.c_str(), &address.sin_addr) != 1) {
        error = "bind_address de heartbeat debe ser una direccion IPv4 valida";
        return false;
    }

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        error = std::string("socket UDP de heartbeat fallo: ") + std::strerror(errno);
        return false;
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        error = "no se pudo enlazar el puerto UDP de heartbeat " +
                std::to_string(requested_port_) + ": " + std::strerror(errno);
        close(fd);
        return false;
    }

    sockaddr_in local{};
    socklen_t local_size = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_size) == -1) {
        error = std::string("getsockname de heartbeat fallo: ") + std::strerror(errno);
        close(fd);
        return false;
    }

    pthread_mutex_lock(&mutex_);
    self_id_ = node_id;
    fd_ = fd;
    port_ = ntohs(local.sin_port);
    sequence_ = 0;
    next_send_ms_ = monotonic_ms();
    running_ = true;
    const int result = pthread_create(&thread_, nullptr, thread_entry, this);
    if (result != 0) {
        running_ = false;
        fd_ = -1;
        port_ = 0;
        pthread_mutex_unlock(&mutex_);
        close(fd);
        error = std::string("no se pudo crear el hilo de heartbeat: ") +
                std::strerror(result);
        return false;
    }
    thread_started_ = true;
    pthread_mutex_unlock(&mutex_);
    return true;
}

void HeartbeatManager::stop() {
    pthread_mutex_lock(&mutex_);
    running_ = false;
    const bool started = thread_started_;
    pthread_mutex_unlock(&mutex_);

    if (started) pthread_join(thread_, nullptr);

    pthread_mutex_lock(&mutex_);
    thread_started_ = false;
    const int fd = fd_;
    fd_ = -1;
    port_ = 0;
    self_id_ = 0;
    sequence_ = 0;
    peers_.clear();
    pthread_mutex_unlock(&mutex_);

    if (fd != -1) close(fd);
}

void* HeartbeatManager::thread_entry(void* context) {
    static_cast<HeartbeatManager*>(context)->loop();
    return nullptr;
}

void HeartbeatManager::loop() {
    while (true) {
        int fd = -1;
        int wait_ms = 50;
        {
            pthread_mutex_lock(&mutex_);
            if (!running_) {
                pthread_mutex_unlock(&mutex_);
                return;
            }
            fd = fd_;
            const uint64_t now = monotonic_ms();
            const uint64_t until_send = next_send_ms_ > now ? next_send_ms_ - now : 0;
            wait_ms = static_cast<int>(std::min<uint64_t>(until_send ? until_send : 1, 50));
            pthread_mutex_unlock(&mutex_);
        }

        pollfd input{fd, POLLIN, 0};
        const int ready = poll(&input, 1, wait_ms);
        if (ready < 0 && errno == EINTR) continue;

        receive_available();
        refresh_and_send();
        expire();
    }
}

void HeartbeatManager::observe(const NodeInfo& node) {
    if (node.id == 0 || node.heartbeat_port == 0) return;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(node.heartbeat_port);
    if (inet_pton(AF_INET, node.ip_address.c_str(), &address.sin_addr) != 1) return;

    const uint64_t now = monotonic_ms();
    pthread_mutex_lock(&mutex_);
    if (node.id == self_id_) {
        pthread_mutex_unlock(&mutex_);
        return;
    }
    const auto found = peers_.find(node.id);
    if (found == peers_.end()) {
        Peer peer;
        peer.info = node;
        peer.address = address;
        peer.has_address = true;
        peer.last_seen_ms = now;
        peers_.emplace(node.id, std::move(peer));
    } else {
        found->second.info = node;
        if (!found->second.has_address || !same_endpoint(found->second.address, address)) {
            found->second.address = address;
            found->second.has_address = true;
            // Un endpoint nuevo reinicia la gracia inicial del participante.
            found->second.last_seen_ms = now;
        }
    }
    pthread_mutex_unlock(&mutex_);
}

void HeartbeatManager::forget(NodeId node_id) {
    pthread_mutex_lock(&mutex_);
    peers_.erase(node_id);
    pthread_mutex_unlock(&mutex_);
}

uint16_t HeartbeatManager::port() const {
    pthread_mutex_lock(&mutex_);
    const uint16_t value = port_;
    pthread_mutex_unlock(&mutex_);
    return value;
}

AvailabilityState HeartbeatManager::state(NodeId node_id) const {
    pthread_mutex_lock(&mutex_);
    const auto found = peers_.find(node_id);
    const AvailabilityState value = found == peers_.end()
        ? AvailabilityState::ACTIVE : found->second.state;
    pthread_mutex_unlock(&mutex_);
    return value;
}

bool HeartbeatManager::available(NodeId node_id) const {
    return state(node_id) != AvailabilityState::UNAVAILABLE;
}

std::vector<HeartbeatSnapshot> HeartbeatManager::snapshot() const {
    const uint64_t now = monotonic_ms();
    std::vector<HeartbeatSnapshot> result;
    pthread_mutex_lock(&mutex_);
    result.reserve(peers_.size());
    for (const auto& entry : peers_) {
        const Peer& peer = entry.second;
        HeartbeatSnapshot item;
        item.node_id = entry.first;
        item.state = peer.state;
        item.has_heartbeat = peer.has_heartbeat;
        item.last_sequence = peer.last_sequence;
        item.age_ms = now >= peer.last_seen_ms ? now - peer.last_seen_ms : 0;
        result.push_back(item);
    }
    pthread_mutex_unlock(&mutex_);
    return result;
}

std::vector<HeartbeatSnapshot> HeartbeatManager::active_snapshot() const {
    std::vector<HeartbeatSnapshot> result = snapshot();
    result.erase(std::remove_if(result.begin(), result.end(),
        [](const HeartbeatSnapshot& item) {
            return item.state == AvailabilityState::UNAVAILABLE;
        }), result.end());
    return result;
}

std::size_t HeartbeatManager::active_count() const {
    return active_snapshot().size();
}

void HeartbeatManager::refresh_and_send() {
    if (peer_provider_) {
        const std::vector<NodeInfo> nodes = peer_provider_();
        for (const auto& node : nodes) observe(node);
    }

    const uint64_t now = monotonic_ms();
    HeartbeatMessage message;
    std::vector<sockaddr_in> targets;
    {
        pthread_mutex_lock(&mutex_);
        if (!running_ || now < next_send_ms_) {
            pthread_mutex_unlock(&mutex_);
            return;
        }
        if (next_send_ms_ + interval_ms_ <= now) {
            next_send_ms_ = now + interval_ms_;
        } else {
            next_send_ms_ += interval_ms_;
        }
        message.node_id = self_id_;
        message.timestamp = system_ms();
        message.sequence_number = sequence_++;
        for (const auto& entry : peers_) {
            if (entry.second.has_address) targets.push_back(entry.second.address);
        }
        pthread_mutex_unlock(&mutex_);
    }

    std::string error;
    std::vector<uint8_t> payload;
    if (!serialize_heartbeat(message, payload, error)) return;
    for (const auto& target : targets) {
        sockaddr_in address = target;
        sendto(fd_, payload.data(), payload.size(), 0,
               reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    }
}

void HeartbeatManager::receive_available() {
    std::vector<std::pair<Peer, AvailabilityTransition>> transitions;
    uint8_t buffer[64];
    while (true) {
        sockaddr_in from{};
        socklen_t from_size = sizeof(from);
        const ssize_t count = recvfrom(fd_, buffer, sizeof(buffer), MSG_DONTWAIT,
                                       reinterpret_cast<sockaddr*>(&from), &from_size);
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (count == 0) continue;

        std::vector<uint8_t> data(buffer, buffer + count);
        HeartbeatMessage message;
        std::string error;
        if (!deserialize_heartbeat(data, message, error)) continue;

        const uint64_t now = monotonic_ms();
        pthread_mutex_lock(&mutex_);
        const auto found = peers_.find(message.node_id);
        if (found != peers_.end()) {
            Peer& peer = found->second;
            peer.last_seen_ms = now;
            peer.last_sequence = message.sequence_number;
            peer.has_heartbeat = true;
            if (peer.state != AvailabilityState::ACTIVE) {
                const Peer previous = peer;
                transitions.emplace_back(previous,
                    AvailabilityTransition{peer.state, AvailabilityState::ACTIVE});
                peer.state = AvailabilityState::ACTIVE;
            }
        }
        pthread_mutex_unlock(&mutex_);
    }
    apply_transitions(transitions);
}

void HeartbeatManager::expire() {
    const uint64_t now = monotonic_ms();
    std::vector<std::pair<Peer, AvailabilityTransition>> transitions;
    pthread_mutex_lock(&mutex_);
    for (auto& entry : peers_) {
        Peer& peer = entry.second;
        AvailabilityState next = AvailabilityState::ACTIVE;
        const uint64_t age = now >= peer.last_seen_ms ? now - peer.last_seen_ms : 0;
        if (age > unavailable_after_ms_) {
            next = AvailabilityState::UNAVAILABLE;
        } else if (age > suspect_after_ms_) {
            next = AvailabilityState::SUSPECT;
        }
        if (next != peer.state) {
            const Peer previous = peer;
            transitions.emplace_back(previous,
                AvailabilityTransition{peer.state, next});
            peer.state = next;
        }
    }
    pthread_mutex_unlock(&mutex_);
    apply_transitions(transitions);
}

void HeartbeatManager::apply_transitions(
    const std::vector<std::pair<Peer, AvailabilityTransition>>& transitions) {
    if (!transition_handler_) return;
    for (const auto& entry : transitions) {
        transition_handler_(entry.first.info, entry.second);
    }
}
