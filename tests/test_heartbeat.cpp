#include "heartbeat.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
    void require(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FALLO: " << message << std::endl;
            std::exit(1);
        }
    }

    uint16_t free_udp_port() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        require(fd != -1, "socket UDP de prueba");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "enlazar puerto UDP de prueba");
        socklen_t size = sizeof(address);
        require(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0,
                "obtener puerto UDP de prueba");
        const uint16_t port = ntohs(address.sin_port);
        close(fd);
        return port;
    }

    NodeInfo make_node(NodeId id, const std::string& name, uint16_t port) {
        NodeInfo node;
        node.id = id;
        node.node_name = name;
        node.ip_address = "127.0.0.1";
        node.tcp_port = 1;
        node.heartbeat_port = port;
        node.memory_size = 4096;
        node.page_size = 4096;
        return node;
    }

    bool wait_for(const std::function<bool()>& condition, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return condition();
    }

    bool has_heartbeat(const HeartbeatManager& manager, NodeId id) {
        for (const auto& item : manager.snapshot()) {
            if (item.node_id == id) return item.has_heartbeat;
        }
        return false;
    }
}

int main() {
    alarm(20);

    HeartbeatMessage message;
    message.node_id = 12345;
    message.timestamp = 999;
    message.sequence_number = 7;
    std::vector<uint8_t> bytes;
    std::string error;
    require(serialize_heartbeat(message, bytes, error), error);
    require(bytes.size() == DSM_HEARTBEAT_SIZE, "heartbeat debe ocupar 20 bytes");
    HeartbeatMessage decoded;
    require(deserialize_heartbeat(bytes, decoded, error), error);
    require(decoded.node_id == message.node_id && decoded.timestamp == message.timestamp &&
            decoded.sequence_number == message.sequence_number, "roundtrip de heartbeat");
    require(!deserialize_heartbeat(std::vector<uint8_t>(19), decoded, error),
            "rechazar heartbeat de tamano invalido");
    require(!serialize_heartbeat(HeartbeatMessage{}, bytes, error),
            "rechazar heartbeat sin node_id");

    HeartbeatManager ephemeral("127.0.0.1", 0, 50, 150, 300);
    require(ephemeral.start(303, error), error);
    require(ephemeral.port() != 0, "heartbeat_port=0 debe enlazar un puerto efimero");
    ephemeral.stop();

    const NodeId id_a = 101;
    const NodeId id_b = 202;
    const uint16_t port_a = free_udp_port();
    const uint16_t port_b = free_udp_port();
    require(port_a != 0 && port_b != 0 && port_a != port_b, "puertos distintas para A y B");
    const NodeInfo a = make_node(id_a, "A", port_a);
    const NodeInfo b = make_node(id_b, "B", port_b);

    std::mutex events_mutex;
    std::vector<AvailabilityTransition> events;
    auto handler = [&](const NodeInfo& node, const AvailabilityTransition& transition) {
        if (node.id != id_b) return;
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(transition);
    };
    auto saw = [&](AvailabilityState state) {
        std::lock_guard<std::mutex> lock(events_mutex);
        for (const auto& event : events) {
            if (event.current == state) return true;
        }
        return false;
    };

    HeartbeatManager hb_a("127.0.0.1", port_a, 50, 150, 300);
    HeartbeatManager hb_b("127.0.0.1", port_b, 50, 150, 300);
    hb_a.set_peer_provider([&] { return std::vector<NodeInfo>{a, b}; });
    hb_b.set_peer_provider([&] { return std::vector<NodeInfo>{a, b}; });
    hb_a.set_transition_handler(handler);

    require(hb_a.start(id_a, error), error);
    require(hb_b.start(id_b, error), error);
    hb_a.observe(b);
    hb_b.observe(a);

    require(wait_for([&] { return has_heartbeat(hb_a, id_b); }, 2000),
            "A no recibio heartbeats de B");
    require(hb_a.available(id_b) && hb_a.state(id_b) == AvailabilityState::ACTIVE,
            "B debe estar ACTIVE mientras envia heartbeats");
    require(wait_for([&] { return has_heartbeat(hb_b, id_a); }, 2000),
            "B no recibio heartbeats de A");

    // B desaparece sin avisar: A debe pasar por SUSPECT y luego UNAVAILABLE.
    hb_b.stop();
    require(wait_for([&] { return hb_a.state(id_b) == AvailabilityState::SUSPECT; }, 2000),
            "A no marco SUSPECT tras el timeout");
    require(wait_for([&] { return hb_a.state(id_b) == AvailabilityState::UNAVAILABLE; }, 2000),
            "A no marco UNAVAILABLE tras el timeout extendido");
    require(!hb_a.available(id_b), "UNAVAILABLE debe bloquear nuevas operaciones");
    require(saw(AvailabilityState::SUSPECT) && saw(AvailabilityState::UNAVAILABLE),
            "faltan eventos de sospecha o indisponibilidad");
    require(hb_a.active_snapshot().empty(),
            "un participante UNAVAILABLE no debe aparecer activo");

    // B vuelve con el mismo NodeId y puerto: A debe recuperarlo.
    HeartbeatManager hb_b2("127.0.0.1", port_b, 50, 150, 300);
    hb_b2.set_peer_provider([&] { return std::vector<NodeInfo>{a, b}; });
    require(hb_b2.start(id_b, error), error);
    hb_b2.observe(a);
    require(wait_for([&] { return hb_a.state(id_b) == AvailabilityState::ACTIVE; }, 2000),
            "A no recupero a B");
    require(hb_a.available(id_b) && saw(AvailabilityState::ACTIVE),
            "faltan el evento y el estado de recuperacion");
    require(wait_for([&] { return has_heartbeat(hb_a, id_b); }, 2000),
            "A no reanudo la recepcion de heartbeats");

    // Un nodo desconocido o eliminado no se considera caido.
    hb_a.forget(id_b);
    require(hb_a.state(id_b) == AvailabilityState::ACTIVE && hb_a.available(9999),
            "nodos desconocidos deben seguir disponibles");

    hb_b2.stop();
    hb_a.stop();
    std::cout << "test_heartbeat: OK (20 bytes, efimero, SUSPECT, UNAVAILABLE, recuperacion y olvido)\n";
    return 0;
}
