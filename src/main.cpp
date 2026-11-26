#include "config.hpp"
#include "demo_app.hpp"
#include "dsm_node.hpp"
#include "memory_manager.hpp"
#include "network.hpp"
#include "node.hpp"
#include "signal_handler.hpp"
#include "status_report.hpp"

#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
    std::string join_list(const std::vector<std::string>& values) {
        std::string result;
        for (const auto& value : values) {
            if (!result.empty()) result += ",";
            result += value;
        }
        return result;
    }
}

struct CliOptions {
    std::string config_path = "config/dsm.conf";
    std::string bootstrap;
    std::string node_name;
    std::string app_name;
    int tcp_port = -1;
    bool help = false;
};

static void print_usage(const char* prog) {
    std::cout
        << "Uso: " << prog << " [opciones]\n"
        << "  --config <ruta>            Archivo de configuracion\n"
        << "  --bootstrap <host:puerto>  Nodo semilla\n"
        << "  --name <nombre>            Override de node_name\n"
        << "  --tcp-port <puerto>        Override de tcp_port\n"
        << "  --demo-app                 Aplicacion de patron y verificacion\n"
        << "  --app <nombre>             Nombre de la aplicacion de prueba\n"
        << "  --help                     Muestra esta ayuda\n";
}

static bool parse_args(int argc, char** argv, CliOptions& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto next = [&](std::string& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };

        if (arg == "--config") {
            if (!next(opt.config_path)) return false;
        } else if (arg == "--bootstrap") {
            if (!next(opt.bootstrap)) return false;
        } else if (arg == "--name") {
            if (!next(opt.node_name)) return false;
        } else if (arg == "--tcp-port") {
            std::string value;
            if (!next(value)) return false;
            try {
                opt.tcp_port = std::stoi(value);
            } catch (...) {
                std::cerr << "Puerto invalido: " << value << "\n";
                return false;
            }
        } else if (arg == "--demo-app") {
            opt.app_name = "demo";
        } else if (arg == "--app") {
            if (!next(opt.app_name) || opt.app_name.empty()) {
                std::cerr << "--app requiere un nombre\n";
                return false;
            }
        } else if (arg == "--help" || arg == "-h") {
            opt.help = true;
        } else {
            std::cerr << "Argumento desconocido: " << arg << "\n";
            return false;
        }
    }
    return true;
}

static void print_memory(const Config& cfg, const MemoryManager& memory, const Pager& pager) {
    std::cout << "[" << cfg.node_name << "] memoria configurada\t"
              << cfg.memory_size << " bytes / "
              << (cfg.memory_size) / (1024.0 * 1024.0) << " MB\n";

    std::cout << "[" << cfg.node_name << "] memoria reservada\t"
              << memory.size() << " bytes / "
              << (memory.size()) / (1024.0 * 1024.0) << " MB\n";

    std::cout << "[" << cfg.node_name << "] paginas\t\t\t" << memory.page_count()
              << " de " << cfg.page_size << " bytes / "
              << (cfg.page_size) / (1024.0) << " KB\n";

    std::cout << "[" << cfg.node_name << "] mmap base\t\t" << memory.base()
              << " (PROT_NONE)\n";
    uint64_t used = 0;
    for (const auto& region : pager.region_snapshot()) used += region.page_count;
    std::cout << "[" << cfg.node_name << "] paginas usadas/libres\t"
              << used << " / " << memory.page_count() - used << "\n";
}

