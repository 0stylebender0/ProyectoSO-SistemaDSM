#include "dsm_node.hpp"
#include "status_report.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace {
    void require(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FALLO: " << message << std::endl;
            std::exit(1);
        }
    }

    struct Participant {
        Config config;
        MemoryManager memory;
        NetworkManager network;
        std::unique_ptr<DSMNode> node;
        RegionId region = 0;

        static Config settings(const std::string& name, uint64_t size, uint16_t bootstrap) {
            Config result;
            result.node_name = name;
            result.memory_size = size;
            result.page_size = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
            result.tcp_port = 0;
            result.bind_address = "127.0.0.1";
            result.worker_count = 1;
            result.request_timeout_ms = 200;
            if (bootstrap) result.bootstrap_nodes = "127.0.0.1:" + std::to_string(bootstrap);
            return result;
        }

        Participant(NodeId id, const std::string& name, uint64_t size, uint16_t bootstrap = 0)
            : config(settings(name, size, bootstrap)), memory(size, config.page_size),
              network(config, id) {
            std::string error;
            require(memory.reserve(error), error);
            node = std::make_unique<DSMNode>(memory, network, id);
            require(network.start(error), error);
            require(node->create_region(config.page_size, region, error), error);
        }

        ~Participant() { network.stop(); }

        volatile uint64_t* access(RegionId id, bool write) {
            RegionInfo info;
            std::string error;
            require(node->pager().region_info(id, info), "region de prueba");
            require(node->resolve_fault(info.first_page, write, error), error);
            return static_cast<volatile uint64_t*>(memory.page_address(info.first_page));
        }
    };

    bool remove_line(std::string& text, const std::string& prefix) {
        const auto start = text.find(prefix);
        if (start == std::string::npos) return false;
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        else ++end;
        text.erase(start, end - start);
        return true;
    }

    bool replace_line(std::string& text, const std::string& prefix, const std::string& replacement) {
        const auto start = text.find(prefix);
        if (start == std::string::npos) return false;
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        text.replace(start, end - start, replacement);
        return true;
    }
}

