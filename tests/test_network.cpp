#include "network.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <iostream>
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

int connect_to(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return -1;

    const timeval timeout{2, 0};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

MessageHeader status_request(uint64_t message_id) {
    MessageHeader header{};
    header.magic = DSM_MAGIC;
    header.version = DSM_PROTOCOL_VERSION;
    header.type = static_cast<uint16_t>(MessageType::STATUS_REQUEST);
    header.message_id = message_id;
    header.sender_id = 42;
    return header;
}

bool query_status(int fd, uint16_t port, NodeId node_id, uint64_t message_id) {
    MessageHeader response{};
    std::vector<uint8_t> payload;
    std::string error;
    if (!send_message(fd, status_request(message_id), {}, error) ||
        !recv_message(fd, response, payload, error)) return false;

    const std::string expected = "node_name=test_node\nnode_id=" +
        std::to_string(node_id) + "\ntcp_port=" +
        std::to_string(port) + "\nworker_count=2\n";
    return response.type == static_cast<uint16_t>(MessageType::STATUS_RESPONSE) &&
           response.sender_id != 0 &&
           response.message_id == message_id && response.receiver_id == 42 &&
           std::string(payload.begin(), payload.end()) == expected;
}

bool request_error(int fd, MessageHeader request, const std::vector<uint8_t>& payload,
                   ErrorCode code, std::string& error) {
    MessageHeader response{};
    std::vector<uint8_t> reply;
    if (!send_message(fd, request, payload, error) ||
        !recv_message(fd, response, reply, error)) return false;
    const uint16_t value = static_cast<uint16_t>(code);
    return response.type == static_cast<uint16_t>(MessageType::ERROR_RESPONSE) &&
           response.message_id == request.message_id &&
           reply == std::vector<uint8_t>({static_cast<uint8_t>(value >> 8),
                                          static_cast<uint8_t>(value & 0xFF)});
}

bool malformed_requests(int fd, std::string& error) {
    bool ok = true;
    auto expect = [&](MessageType type, uint64_t id, std::vector<uint8_t> payload,
                      ErrorCode code, const char* message) {
        MessageHeader request = status_request(id);
        request.type = static_cast<uint16_t>(type);
        if (!request_error(fd, request, payload, code, error)) {
            std::cerr << "FALLO: " << message << " (" << error << ")\n";
            ok = false;
        }
    };

    expect(MessageType::STATUS_REQUEST, 201, {0, 0}, ErrorCode::INVALID_MESSAGE,
           "STATUS_REQUEST con payload invalido");
    expect(MessageType::STATUS_REQUEST, 202, {0x80}, ErrorCode::INVALID_MESSAGE,
           "STATUS_REQUEST con banderas invalidas");
    expect(MessageType::REGION_INFO_REQUEST, 203, std::vector<uint8_t>(7),
           ErrorCode::INVALID_MESSAGE, "REGION_INFO_REQUEST truncado");
    expect(MessageType::REGION_INFO_REQUEST, 204, std::vector<uint8_t>(8, 0),
           ErrorCode::INVALID_MESSAGE, "REGION_INFO_REQUEST con region cero");
    expect(MessageType::REGION_LIST_REQUEST, 205, {1},
           ErrorCode::INVALID_MESSAGE, "REGION_LIST_REQUEST con payload");
    expect(MessageType::PAGE_REQUEST, 206, std::vector<uint8_t>(33),
           ErrorCode::INVALID_MESSAGE, "PAGE_REQUEST truncado");
    std::vector<uint8_t> page_request(34, 0);
    page_request[24] = 2;
    expect(MessageType::PAGE_REQUEST, 207, page_request,
           ErrorCode::INVALID_MESSAGE, "PAGE_REQUEST con access_type invalido");
    expect(MessageType::PAGE_RELEASE, 208, {0},
           ErrorCode::INVALID_MESSAGE, "PAGE_RELEASE truncado");
    expect(MessageType::INVALIDATE, 209, std::vector<uint8_t>(40),
           ErrorCode::INVALID_MESSAGE, "INVALIDATE truncado");
    expect(MessageType::NODE_ANNOUNCE, 210, std::vector<uint8_t>(4, 0),
           ErrorCode::INVALID_MESSAGE, "NODE_ANNOUNCE vacio");
    expect(MessageType::NODE_LEAVE, 211, {0},
           ErrorCode::INVALID_MESSAGE, "NODE_LEAVE con payload");
    expect(MessageType::JOIN_REQUEST, 212, std::vector<uint8_t>(4, 0),
           ErrorCode::INVALID_MESSAGE, "JOIN_REQUEST vacio");
    expect(MessageType::NODE_LIST_REQUEST, 213, {0},
           ErrorCode::INVALID_MESSAGE, "NODE_LIST_REQUEST con payload");

    MessageHeader unknown = status_request(214);
    unknown.receiver_id = 999999;
    if (!request_error(fd, unknown, {}, ErrorCode::UNKNOWN_NODE, error)) {
        std::cerr << "FALLO: receptor desconocido (" << error << ")\n";
        ok = false;
    }

    expect(MessageType::REGION_INFO_REQUEST, 215, std::vector<uint8_t>(8, 1),
           ErrorCode::PAGE_NOT_FOUND, "REGION_INFO_REQUEST sin manejador");
    expect(MessageType::REGION_LIST_REQUEST, 216, {},
           ErrorCode::PAGE_NOT_FOUND, "REGION_LIST_REQUEST sin manejador");
    expect(MessageType::PAGE_REQUEST, 217, std::vector<uint8_t>(34),
           ErrorCode::PAGE_NOT_FOUND, "PAGE_REQUEST sin manejador");
    return ok;
}

}