static void print_pages(const Config& cfg, const Pager& pager) {
    const auto regions = pager.region_snapshot();
    auto pages = pager.page_snapshot();
    std::sort(pages.begin(), pages.end(),
              [](const PageInfo& a, const PageInfo& b) { return a.page_id < b.page_id; });

    std::size_t invalid = 0;
    std::size_t shared = 0;
    std::size_t modified = 0;
    for (const auto& page : pages) {
        switch (page.state) {
            case MSIState::INVALID:  ++invalid;  break;
            case MSIState::SHARED:   ++shared;   break;
            case MSIState::MODIFIED: ++modified; break;
        }
    }

    std::cout << "[" << cfg.node_name << "] regiones\t\t" << regions.size()
              << ", paginas locales: " << pages.size() << "\n";
    std::cout << "[" << cfg.node_name << "] estados MSI\t\tI=" << invalid
              << " S=" << shared << " M=" << modified << "\n";

    if (pages.empty()) return;

    std::cout << std::right << std::setw(10) << "PageId" << "  "
              << std::setw(10) << "RegionId" << "  "
              << std::setw(20) << "Owner" << "  "
              << std::left << std::setw(10) << "Estado" << "  "
              << std::setw(9) << "Presente" << "  "
              << "Version\n";
    for (const auto& page : pages) {
        std::cout << std::right << std::setw(10) << page.page_id << "  "
                  << std::setw(10) << page.region_id << "  "
                  << std::setw(20) << page.owner_id << "  "
                  << std::left << std::setw(10) << msi_state_name(page.state) << "  "
                  << std::setw(9) << (page.locally_present ? "si" : "no") << "  "
                  << page.version << "\n";
    }
}

static void print_nodes(const Config& cfg, const NetworkManager& network,
                        bool active_only = false) {
    const auto nodes = active_only ? network.active_nodes() : network.nodes();
    std::map<NodeId, HeartbeatSnapshot> heartbeats;
    for (const auto& item : network.availability_snapshot()) {
        heartbeats[item.node_id] = item;
    }

    std::size_t name_width = std::string("Nombre").size();
    std::size_t address_width = std::string("Direccion").size();
    std::size_t state_width = std::string("Estado").size();
    std::size_t last_width = std::string("Ultimo").size();
    for (const auto& node : nodes) {
        const std::string address = node.ip_address + ":" + std::to_string(node.tcp_port);
        const AvailabilityState state = node.id == network.node_id()
            ? AvailabilityState::ACTIVE : network.availability(node.id);
        const std::string last = node.id == network.node_id() ? "local" : "sin heartbeat";
        if (node.node_name.size() > name_width) name_width = node.node_name.size();
        if (address.size() > address_width) address_width = address.size();
        if (std::strlen(availability_state_name(state)) > state_width) {
            state_width = std::strlen(availability_state_name(state));
        }
        if (last.size() > last_width) last_width = last.size();
    }
    // Las edades se calculan antes de imprimir para mantener alineadas las filas.
    std::map<NodeId, std::string> last_seen;
    for (const auto& node : nodes) {
        if (node.id == network.node_id()) {
            last_seen[node.id] = "local";
        } else {
            const auto found = heartbeats.find(node.id);
            if (found != heartbeats.end() && found->second.has_heartbeat) {
                last_seen[node.id] = "hace " + std::to_string(found->second.age_ms) + " ms";
                last_width = std::max(last_width, last_seen[node.id].size());
            } else {
                last_seen[node.id] = "sin heartbeat";
            }
        }
    }

    std::cout << "[" << cfg.node_name << "] "
              << (active_only ? "nodos activos: " : "nodos conocidos: ")
              << nodes.size() << "\n"
              << std::right << std::setw(20) << "ID" << "  "
              << std::left << std::setw(static_cast<int>(name_width)) << "Nombre" << "  "
              << std::setw(static_cast<int>(address_width)) << "Direccion" << "  "
              << std::setw(static_cast<int>(state_width)) << "Estado" << "  "
              << std::setw(static_cast<int>(last_width)) << "Ultimo" << "  "
              << "Memoria\n";
    for (const auto& node : nodes) {
        const std::string address = node.ip_address + ":" + std::to_string(node.tcp_port);
        const AvailabilityState state = node.id == network.node_id()
            ? AvailabilityState::ACTIVE : network.availability(node.id);
        std::cout << std::right << std::setw(20) << node.id << "  "
                  << std::left << std::setw(static_cast<int>(name_width)) << node.node_name << "  "
                  << std::setw(static_cast<int>(address_width)) << address << "  "
                  << std::setw(static_cast<int>(state_width)) << availability_state_name(state) << "  "
                  << std::setw(static_cast<int>(last_width)) << last_seen[node.id] << "  "
                  << node.memory_size << " bytes / "
                  << node.memory_size / (1024.0 * 1024.0) << " MB\n";
    }
}

static void print_history(const Config& cfg, const NetworkManager& network) {
    std::cout << "[" << cfg.node_name << "] historial\n"
              << status_report::history(network.history()) << std::flush;
}

