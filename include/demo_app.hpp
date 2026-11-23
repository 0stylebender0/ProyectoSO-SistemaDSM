#pragma once

#include "dsm_node.hpp"

#include <atomic>
#include <string>
#include <thread>

// Aplicacion de validacion que se ejecuta dentro del nodo. Crea una region
// propia, escribe un patron y luego descubre y verifica las regiones de los
// demas participantes. Se activa con --demo-app o --app <nombre>.
class DemoApp {
public:
    DemoApp(DSMNode& node, std::string app_name, std::string node_name);
    ~DemoApp();

    DemoApp(const DemoApp&) = delete;
    DemoApp& operator=(const DemoApp&) = delete;

    bool start(std::string& error);
    void stop();

    bool success() const { return success_.load(); }

private:
    void run();
    void sleep_checked(unsigned milliseconds) const;

    DSMNode& node_;
    std::string app_name_;
    std::string node_name_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> success_{false};
};
