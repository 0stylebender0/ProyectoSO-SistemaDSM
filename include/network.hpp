#pragma once

#include "config.hpp"

#include <pthread.h>

#include <queue>
#include <set>
#include <string>
#include <vector>

class NetworkManager {
public:
    explicit NetworkManager(const Config& config);
    ~NetworkManager();

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    bool start(std::string& error);
    void stop();

    uint16_t port() const { return port_; }

private:
    static void* listener_entry(void* context);
    static void* worker_entry(void* context);

    void listen_loop();
    void worker_loop();
    void handle_connection(int fd);

    Config config_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    bool running_ = false;
    bool listener_started_ = false;
    pthread_t listener_{};
    std::vector<pthread_t> workers_;
    std::queue<int> pending_;
    std::set<int> connections_;
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t ready_ = PTHREAD_COND_INITIALIZER;
};