static void print_statistics(const Config& cfg, const Pager& pager,
                             const NetworkManager& network) {
    std::cout << "[" << cfg.node_name << "] estadisticas\n"
              << status_report::statistics(pager, network) << std::flush;
}

void print_info(const Config& cfg, const MemoryManager& memory, const NetworkManager& network,
                const Pager& pager, const CliOptions& opt) {
    std::cout << "[" << cfg.node_name << "] config\t\t\t" << opt.config_path << "\n";
    std::cout << "[" << cfg.node_name << "] node id\t\t\t" << network.node_id() << "\n";
    std::cout << "[" << cfg.node_name << "] bind\t\t\t" << cfg.bind_address << "\n";
    std::cout << "[" << cfg.node_name << "] tcp\t\t\t" << cfg.tcp_port << "\n";
    std::cout << "[" << cfg.node_name << "] heartbeat\t\t" << network.heartbeat_port() << "\n";

    print_memory(cfg, memory, pager);
    print_pages(cfg, pager);

    std::cout << "[" << cfg.node_name << "] bootstrap\t\t"
              << (cfg.bootstrap_nodes.empty() ? "(ninguno)" : cfg.bootstrap_nodes)
              << "\n";
    std::cout << "[" << cfg.node_name << "] participantes\t\t"
              << (cfg.node_addresses.empty() ? "(ninguno)" : join_list(cfg.node_addresses))
              << "\n";

    std::cout << "[" << cfg.node_name << "] escuchando TCP en "
              << cfg.bind_address << ":" << network.port()
              << " con " << cfg.worker_count << " workers\n"
              << std::flush;
}

static bool handle_command(const std::string& line, const Config& cfg,
                           const MemoryManager& memory, NetworkManager& network,
                           const Pager& pager, const CliOptions& opt) {
    const std::size_t begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return true;
    const std::size_t end = line.find_last_not_of(" \t\r");
    const std::string command = line.substr(begin, end - begin + 1);
    std::istringstream input(command);
    std::string verb, option, extra;
    input >> verb >> option >> extra;

    if (extra.empty() && verb == "nodes" && option == "--active") {
        print_nodes(cfg, network, true);
    } else if (extra.empty() &&
               ((verb == "memory" || verb == "pages" || verb == "stats") &&
                option == "--global")) {
        const uint8_t flags = verb == "memory" ? STATUS_FLAG_MEMORY :
            verb == "pages" ? STATUS_FLAG_PAGES : STATUS_FLAG_STATS;
        std::cout << "[" << cfg.node_name << "] consultando participantes...\n" << std::flush;
        const auto report = status_report::collect(network.nodes(), flags,
            [&](const NodeInfo& peer, uint8_t requested, std::string& error) {
                return network.query_status(peer, requested, error);
            });
        std::cout << status_report::global(report, flags);
    } else if (command == "nodes") {
        print_nodes(cfg, network);
    } else if (command == "memory") {
        print_memory(cfg, memory, pager);
    } else if (command == "pages") {
        print_pages(cfg, pager);
    } else if (command == "history") {
        print_history(cfg, network);
    } else if (command == "stats") {
        print_statistics(cfg, pager, network);
    } else if (command == "quit") {
        return false;
    } else if (command == "info") {
        print_info(cfg, memory, network, pager, opt);
    } else {
        std::cout << "Comando desconocido: " << command << "\n"
                  << "Comandos: nodes [--active], memory [--global], pages [--global], "
                     "stats [--global], history, info, quit\n";
    }
    std::cout << std::flush;
    return true;
}

