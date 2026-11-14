#include "config.hpp"
#include "memory_manager.hpp"
#include "network.hpp"

#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

struct CliOptions {
    std::string config_path = "config/dsm.conf";
    std::string bootstrap;
    std::string node_name;
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
        } else if (arg == "--help" || arg == "-h") {
            opt.help = true;
        } else {
            std::cerr << "Argumento desconocido: " << arg << "\n";
            return false;
        }
    }
    return true;
}

static void print_memory(const Config& cfg, const MemoryManager& memory) {
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
}

static void print_nodes(const Config& cfg, const NetworkManager& network) {
    const auto nodes = network.nodes();
    std::size_t name_width = std::string("Nombre").size();
    std::size_t address_width = std::string("Direccion").size();
    for (const auto& node : nodes) {
        const std::string address = node.ip_address + ":" + std::to_string(node.tcp_port);
        if (node.node_name.size() > name_width) name_width = node.node_name.size();
        if (address.size() > address_width) address_width = address.size();
    }

    std::cout << "[" << cfg.node_name << "] nodos conocidos: " << nodes.size() << "\n"
              << std::right << std::setw(20) << "ID" << "  "
              << std::left << std::setw(static_cast<int>(name_width)) << "Nombre" << "  "
              << std::setw(static_cast<int>(address_width)) << "Direccion" << "  "
              << "Memoria\n";
    for (const auto& node : nodes) {
        const std::string address = node.ip_address + ":" + std::to_string(node.tcp_port);
        std::cout << std::right << std::setw(20) << node.id << "  "
                  << std::left << std::setw(static_cast<int>(name_width)) << node.node_name << "  "
                  << std::setw(static_cast<int>(address_width)) << address << "  "
                  << node.memory_size << " bytes / "
                  << node.memory_size / (1024.0 * 1024.0) << " MB\n";
    }
}

void print_info(Config& cfg, MemoryManager& memory, NetworkManager& network, CliOptions& opt) {
    std::cout << "[" << cfg.node_name << "] config\t\t\t" << opt.config_path << "\n";
    std::cout << "[" << cfg.node_name << "] node id\t\t\t" << network.node_id() << "\n";
    std::cout << "[" << cfg.node_name << "] bind\t\t\t" << cfg.bind_address << "\n";
    std::cout << "[" << cfg.node_name << "] tcp\t\t\t" << cfg.tcp_port << "\n";
    std::cout << "[" << cfg.node_name << "] heartbeat\t\t" << cfg.heartbeat_port << "\n";
    std::cout << "[" << cfg.node_name << "] control\t\t\t" << cfg.control_port << "\n";

    print_memory(cfg, memory);

    std::cout << "[" << cfg.node_name << "] bootstrap\t\t"
              << (cfg.bootstrap_nodes.empty() ? "(ninguno)" : cfg.bootstrap_nodes)
              << "\n";

    std::cout << "[" << cfg.node_name << "] escuchando TCP en "
              << cfg.bind_address << ":" << network.port()
              << " con " << cfg.worker_count << " workers (Ctrl+C para salir)\n"
              << std::flush;
}

static bool handle_command(const std::string& line, const Config& cfg,
                           const MemoryManager& memory, const NetworkManager& network) {
    const std::size_t begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return true;
    const std::size_t end = line.find_last_not_of(" \t\r");
    const std::string command = line.substr(begin, end - begin + 1);

    if (command == "nodes") {
        print_nodes(cfg, network);
    } else if (command == "memory") {
        print_memory(cfg, memory);
    } else if (command == "quit") {
        return false;
    } else {
        std::cout << "Comando desconocido: " << command << "\n"
                  << "Comandos: nodes, memory, quit\n";
    }
    std::cout << std::flush;
    return true;
}

static bool run_console(const Config& cfg, const MemoryManager& memory,
                        const NetworkManager& network, const sigset_t& signals) {
    const int signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
    if (signal_fd == -1) {
        std::cerr << "Error configurando consola: " << std::strerror(errno) << "\n";
        return false;
    }
    pollfd inputs[] = {{STDIN_FILENO, POLLIN, 0}, {signal_fd, POLLIN, 0}};
    std::string pending;
    bool running = true;
    bool ok = true;
    std::cout << "[" << cfg.node_name << "] comandos: nodes, memory, quit\n" << std::flush;

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
            if (!pending.empty()) running = handle_command(pending, cfg, memory, network);
            // Sin stdin el nodo sigue atendiendo TCP hasta recibir una senal.
            inputs[0].fd = -1;
            continue;
        }
        pending.append(buffer, static_cast<std::size_t>(count));
        std::size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!handle_command(line, cfg, memory, network)) {
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

    NetworkManager network(cfg);
    if (!network.start(error)) {
        std::cerr << "Error iniciando red: " << error << "\n";
        return 1;
    }

    print_info(cfg, memory, network, opt);

    const bool ok = run_console(cfg, memory, network, signals);
    network.stop();
    std::cout << "[" << cfg.node_name << "] detenido\n";
    return ok ? 0 : 1;
}