int main() {
    alarm(20);
    const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    Participant a(111, "A", page * 2 + 1);
    Participant b(222, "B", page * 3, a.network.port());
    Participant c(333, "C", page * 4, a.network.port());
    const auto nodes = a.network.nodes();
    require(nodes.size() == 3, "descubrir tres participantes");
    std::string error;
    *a.access(a.region, true) = 7;
    require(b.node->attach_region_from(111, a.region, error), error);
    require(c.node->attach_region_from(111, a.region, error), error);
    require(*b.access(a.region, false) == 7, "lectura remota");
    *c.access(a.region, true) = 42;
    require(*b.access(a.region, false) == 42 && *a.access(a.region, false) == 42,
            "MSI antes de consultar");

    auto query = [&](const NodeInfo& node, uint8_t flags, std::string& why) {
        return a.network.query_status(node, flags, why);
    };
    const auto report = status_report::collect(nodes, STATUS_FLAG_ALL, query);
    const auto output = status_report::global(report, STATUS_FLAG_ALL);
    require(!report.partial() && report.responded == 3, output);
    require(report.totals.at("memory_configured") == page * 9 + 1, "suma memoria configurada");
    require(report.totals.at("memory_reserved") == page * 10, "suma reservas redondeadas");
    require(report.totals.at("memory_pages_used") == 5 && report.totals.at("memory_pages_free") == 5,
            "paginas usadas y libres incluyen regiones adjuntas");
    require(report.totals.at("pages_total") == 5 && report.totals.at("pages_owned") == 3,
            "copias locales y paginas propias");
    require(report.totals.at("pages_shared") == 3 && report.totals.at("pages_state_shared") == 3,
            "una pagina logica compartida se cuenta en los tres nodos");
    require(report.totals.at("page_faults") == 5 && report.totals.at("page_requests") == 3 &&
            report.totals.at("page_transfers") == 3, "sumar contadores de fallos y transferencias");
    require(output.find("total_memory_reserved=" + std::to_string(page * 10)) != std::string::npos,
            "imprimir totales exactos ademas de conversiones");

    const auto incomplete = status_report::collect(nodes, STATUS_FLAG_MEMORY,
        [&](const NodeInfo& node, uint8_t flags, std::string& why) {
            auto text = query(node, flags, why);
            if (node.id == 222) text += "memory_pages_used=999\n";
            return text;
        });
    require(incomplete.partial() && incomplete.responded == 2 &&
            !incomplete.nodes[1].error.empty(), "no sumar respuestas con datos duplicados");

    const auto missing = status_report::collect(nodes, STATUS_FLAG_MEMORY,
        [&](const NodeInfo& node, uint8_t flags, std::string& why) {
            auto text = query(node, flags, why);
            if (node.id == 111 && !remove_line(text, "memory_pages_free=")) {
                why = "no se pudo preparar el STATUS incompleto";
            }
            return text;
        });
    require(missing.partial() && missing.responded == 2 &&
            missing.nodes[0].error.find("falta") != std::string::npos,
            "un STATUS sin un campo obligatorio no se suma");

    const auto duplicate = status_report::collect(nodes, STATUS_FLAG_MEMORY,
        [&](const NodeInfo& node, uint8_t flags, std::string& why) {
            auto text = query(node, flags, why);
            if (node.id == 111) text += "memory_pages_free=0\n";
            return text;
        });
    require(duplicate.partial() && duplicate.responded == 2 &&
            duplicate.nodes[0].error.find("repetido") != std::string::npos,
            "un campo STATUS repetido no se suma");

    const auto wrong_id = status_report::collect(nodes, STATUS_FLAG_MEMORY,
        [&](const NodeInfo& node, uint8_t flags, std::string& why) {
            auto text = query(node, flags, why);
            if (node.id == 111 && !replace_line(text, "node_id=", "node_id=999999\n")) {
                why = "no se pudo preparar el node_id incorrecto";
            }
            return text;
        });
    require(wrong_id.partial() && wrong_id.responded == 2 &&
            wrong_id.nodes[0].error.find("node_id") != std::string::npos,
            "un STATUS con node_id ajeno no se suma");

    const auto inconsistent = status_report::collect(nodes, STATUS_FLAG_MEMORY,
        [&](const NodeInfo& node, uint8_t flags, std::string& why) {
            auto text = query(node, flags, why);
            if (node.id == 111 && !replace_line(text, "memory_pages_free=", "memory_pages_free=99\n")) {
                why = "no se pudo preparar el STATUS inconsistente";
            }
            return text;
        });
    require(inconsistent.partial() && inconsistent.responded == 2 &&
            inconsistent.nodes[0].error.find("inconsistentes") != std::string::npos,
            "un STATUS con contadores inconsistentes no se suma");

    const auto invalid_number = status_report::collect(nodes, STATUS_FLAG_MEMORY,
        [&](const NodeInfo& node, uint8_t flags, std::string& why) {
            auto text = query(node, flags, why);
            if (node.id == 111 && !replace_line(text, "memory_pages_free=", "memory_pages_free=abc\n")) {
                why = "no se pudo preparar el STATUS no numerico";
            }
            return text;
        });
    require(invalid_number.partial() && invalid_number.responded == 2 &&
            invalid_number.nodes[0].error.find("invalido") != std::string::npos,
            "un STATUS con un valor no numerico no se suma");

    const auto overflow = status_report::collect(nodes, STATUS_FLAG_MEMORY,
        [](const NodeInfo& node, uint8_t, std::string&) {
            std::ostringstream text;
            text << "node_id=" << node.id << "\n"
                 << "memory_configured=" << std::numeric_limits<uint64_t>::max() << "\n"
                 << "memory_reserved=" << std::numeric_limits<uint64_t>::max() << "\n"
                 << "memory_pages_total=1\n"
                 << "memory_pages_used=0\n"
                 << "memory_pages_free=1\n";
            return text.str();
        });
    require(overflow.partial() && overflow.responded == 1 &&
            overflow.nodes[1].error.find("desbordamiento") != std::string::npos,
            "un total global desbordado se descarta");

    error.clear();
    require(a.network.query_status(nodes.front(), 0x80, error).empty() &&
            error.find("banderas") != std::string::npos,
            "rechazar banderas STATUS desconocidas");
    error.clear();
    NodeInfo zero_id = nodes.front();
    zero_id.id = 0;
    require(a.network.query_status(zero_id, STATUS_FLAG_MEMORY, error).empty() &&
            error.find("nodo o banderas") != std::string::npos,
            "rechazar un nodo STATUS invalido");

    c.network.stop();
    const auto partial = status_report::collect(nodes, STATUS_FLAG_MEMORY, query);
    require(partial.partial() && partial.responded == 2 &&
            partial.totals.at("memory_configured") == page * 5 + 1,
            "totales parciales excluyen al nodo no consultado");
    require(status_report::global(partial, STATUS_FLAG_MEMORY).find("No consultado: C") != std::string::npos,
            "identificar al participante que falta");

    const int stalled = socket(AF_INET, SOCK_STREAM, 0);
    require(stalled != -1, "socket sin respuesta");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(stalled, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            listen(stalled, 1) == 0, "listener sin respuestas");
    socklen_t length = sizeof(address);
    require(getsockname(stalled, reinterpret_cast<sockaddr*>(&address), &length) == 0, "puerto sin respuesta");
    NodeInfo silent = nodes.front();
    silent.id = 444;
    silent.tcp_port = ntohs(address.sin_port);
    const auto before = std::chrono::steady_clock::now();
    require(a.network.query_status(silent, STATUS_FLAG_MEMORY, error).empty() && !error.empty(),
            "STATUS_REQUEST con timeout");
    require(std::chrono::steady_clock::now() - before < std::chrono::seconds(1), "timeout acotado");
    close(stalled);
    std::cout << "test_global: OK (totales, copias MSI, datos invalidos, parcial, desbordamiento y timeout)\n";
    return 0;
}