static bool run_console(const Config& cfg, const MemoryManager& memory,
                        NetworkManager& network, const Pager& pager,
                        const sigset_t& signals, const CliOptions& opt) {
    const int signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
    if (signal_fd == -1) {
        std::cerr << "Error configurando consola: " << std::strerror(errno) << "\n";
        return false;
    }
    pollfd inputs[] = {{STDIN_FILENO, POLLIN, 0}, {signal_fd, POLLIN, 0}};
    std::string pending;
    bool running = true;
    bool ok = true;
    std::cout << "[" << cfg.node_name << "] comandos: nodes [--active], memory [--global], "
                 "pages [--global], stats [--global], history, info, quit\n" << std::flush;

    while (running) {
        if (poll(inputs, 2, -1) == -1) {
            if (errno == EINTR) continue;
            std::cerr << "Error esperando entrada: " << std::strerror(errno) << "\n";
            ok = false;
            break;
        }
        if (inputs[1].revents & POLLIN) {
            signalfd_siginfo info{};
            if (read(signal_fd, &info, sizeof(info)) != sizeof(info)) {
                std::cerr << "Error leyendo senal: " << std::strerror(errno) << "\n";
                ok = false;
            }
            break;
        }
        if (inputs[0].revents & POLLNVAL) {
            inputs[0].fd = -1;
            continue;
        }
        if (!(inputs[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;

        // read no espera una linea completa, asi Ctrl+C funciona con entrada parcial.
        char buffer[1024];
        const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) continue;
            std::cerr << "Error leyendo comando: " << std::strerror(errno) << "\n";
            ok = false;
            break;
        }
        if (count == 0) {
            if (!pending.empty()) running = handle_command(pending, cfg, memory, network, pager, opt);
            // Sin stdin el nodo sigue atendiendo TCP hasta recibir una senal.
            inputs[0].fd = -1;
            continue;
        }
        pending.append(buffer, static_cast<std::size_t>(count));
        std::size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!handle_command(line, cfg, memory, network, pager, opt)) {
                running = false;
                break;
            }
        }
    }
    close(signal_fd);
    return ok;
}

int main(int argc, char** argv) {
    CliOptions opt;

    if (!parse_args(argc, argv, opt)) {
        print_usage(argv[0]);
        return 1;
    }
    if (opt.help) {
        print_usage(argv[0]);
        return 0;
    }

    Config cfg;
    std::string error;

    if (!load_config(opt.config_path, cfg, error)) {
        std::cerr << "Error de configuracion: " << error << "\n";
        return 1;
    }

    if (!opt.node_name.empty()) {
        cfg.node_name = opt.node_name;
    }

    if (opt.tcp_port >= 0) {
        if (opt.tcp_port < 1 || opt.tcp_port > 65535) {
            std::cerr << "Puerto TCP fuera de rango: " << opt.tcp_port << "\n";
            return 1;
        }
        cfg.tcp_port = static_cast<uint16_t>(opt.tcp_port);
    }

    if (!opt.bootstrap.empty()) {
        cfg.bootstrap_nodes = opt.bootstrap;
    }

    if (!validate_config(cfg, error)) {
        std::cerr << "Configuracion invalida: " << error << "\n";
        return 1;
    }

    MemoryManager memory(cfg.memory_size, cfg.page_size);
    if (!memory.reserve(error)) {
        std::cerr << "Error reservando memoria: " << error << "\n";
        return 1;
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP);

    // Los hilos heredan el bloqueo; main recibe las senales por signalfd.
    const int mask_result = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (mask_result != 0) {
        std::cerr << "Error configurando senales: " << std::strerror(mask_result) << "\n";
        return 1;
    }

    NodeId local_id = 0;
    if (!generate_node_id(local_id, error)) {
        std::cerr << "Error generando NodeId: " << error << "\n";
        return 1;
    }

    NetworkManager network(cfg, local_id);
    DSMNode node(memory, network, local_id);

    SignalHandler faults(memory,
        [&node](PageId page_id, bool write, std::string& fault_error) {
            return node.pager().resolve_fault(page_id, write, fault_error);
        });
    if (!faults.start(error)) {
        std::cerr << "Error iniciando manejo de fallos: " << error << "\n";
        return 1;
    }

    if (!network.start(error)) {
        std::cerr << "Error iniciando red: " << error << "\n";
        return 1;
    }

    std::unique_ptr<DemoApp> app;
    if (!opt.app_name.empty()) {
        app = std::make_unique<DemoApp>(node, opt.app_name, cfg.node_name);
        if (!app->start(error)) {
            std::cerr << "Error iniciando aplicacion: " << error << "\n";
            network.stop();
            return 1;
        }
    }

    const bool ok = run_console(cfg, memory, network, node.pager(), signals, opt);
    if (app) app->stop();
    network.stop();
    std::cout << "[" << cfg.node_name << "] detenido\n";
    return (ok && (app == nullptr || app->success())) ? 0 : 1;
}
