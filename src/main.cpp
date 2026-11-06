#include "config.hpp"

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

    std::cout << "[" << cfg.node_name << "] config=" << opt.config_path << "\n";
    std::cout << "[" << cfg.node_name << "] bind=" << cfg.bind_address << "\n";
    std::cout << "[" << cfg.node_name << "] tcp=" << cfg.tcp_port
              << " heartbeat=" << cfg.heartbeat_port
              << " control=" << cfg.control_port << "\n";
    std::cout << "[" << cfg.node_name << "] memoria=" << cfg.memory_size
              << " bytes, page_size=" << cfg.page_size << "\n";
    std::cout << "[" << cfg.node_name << "] workers=" << cfg.worker_count
              << " max_connections=" << cfg.max_connections << "\n";
    std::cout << "[" << cfg.node_name << "] bootstrap="
              << (cfg.bootstrap_nodes.empty() ? "(ninguno)" : cfg.bootstrap_nodes)
              << "\n";

    return 0;
}