int main() {
    Config config;
    config.node_name = "test_node";
    config.bind_address = "127.0.0.1";
    config.tcp_port = 0;
    config.worker_count = 2;
    config.request_timeout_ms = 5000;

    NetworkManager network(config);
    std::string error;
    const bool started = network.start(error);
    check(started, "iniciar listener: " + error);
    if (!started) return 1;
    const uint16_t port = network.port();
    check(port != 0, "puerto asignado por el SO");
    const NodeId node_id = network.node_id();

    const int fd = connect_to(port);
    check(fd != -1, "abrir conexion TCP");
    if (fd != -1) {
        check(query_status(fd, port, node_id, 1), "STATUS_REQUEST y contenido de STATUS_RESPONSE");
        check(query_status(fd, port, node_id, 2), "reutilizar la misma conexion");
        check(malformed_requests(fd, error), "mensajes malformados con ERROR_RESPONSE");
        check(query_status(fd, port, node_id, 3), "conexion util tras mensajes malformados");

        MessageHeader unsupported = status_request(3);
        unsupported.type = 65535;
        MessageHeader response{};
        std::vector<uint8_t> payload;
        check(send_message(fd, unsupported, {}, error) &&
              recv_message(fd, response, payload, error) &&
              response.type == static_cast<uint16_t>(MessageType::ERROR_RESPONSE) &&
              response.message_id == unsupported.message_id &&
              payload == std::vector<uint8_t>({0, 1}), "rechazar tipo no soportado");
        close(fd);
    }

    // El otro worker debe responder aunque este cliente no envie datos.
    const int idle = connect_to(port);
    check(idle != -1, "abrir cliente inactivo");
    std::vector<int> results(6, 0);
    std::vector<std::thread> clients;
    for (std::size_t i = 0; i < results.size(); ++i) {
        clients.emplace_back([&, i] {
            const int client = connect_to(port);
            if (client == -1) return;
            results[i] = query_status(client, port, node_id, 100 + i);
            close(client);
        });
    }
    for (auto& client : clients) client.join();
    for (int result : results) check(result == 1, "atender clientes concurrentes");

    const auto before_stop = std::chrono::steady_clock::now();
    network.stop();
    check(std::chrono::steady_clock::now() - before_stop < std::chrono::seconds(2),
          "apagar sin esperar el timeout del cliente inactivo");
    if (idle != -1) {
        uint8_t byte = 0;
        const ssize_t count = recv(idle, &byte, 1, 0);
        check(count == 0 || (count == -1 && errno == ECONNRESET),
              "cerrar conexiones al apagar");
        close(idle);
    }

    if (failures == 0) {
        std::cout << "test_network: OK\n";
        return 0;
    }
    std::cerr << "test_network: " << failures << " fallo(s)\n";
    return 1;
}
