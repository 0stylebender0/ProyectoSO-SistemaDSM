#include "network.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FALLO: " << message << "\n";
        ++failures;
    }
}

NodeInfo sample_node(NodeId id) {
    NodeInfo node;
    node.id = id;
    node.node_name = "test";
    node.ip_address = "127.0.0.1";
    node.tcp_port = 5000;
    node.heartbeat_port = 5001;
    node.memory_size = 104857600;
    node.page_size = 4096;
    return node;
}

bool wait_for_membership(const std::vector<NetworkManager*>& cluster) {
    std::vector<NodeId> expected;
    for (const auto* node : cluster) expected.push_back(node->node_id());
    std::sort(expected.begin(), expected.end());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        bool complete = true;
        for (const auto* node : cluster) {
            std::vector<NodeId> actual;
            for (const auto& peer : node->nodes()) actual.push_back(peer.id);
            complete &= actual == expected;
        }
        if (complete) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

int connect_to(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const timeval timeout{2, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

}

int main() {
    std::string error;
    NodeInfo original = sample_node(0x0102030405060708ULL);
    std::vector<uint8_t> bytes;
    std::vector<NodeInfo> decoded;
    check(serialize_nodes({original}, bytes, error), "serializar informacion de nodo");
    check(bytes.size() == 38 && bytes[3] == 1 && bytes[4] == 1 && bytes[11] == 8 &&
          bytes[12] == 127 && bytes[15] == 1 && bytes[16] == 0x13 && bytes[17] == 0x88,
          "registro de nodo en big-endian");
    const bool decoded_ok = deserialize_nodes(bytes, decoded, error) && decoded.size() == 1;
    check(decoded_ok, "deserializar informacion de nodo");
    if (decoded_ok) {
        const auto& node = decoded[0];
        check(node.id == original.id && node.node_name == original.node_name &&
              node.ip_address == original.ip_address && node.tcp_port == original.tcp_port &&
              node.heartbeat_port == original.heartbeat_port &&
              node.memory_size == original.memory_size && node.page_size == original.page_size,
              "conservar todos los campos del nodo");
    }
    auto truncated = bytes;
    truncated.pop_back();
    check(!deserialize_nodes(truncated, decoded, error), "rechazar registro incompleto");
    auto trailing = bytes;
    trailing.push_back(0);
    check(!deserialize_nodes(trailing, decoded, error), "rechazar bytes sobrantes");
    auto duplicate = bytes;
    duplicate[3] = 2;
    duplicate.insert(duplicate.end(), bytes.begin() + 4, bytes.end());
    check(!deserialize_nodes(duplicate, decoded, error), "rechazar NodeId repetido");
    check(!deserialize_nodes({255, 255, 255, 255}, decoded, error),
          "rechazar cantidad de nodos sin registros");

    NodeTable table;
    check(table.add(original, error) && table.add(original, error) && table.snapshot().size() == 1,
          "registro idempotente de un nodo");
    NodeInfo conflict = original;
    conflict.node_name = "otro";
    check(!table.merge({sample_node(2), conflict}, error) && table.snapshot().size() == 1,
          "rechazar colision de NodeId sin modificar la tabla");
    std::set<NodeId> ids;
    bool unique_ids = true;
    for (int i = 0; i < 100; ++i) {
        NodeId id = 0;
        unique_ids &= generate_node_id(id, error) && id != 0 && ids.insert(id).second;
    }
    check(unique_ids, "generar identificadores distintos y no nulos");

    Config config;
    config.node_name = "A";
    config.tcp_port = 0;
    config.worker_count = 1;
    config.request_timeout_ms = 1000;
    NetworkManager a(config);
    const bool a_started = a.start(error);
    check(a_started, "iniciar primer nodo: " + error);
    if (!a_started) return 1;
    check(a.nodes().size() == 1 && a.nodes()[0].id == a.node_id(),
          "registrar al nodo local en su tabla");

    Config b_config = config;
    b_config.node_name = "B";
    b_config.bootstrap_nodes = "localhost:" + std::to_string(a.port());
    NetworkManager b(b_config);
    const bool b_started = b.start(error);
    check(b_started, "incorporar B mediante A: " + error);
    if (!b_started) return 1;
    check(a.node_id() != b.node_id() && wait_for_membership({&a, &b}),
          "A y B conocen a ambos participantes");

    Config c_config = config;
    c_config.node_name = "C";
    c_config.bootstrap_nodes = "127.0.0.1:" + std::to_string(b.port());
    NetworkManager c(c_config);
    const bool c_started = c.start(error);
    check(c_started, "incorporar C mediante B: " + error);
    if (!c_started) return 1;
    check(wait_for_membership({&a, &b, &c}), "propagar NODE_ANNOUNCE al primer nodo");

    Config d_config = b_config;
    d_config.node_name = "D";
    Config e_config = c_config;
    e_config.node_name = "E";
    NetworkManager d(d_config);
    NetworkManager e(e_config);
    bool d_started = false;
    bool e_started = false;
    std::string d_error;
    std::string e_error;
    std::thread join_d([&] { d_started = d.start(d_error); });
    std::thread join_e([&] { e_started = e.start(e_error); });
    join_d.join();
    join_e.join();
    check(d_started, "incorporar D concurrentemente: " + d_error);
    check(e_started, "incorporar E concurrentemente: " + e_error);
    check(wait_for_membership({&a, &b, &c, &d, &e}),
          "converger con uniones simultaneas por distintos bootstraps y un worker");

    Config incompatible = b_config;
    incompatible.page_size = 8192;
    NetworkManager rejected(incompatible);
    check(!rejected.start(error), "rechazar page_size incompatible");
    check(wait_for_membership({&a, &b, &c, &d, &e}),
          "no registrar el nodo incompatible");

    const int fd = connect_to(a.port());
    check(fd != -1, "consultar tabla por TCP");
    if (fd != -1) {
        MessageHeader request{};
        request.magic = DSM_MAGIC;
        request.version = DSM_PROTOCOL_VERSION;
        request.type = static_cast<uint16_t>(MessageType::NODE_LIST_REQUEST);
        request.message_id = 123;
        MessageHeader response{};
        std::vector<uint8_t> payload;
        check(send_message(fd, request, {}, error) &&
              recv_message(fd, response, payload, error) &&
              response.type == static_cast<uint16_t>(MessageType::NODE_LIST_RESPONSE) &&
              response.sender_id == a.node_id() && response.message_id == 123 &&
              deserialize_nodes(payload, decoded, error) && decoded.size() == 5,
              "NODE_LIST_REQUEST devuelve los participantes");
        request.type = static_cast<uint16_t>(MessageType::JOIN_REQUEST);
        check(send_message(fd, request, {0, 0, 0, 0}, error) &&
              recv_message(fd, response, payload, error) &&
              response.type == static_cast<uint16_t>(MessageType::ERROR_RESPONSE) &&
              payload == std::vector<uint8_t>({0, 1}) && a.nodes().size() == 5,
              "rechazar JOIN_REQUEST sin informacion de nodo");
        close(fd);
    }

    Config invalid = config;
    invalid.bootstrap_nodes = "127.0.0.1:0";
    NetworkManager bad_bootstrap(invalid);
    check(!bad_bootstrap.start(error) && bad_bootstrap.port() == 0,
          "rechazar bootstrap invalido y liberar la escucha");

    Config bound_config = b_config;
    bound_config.node_name = "F";
    bound_config.bind_address = "127.0.0.2";
    NetworkManager bound(bound_config);
    const bool bound_started = bound.start(error);
    check(bound_started, "incorporar nodo con direccion de escucha especifica: " + error);
    check(wait_for_membership({&a, &b, &c, &d, &e, &bound}),
          "descubrir nodo que escucha en otra direccion IPv4");
    bool address_found = false;
    for (const auto& node : a.nodes()) {
        if (node.id == bound.node_id()) address_found = node.ip_address == "127.0.0.2";
    }
    check(address_found, "anunciar una direccion donde el nodo realmente escucha");

    if (failures == 0) {
        std::cout << "test_membership: OK\n";
        return 0;
    }
    std::cerr << "test_membership: " << failures << " fallo(s)\n";
    return 1;
}
