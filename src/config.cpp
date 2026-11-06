#include "config.hpp"

#include <unistd.h>

#include <cstddef>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace {
    std::string trim(const std::string& text) {
        const char* whitespace = " \t\r\n";
        const std::size_t begin = text.find_first_not_of(whitespace);
        if (begin == std::string::npos) {
            return "";
        }
        const std::size_t end = text.find_last_not_of(whitespace);
        return text.substr(begin, end - begin + 1);
    }

    bool parse_u64(const std::string& text, uint64_t& out) {
        if (text.empty()) {
            return false;
        }
        for (char c : text) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        try {
            std::size_t consumed = 0;
            const unsigned long long value = std::stoull(text, &consumed, 10);
            if (consumed != text.size()) {
                return false;
            }
            out = static_cast<uint64_t>(value);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parse_u32(const std::string& text, uint32_t& out) {
        uint64_t value = 0;
        if (!parse_u64(text, value)) {
            return false;
        }
        if (value > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    }

    bool parse_u16(const std::string& text, uint16_t& out) {
        uint64_t value = 0;
        if (!parse_u64(text, value)) {
            return false;
        }
        if (value > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        out = static_cast<uint16_t>(value);
        return true;
    }
}  // namespace

bool validate_config(const Config& cfg, std::string& error) {
    if (cfg.node_name.empty()) {
        error = "node_name no puede estar vacio";
        return false;
    }

    if (cfg.memory_size == 0) {
        error = "memory_size debe ser mayor que 0";
        return false;
    }

    if (cfg.page_size == 0 || (cfg.page_size & (cfg.page_size - 1)) != 0) {
        error = "page_size debe ser una potencia de 2";
        return false;
    }

    const long os_page = sysconf(_SC_PAGESIZE);
    if (os_page <= 0) {
        error = "no se pudo obtener el tamano de pagina del sistema";
        return false;
    }

    if (cfg.page_size % static_cast<uint32_t>(os_page) != 0) {
        error = "page_size (" + std::to_string(cfg.page_size) +
                ") debe ser multiplo del tamano de pagina del SO (" +
                std::to_string(os_page) + ")";
        return false;
    }

    if (cfg.tcp_port == 0 || cfg.heartbeat_port == 0 || cfg.control_port == 0) {
        error = "los puertos deben estar en el rango 1-65535";
        return false;
    }

    if (cfg.worker_count == 0) {
        error = "worker_count debe ser mayor que 0";
        return false;
    }

    return true;
}

bool load_config(const std::string& path, Config& out, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "no se pudo abrir el archivo de configuracion: " + path;
        return false;
    }

    std::string line;
    std::size_t line_number = 0;

    while (std::getline(file, line)) {
        ++line_number;

        const std::size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.erase(comment);
        }

        line = trim(line);
        if (line.empty() || line.front() == '[') {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            std::cerr << "config: linea " << line_number
                      << " ignorada (no contiene '=')\n";
            continue;
        }

        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));

        auto invalid_value = [&](const char* expected) {
            error = "valor invalido para '" + key + "' en linea " +
                    std::to_string(line_number) + " (se esperaba " +
                    expected + "): '" + value + "'";
            return false;
        };

        if (key == "node_name") {
            out.node_name = value;
        } else if (key == "memory_size") {
            if (!parse_u64(value, out.memory_size)) return invalid_value("entero sin signo");
        } else if (key == "page_size") {
            if (!parse_u32(value, out.page_size)) return invalid_value("entero de 32 bits");
        } else if (key == "bind_address") {
            out.bind_address = value;
        } else if (key == "tcp_port") {
            if (!parse_u16(value, out.tcp_port)) return invalid_value("entero de 16 bits");
        } else if (key == "heartbeat_port") {
            if (!parse_u16(value, out.heartbeat_port)) return invalid_value("entero de 16 bits");
        } else if (key == "control_port") {
            if (!parse_u16(value, out.control_port)) return invalid_value("entero de 16 bits");
        } else if (key == "bootstrap_nodes") {
            out.bootstrap_nodes = value;
        } else if (key == "interval_ms") {
            if (!parse_u32(value, out.heartbeat_interval_ms)) return invalid_value("entero de 32 bits");
        } else if (key == "timeout_ms") {
            if (!parse_u32(value, out.heartbeat_timeout_ms)) return invalid_value("entero de 32 bits");
        } else if (key == "worker_count") {
            if (!parse_u32(value, out.worker_count)) return invalid_value("entero de 32 bits");
        } else if (key == "max_connections") {
            if (!parse_u32(value, out.max_connections)) return invalid_value("entero de 32 bits");
        } else if (key == "max_pending_faults") {
            if (!parse_u32(value, out.max_pending_faults)) return invalid_value("entero de 32 bits");
        } else if (key == "request_timeout_ms") {
            if (!parse_u32(value, out.request_timeout_ms)) return invalid_value("entero de 32 bits");
        } else {
            // Claves desconocidas (incluidas [logging] -> log_level, log_file)
            // se ignoran en silencio.
            std::cout << "Clave desconocida '" << key << "' " << std::endl;
        }
    }

    return validate_config(out, error);
}
