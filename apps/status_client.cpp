#include "protocol.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
    struct Options {
        bool nodes = false;
        bool history = false;
        bool pages = false;
        bool stats = false;
    };

    void usage(const char* program) {
        std::cerr << "Uso: " << program
                  << " <IPv4> <puerto> [--nodes] [--history] [--pages] [--stats] [--all]\n";
    }

    bool parse_options(int argc, char** argv, Options& options) {
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--nodes") {
                options.nodes = true;
            } else if (argument == "--history") {
                options.history = true;
            } else if (argument == "--pages") {
                options.pages = true;
            } else if (argument == "--stats") {
                options.stats = true;
            } else if (argument == "--all") {
                options.nodes = true;
                options.history = true;
                options.pages = true;
                options.stats = true;
            } else {
                std::cerr << "Opcion desconocida: " << argument << "\n";
                return false;
            }
        }
        return true;
    }

    int connect_to(const std::string& host, int port) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            std::cerr << "Direccion IPv4 invalida: " << host << "\n";
            return -1;
        }

        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
            std::cerr << "socket fallo: " << std::strerror(errno) << "\n";
            return -1;
        }
        const timeval timeout{5, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1 ||
            connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
            std::cerr << "Error conectando: " << std::strerror(errno) << "\n";
            close(fd);
            return -1;
        }
        return fd;
    }

    bool request(const std::string& host, int port, MessageType type,
                 const std::vector<uint8_t>& payload,
                 MessageType expected, MessageHeader& response,
                 std::vector<uint8_t>& reply) {
        const int fd = connect_to(host, port);
        if (fd == -1) return false;

        MessageHeader request_header{};
        request_header.magic = DSM_MAGIC;
        request_header.version = DSM_PROTOCOL_VERSION;
        request_header.type = static_cast<uint16_t>(type);
        request_header.message_id = 1;

        std::string error;
        const bool sent = send_message(fd, request_header, payload, error) &&
                          recv_message(fd, response, reply, error);
        close(fd);
        if (!sent) {
            std::cerr << "Error consultando estado: " << error << "\n";
            return false;
        }
        if (response.type != static_cast<uint16_t>(expected) ||
            response.message_id != request_header.message_id) {
            std::cerr << "Respuesta de estado inesperada\n";
            return false;
        }
        return true;
    }

    bool query_status(const std::string& host, int port, uint8_t flags) {
        std::vector<uint8_t> payload;
        if (flags != 0) payload.push_back(flags);
        MessageHeader response{};
        std::vector<uint8_t> reply;
        if (!request(host, port, MessageType::STATUS_REQUEST, payload,
                     MessageType::STATUS_RESPONSE, response, reply)) {
            return false;
        }
        std::cout << "STATUS_RESPONSE\n" << std::string(reply.begin(), reply.end());
        return true;
    }

    bool query_nodes(const std::string& host, int port) {
        MessageHeader response{};
        std::vector<uint8_t> reply;
        if (!request(host, port, MessageType::NODE_LIST_REQUEST, {},
                     MessageType::NODE_LIST_RESPONSE, response, reply)) {
            return false;
        }
        std::vector<NodeInfo> nodes;
        std::string error;
        if (!deserialize_nodes(reply, nodes, error)) {
            std::cerr << "Lista de nodos invalida: " << error << "\n";
            return false;
        }
        std::cout << "NODE_LIST_RESPONSE (" << nodes.size() << " nodos)\n";
        for (const auto& node : nodes) {
            std::cout << node.id << "\t" << node.node_name << "\t"
                      << node.ip_address << ":" << node.tcp_port
                      << "\t" << node.memory_size << " bytes / "
                      << node.memory_size / (1024.0 * 1024.0) << " MB\n";
        }
        return true;
    }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    Options options;
    if (!parse_options(argc, argv, options)) {
        usage(argv[0]);
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

    // Se valida la IPv4 antes de intentar cualquier consulta.
    in_addr address{};
    if (inet_pton(AF_INET, argv[1], &address) != 1) {
        std::cerr << "Direccion IPv4 invalida: " << argv[1] << "\n";
        return 1;
    }

    uint8_t flags = 0;
    if (options.history) flags |= STATUS_FLAG_HISTORY;
    if (options.pages) flags |= STATUS_FLAG_PAGES;
    if (options.stats) flags |= STATUS_FLAG_STATS;

    // Con --nodes solamente se conserva la consulta estructurada historica.
    const bool needs_status = !options.nodes || flags != 0;
    if (needs_status && !query_status(argv[1], port, flags)) return 1;
    if (options.nodes && !query_nodes(argv[1], port)) return 1;
    return 0;
}
