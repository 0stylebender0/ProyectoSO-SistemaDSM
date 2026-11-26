#include "heartbeat.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
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

    void manager_edges() {
        std::string error;
        HeartbeatManager invalid("127.0.0.1", 0, 20, 60, 120);
        require(!invalid.start(0, error), "heartbeat necesita un node_id distinto de cero");

        HeartbeatManager bad_bind("no-es-una-ip", 0, 20, 60, 120);
        require(!bad_bind.start(7, error), "rechazar bind_address invalido");

        HeartbeatManager manager("127.0.0.1", 0, 20, 60, 120);
        require(manager.start(77, error), error);
        const uint16_t port = manager.port();
        require(port != 0, "puerto efimero del gestor");
        require(!manager.start(77, error), "rechazar doble start");
        require(manager.port() == port, "doble start conserva el puerto");
        manager.stop();
        require(manager.port() == 0, "stop libera el puerto");
        manager.stop();
        require(manager.start(78, error), error);
        require(manager.port() != 0, "reiniciar el gestor");
        manager.stop();
    }

    void malformed_datagrams() {
        std::string error;
        HeartbeatManager manager("127.0.0.1", 0, 20, 5000, 10000);
        require(manager.start(500, error), error);

        const NodeId known_id = 501;
        const NodeId unknown_id = 502;
        const NodeInfo known = make_node(known_id, "known", free_udp_port());
        manager.observe(known);
        require(manager.snapshot().size() == 1 &&
                !has_heartbeat(manager, known_id) &&
                manager.state(known_id) == AvailabilityState::ACTIVE,
                "el par observado empieza ACTIVE sin heartbeat");

        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        require(fd != -1, "socket UDP emisor");
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        destination.sin_port = htons(manager.port());
        auto send = [&](const std::vector<uint8_t>& data) {
            const ssize_t count = sendto(fd, data.data(), data.size(), 0,
                reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
            require(count == static_cast<ssize_t>(data.size()), "enviar datagrama de prueba");
        };

        for (std::size_t size : {std::size_t{1}, std::size_t{18}, std::size_t{19},
                                 std::size_t{21}, std::size_t{64}, std::size_t{65}}) {
            send(std::vector<uint8_t>(size, 0));
        }
        send(std::vector<uint8_t>(DSM_HEARTBEAT_SIZE, 0));

        HeartbeatMessage unknown;
        unknown.node_id = unknown_id;
        unknown.timestamp = 1;
        unknown.sequence_number = 1;
        std::vector<uint8_t> bytes;
        require(serialize_heartbeat(unknown, bytes, error), error);
        send(bytes);

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto snapshot = manager.snapshot();
        require(snapshot.size() == 1, "un nodo desconocido no se agrega a la tabla");
        require(!has_heartbeat(manager, known_id) &&
                manager.state(known_id) == AvailabilityState::ACTIVE,
                "los datagramas malformados se descartan");
        require(manager.active_count() == 1, "el par conocido sigue activo");

        HeartbeatMessage valid;
        valid.node_id = known_id;
        valid.timestamp = 999;
        valid.sequence_number = 7;
        require(serialize_heartbeat(valid, bytes, error), error);
        send(bytes);
        require(wait_for([&] { return has_heartbeat(manager, known_id); }, 2000),
                "un heartbeat valido se acepta despues de los malformados");

        bool sequence_ok = false;
        for (const auto& item : manager.snapshot()) {
            if (item.node_id == known_id) {
                sequence_ok = item.last_sequence == 7 && item.state == AvailabilityState::ACTIVE;
            }
        }
        require(sequence_ok, "conservar la secuencia del heartbeat valido");

        send(std::vector<uint8_t>(19, 0));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(has_heartbeat(manager, known_id), "un malformado posterior no borra el estado");

        close(fd);
        manager.stop();
    }
}

int main() {
    alarm(25);

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
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{18},
                             std::size_t{21}, std::size_t{64}}) {
        require(!deserialize_heartbeat(std::vector<uint8_t>(size), decoded, error),
                "rechazar heartbeat con tamano " + std::to_string(size));
    }
    std::vector<uint8_t> zero_id(DSM_HEARTBEAT_SIZE, 0);
    require(!deserialize_heartbeat(zero_id, decoded, error),
            "rechazar heartbeat con node_id cero");
    require(!serialize_heartbeat(HeartbeatMessage{}, bytes, error),
            "rechazar heartbeat sin node_id");
    HeartbeatMessage limits;
    limits.node_id = std::numeric_limits<uint64_t>::max();
    limits.timestamp = std::numeric_limits<uint64_t>::max();
    limits.sequence_number = std::numeric_limits<uint32_t>::max();
    require(serialize_heartbeat(limits, bytes, error) && bytes.size() == DSM_HEARTBEAT_SIZE,
            "serializar los limites del heartbeat");
    require(deserialize_heartbeat(bytes, decoded, error) &&
            decoded.node_id == limits.node_id && decoded.timestamp == limits.timestamp &&
            decoded.sequence_number == limits.sequence_number,
            "roundtrip de los limites del heartbeat");
    require(bytes[0] == 0xFF && bytes[7] == 0xFF && bytes[16] == 0xFF && bytes[19] == 0xFF,
            "heartbeat en orden de bytes de red");

    manager_edges();
    malformed_datagrams();

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
    std::cout << "test_heartbeat: OK (20 bytes, efimero, limites, malformados, SUSPECT, UNAVAILABLE, recuperacion y olvido)\n";
    return 0;
}
