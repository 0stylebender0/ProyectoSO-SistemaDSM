#pragma once

#include "node.hpp"
#include "protocol.hpp"

#include <pthread.h>
#include <netinet/in.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Disponibilidad observada por el heartbeat UDP. Un nodo desconocido se
// considera disponible: la tabla de disponibilidad es informativa y solo
// bloquea operaciones cuando el nodo fue marcado UNAVAILABLE.
enum class AvailabilityState : uint8_t {
    ACTIVE      = 0,
    SUSPECT     = 1,
    UNAVAILABLE = 2,
};

inline const char* availability_state_name(AvailabilityState state) {
    switch (state) {
        case AvailabilityState::ACTIVE:      return "ACTIVE";
        case AvailabilityState::SUSPECT:     return "SUSPECT";
        case AvailabilityState::UNAVAILABLE: return "UNAVAILABLE";
    }
    return "DESCONOCIDO";
}

struct HeartbeatSnapshot {
    NodeId node_id = 0;
    AvailabilityState state = AvailabilityState::ACTIVE;
    bool has_heartbeat = false;
    uint32_t last_sequence = 0;
    uint64_t age_ms = 0;  // desde el ultimo heartbeat, o desde observe()
};

struct AvailabilityTransition {
    AvailabilityState previous = AvailabilityState::ACTIVE;
    AvailabilityState current = AvailabilityState::ACTIVE;
};

// Envia heartbeats UDP a los nodos conocidos, recibe los de los demas y
// mantiene una tabla de disponibilidad independiente de la tabla de nodos.
// La lista de pares se obtiene mediante PeerProvider; asi este modulo no
// depende de NetworkManager ni de NodeTable.
class HeartbeatManager {
public:
    using PeerProvider = std::function<std::vector<NodeInfo>()>;
    using TransitionHandler = std::function<void(
        const NodeInfo&, const AvailabilityTransition&)>;

    HeartbeatManager(std::string bind_address, uint16_t port,
                     uint32_t interval_ms, uint32_t suspect_after_ms,
                     uint32_t unavailable_after_ms);
    ~HeartbeatManager();

    HeartbeatManager(const HeartbeatManager&) = delete;
    HeartbeatManager& operator=(const HeartbeatManager&) = delete;

    // Debe llamarse antes de start(). El proveedor y el manejador se leen
    // desde el hilo de heartbeat; no deben cambiarse en ejecucion.
    void set_peer_provider(PeerProvider provider);
    void set_transition_handler(TransitionHandler handler);

    bool start(NodeId node_id, std::string& error);
    void stop();

    // Registra o actualiza el endpoint UDP de un participante. Ignora al
    // nodo local, ids invalidos y nodos sin puerto de heartbeat.
    void observe(const NodeInfo& node);
    void forget(NodeId node_id);

    uint16_t port() const;

    AvailabilityState state(NodeId node_id) const;
    // UNAVAILABLE bloquea; SUSPECT y los nodos desconocidos se consideran
    // disponibles para no interrumpir operaciones por una racha perdida.
    bool available(NodeId node_id) const;

    std::vector<HeartbeatSnapshot> snapshot() const;
    std::vector<HeartbeatSnapshot> active_snapshot() const;
    std::size_t active_count() const;

private:
    struct Peer {
        NodeInfo info;
        sockaddr_in address{};
        bool has_address = false;
        uint64_t last_seen_ms = 0;
        uint32_t last_sequence = 0;
        bool has_heartbeat = false;
        AvailabilityState state = AvailabilityState::ACTIVE;
    };

    static void* thread_entry(void* context);
    void loop();
    void refresh_and_send();
    void receive_available();
    void expire();
    void apply_transitions(const std::vector<std::pair<Peer, AvailabilityTransition>>& transitions);

    std::string bind_address_;
    uint16_t requested_port_ = 0;
    uint32_t interval_ms_ = 1000;
    uint32_t suspect_after_ms_ = 5000;
    uint32_t unavailable_after_ms_ = 6000;

    int fd_ = -1;
    uint16_t port_ = 0;
    NodeId self_id_ = 0;
    bool running_ = false;
    bool thread_started_ = false;
    pthread_t thread_{};

    mutable pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::map<NodeId, Peer> peers_;
    uint32_t sequence_ = 0;
    uint64_t next_send_ms_ = 0;

    PeerProvider peer_provider_;
    TransitionHandler transition_handler_;
};
