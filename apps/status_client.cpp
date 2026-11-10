#include "protocol.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Uso: " << argv[0] << " <IPv4> <puerto>\n";
        return 1;
    }

    int port = 0;
    try {
        const std::string value = argv[2];
        std::size_t consumed = 0;
        port = std::stoi(value, &consumed);
        if (consumed != value.size() || port < 1 || port > 65535) {
            port = 0;
        }
    } catch (...) {
        port = 0;
    }
    if (port == 0) {
        std::cerr << "Puerto invalido: " << argv[2] << "\n";
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
        std::cerr << "Direccion IPv4 invalida: " << argv[1] << "\n";
        return 1;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        std::cerr << "socket fallo: " << std::strerror(errno) << "\n";
        return 1;
    }
    const timeval timeout{5, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        std::cerr << "Error conectando: " << std::strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    MessageHeader request{};
    request.magic = DSM_MAGIC;
    request.version = DSM_PROTOCOL_VERSION;
    request.type = static_cast<uint16_t>(MessageType::STATUS_REQUEST);
    request.message_id = 1;

    MessageHeader response{};
    std::vector<uint8_t> payload;
    std::string error;
    const bool ok = send_message(fd, request, {}, error) &&
                    recv_message(fd, response, payload, error);
    close(fd);
    if (!ok) {
        std::cerr << "Error consultando estado: " << error << "\n";
        return 1;
    }
    if (response.type != static_cast<uint16_t>(MessageType::STATUS_RESPONSE) ||
        response.message_id != request.message_id) {
        std::cerr << "Respuesta de estado inesperada\n";
        return 1;
    }

    std::cout << "STATUS_RESPONSE\n" << std::string(payload.begin(), payload.end());
    return 0;
}
