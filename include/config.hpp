#pragma once

#include <cstdint>
#include <string>

struct Config {
    std::string node_name       = "node1";
    uint64_t    memory_size     = 104857600;
    uint32_t    page_size       = 4096;

    std::string bind_address    = "0.0.0.0";
    uint16_t    tcp_port        = 5000;
    uint16_t    heartbeat_port  = 5001;
    uint16_t    control_port    = 5002;

    std::string bootstrap_nodes;

    uint32_t heartbeat_interval_ms = 1000;
    uint32_t heartbeat_timeout_ms  = 5000;
    uint32_t worker_count          = 4;
    uint32_t max_connections       = 64;
    uint32_t max_pending_faults    = 1024;
    uint32_t request_timeout_ms    = 5000;
};

bool load_config(const std::string& path, Config& out, std::string& error);
bool validate_config(const Config& cfg, std::string& error);
