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

bool query_status(int fd, uint16_t port, uint64_t message_id) {
    MessageHeader response{};
    std::vector<uint8_t> payload;
    std::string error;
    if (!send_message(fd, status_request(message_id), {}, error) ||
        !recv_message(fd, response, payload, error)) return false;

    const std::string expected = "node_name=test_node\ntcp_port=" +
        std::to_string(port) + "\nworker_count=2\n";
    return response.type == static_cast<uint16_t>(MessageType::STATUS_RESPONSE) &&
           response.sender_id != 0 &&
           response.message_id == message_id && response.receiver_id == 42 &&
           std::string(payload.begin(), payload.end()) == expected;
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

    const int fd = connect_to(port);
    check(fd != -1, "abrir conexion TCP");
    if (fd != -1) {
        check(query_status(fd, port, 1), "STATUS_REQUEST y contenido de STATUS_RESPONSE");
        check(query_status(fd, port, 2), "reutilizar la misma conexion");

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
            results[i] = query_status(client, port, 100 + i);
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
