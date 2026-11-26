#include "protocol.hpp"
#include "status_report.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
    struct Options {
        bool nodes = false;
        bool history = false;
        bool pages = false;
        bool stats = false;
        bool memory = false;
        bool global = false;
        bool html = false;
        bool open = false;
    };

    void usage(const char* program) {
        std::cerr << "Uso: " << program
                  << " <IPv4> <puerto> [--global] [--nodes] [--memory] [--history] [--pages] [--stats] [--all] [--html] [--open]\n"
                  << "  --html  genera dsm-<global|node>-<IPv4>-<puerto>.html en el directorio actual\n"
                  << "  --open  abre ese archivo con xdg-open tras generarlo\n";
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
            } else if (argument == "--memory") {
                options.memory = true;
            } else if (argument == "--global") {
                options.global = true;
            } else if (argument == "--html") {
                options.html = true;
            } else if (argument == "--open") {
                options.open = true;
                options.html = true;
            } else if (argument == "--all") {
                options.nodes = true;
                options.history = true;
                options.pages = true;
                options.stats = true;
                options.memory = true;
            } else {
                std::cerr << "Opcion desconocida: " << argument << "\n";
                return false;
            }
        }
        return true;
    }

    std::string escape_html(const std::string& text) {
        std::string result;
        result.reserve(text.size());
        for (const char c : text) {
            switch (c) {
                case '&': result += "&amp;"; break;
                case '<': result += "&lt;"; break;
                case '>': result += "&gt;"; break;
                case '"': result += "&quot;"; break;
                case '\'': result += "&#39;"; break;
                default: result += c;
            }
        }
        return result;
    }

    bool write_html(const std::string& path, const std::string& title, const std::string& text) {
        std::ofstream file(path, std::ios::trunc);
        if (!file) {
            std::cerr << "No se pudo crear HTML: " << path << "\n";
            return false;
        }
        file << "<!DOCTYPE html>\n<html lang=\"es\"><head><meta charset=\"utf-8\">\n"
             << "<title>" << escape_html(title) << "</title>\n"
             << "<style>body{font-family:monospace;margin:2em}pre{white-space:pre-wrap}</style>\n"
             << "</head><body><h1>" << escape_html(title) << "</h1><pre>"
             << escape_html(text) << "</pre></body></html>\n";
        return file.good();
    }

    void launch_browser(const std::string& path) {
        const pid_t child = fork();
        if (child == -1) {
            std::cerr << "No se pudo abrir el navegador: " << std::strerror(errno) << "\n";
        } else if (child == 0) {
            execlp("xdg-open", "xdg-open", path.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
    }

    int connect_to(const std::string& host, int port, IODeadline deadline) {
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
        const int flags = fcntl(fd, F_GETFL);
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1 ||
            flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            std::cerr << "Error configurando conexion: " << std::strerror(errno) << "\n";
            close(fd);
            return -1;
        }
        bool connected = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        if (!connected && errno == EINPROGRESS) {
            while (true) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) { errno = ETIMEDOUT; break; }
                pollfd output{fd, POLLOUT, 0};
                const int ready = poll(&output, 1, static_cast<int>(left));
                if (ready < 0 && errno == EINTR) continue;
                int failure = 0;
                socklen_t length = sizeof(failure);
                connected = ready > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &length) == 0 && failure == 0;
                if (ready == 0) errno = ETIMEDOUT;
                else if (failure) errno = failure;
                break;
            }
        }
        if (!connected || fcntl(fd, F_SETFL, flags) == -1) {
            std::cerr << "Error conectando: " << std::strerror(errno) << "\n";
            close(fd);
            return -1;
        }
        return fd;
    }

    bool request(const std::string& host, int port, MessageType type,
                 const std::vector<uint8_t>& payload,
                 MessageType expected, MessageHeader& response,
                 std::vector<uint8_t>& reply,
                 IODeadline deadline = IODeadline::max()) {
        if (deadline == IODeadline::max()) {
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        const int fd = connect_to(host, port, deadline);
        if (fd == -1) return false;

        MessageHeader request_header{};
        request_header.magic = DSM_MAGIC;
        request_header.version = DSM_PROTOCOL_VERSION;
        request_header.type = static_cast<uint16_t>(type);
        request_header.message_id = 1;

        std::string error;
        const bool sent = send_message(fd, request_header, payload, error, deadline) &&
                          recv_message(fd, response, reply, error, deadline);
        close(fd);
        if (!sent) {
            std::cerr << "Error consultando estado: " << error << "\n";
            return false;
        }
        if (response.type != static_cast<uint16_t>(expected) ||
            response.message_id != request_header.message_id || response.sender_id == 0 ||
            response.receiver_id != request_header.sender_id) {
            std::cerr << "Respuesta de estado inesperada\n";
            return false;
        }
        return true;
    }

    bool query_status(const std::string& host, int port, uint8_t flags, std::ostream& out) {
        std::vector<uint8_t> payload;
        if (flags != 0) payload.push_back(flags);
        MessageHeader response{};
        std::vector<uint8_t> reply;
        if (!request(host, port, MessageType::STATUS_REQUEST, payload,
                     MessageType::STATUS_RESPONSE, response, reply)) {
            return false;
        }
        out << "STATUS_RESPONSE\n" << std::string(reply.begin(), reply.end());
        return true;
    }

    bool query_nodes(const std::string& host, int port, std::ostream& out) {
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
        out << "NODE_LIST_RESPONSE (" << nodes.size() << " nodos)\n";
        for (const auto& node : nodes) {
            out << node.id << "\t" << node.node_name << "\t"
                << node.ip_address << ":" << node.tcp_port
                << "\t" << node.memory_size << " bytes / "
                << node.memory_size / (1024.0 * 1024.0) << " MB\n";
        }
        return true;
    }

    int query_global(const std::string& host, int port, uint8_t flags, std::ostream& out) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        MessageHeader response{};
        std::vector<uint8_t> reply;
        if (!request(host, port, MessageType::NODE_LIST_REQUEST, {},
                     MessageType::NODE_LIST_RESPONSE, response, reply, deadline)) return 1;
        std::vector<NodeInfo> nodes;
        std::string error;
        if (!deserialize_nodes(reply, nodes, error) || nodes.empty()) {
            std::cerr << "No se pudo descubrir participantes: " << error << '\n';
            return 1;
        }
        out << "Consultando " << nodes.size() << " participantes...\n" << std::flush;
        const auto report = status_report::collect(nodes, flags,
            [&](const NodeInfo& node, uint8_t requested, std::string& why) -> std::string {
                const std::vector<uint8_t> payload = requested ? std::vector<uint8_t>{requested} : std::vector<uint8_t>{};
                MessageHeader header{};
                std::vector<uint8_t> data;
                if (!request(node.ip_address, node.tcp_port, MessageType::STATUS_REQUEST, payload,
                             MessageType::STATUS_RESPONSE, header, data, deadline)) {
                    why = "no se recibio STATUS_RESPONSE (conexion, timeout o protocolo)";
                    return {};
                }
                if (header.sender_id != node.id) { why = "el nodo cambio de identificador"; return {}; }
                return std::string(data.begin(), data.end());
            });
        out << status_report::global(report, flags);
        return report.partial() ? 2 : 0;
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
    if (options.memory) flags |= STATUS_FLAG_MEMORY;

    std::ostringstream buffered;
    std::ostream& out = options.html ? buffered : std::cout;
    int code = 0;
    if (options.global) {
        if (!options.nodes && flags == 0) flags = STATUS_FLAG_MEMORY | STATUS_FLAG_PAGES | STATUS_FLAG_STATS;
        code = query_global(argv[1], port, flags, out);
    } else {
        // Con --nodes solamente se conserva la consulta estructurada historica.
        const bool needs_status = !options.nodes || flags != 0;
        if ((needs_status && !query_status(argv[1], port, flags, out)) ||
            (options.nodes && !query_nodes(argv[1], port, out))) code = 1;
    }
    if (!options.html) return code;

    // Un archivo por nodo consultado: varios nodos en la misma maquina no se
    // sobrescriben entre si.
    const std::string path = std::string("dsm-") + (options.global ? "global-" : "node-") +
                             argv[1] + "-" + argv[2] + ".html";
    if (!write_html(path, "DSM " + std::string(argv[1]) + ":" + argv[2], buffered.str())) return 1;
    std::cout << "HTML: " << path << "\n";
    if (options.open) launch_browser(path);
    return code;
}
