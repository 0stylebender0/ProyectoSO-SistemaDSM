#include "demo_app.hpp"

#include "signal_handler.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    unsigned char pattern_byte(const std::string& node_name, RegionId region_id,
                               uint64_t offset) {
        uint64_t hash = 1469598103934665603ULL;
        auto mix = [&hash](uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ULL;
        };
        for (unsigned char byte : node_name) mix(byte);
        mix(region_id);
        mix(offset);
        return static_cast<unsigned char>((hash >> 32) & 0xFF);
    }

    bool write_pattern(DSMNode& node, const RegionInfo& region,
                       const std::string& owner_name, std::string& error) {
        const uint32_t page_size = node.pager().page_size();
        const uint64_t pages = region.descriptor.size / page_size;
        for (uint64_t page_offset = 0; page_offset < pages; ++page_offset) {
            void* address = node.memory().page_address(region.first_page + page_offset);
            if (address == nullptr) {
                error = "direccion de pagina fuera de la memoria local";
                return false;
            }
            auto* data = static_cast<volatile unsigned char*>(address);
            for (uint32_t i = 0; i < page_size; ++i) {
                data[i] = pattern_byte(owner_name, region.descriptor.region_id,
                                       page_offset * page_size + i);
            }
        }
        return true;
    }

    bool verify_pattern(DSMNode& node, const RegionInfo& region,
                        const std::string& owner_name, std::string& error) {
        const uint32_t page_size = node.pager().page_size();
        const uint64_t pages = region.descriptor.size / page_size;
        for (uint64_t page_offset = 0; page_offset < pages; ++page_offset) {
            void* address = node.memory().page_address(region.first_page + page_offset);
            if (address == nullptr) {
                error = "direccion de pagina fuera de la memoria local";
                return false;
            }
            const auto* data = static_cast<const volatile unsigned char*>(address);
            for (uint32_t i = 0; i < page_size; ++i) {
                const unsigned char expected = pattern_byte(
                    owner_name, region.descriptor.region_id,
                    page_offset * page_size + i);
                if (data[i] != expected) {
                    error = "patron incorrecto en region " +
                        std::to_string(region.descriptor.region_id) +
                        ", byte " + std::to_string(page_offset * page_size + i);
                    return false;
                }
            }
        }
        return true;
    }
}  // namespace

DemoApp::DemoApp(DSMNode& node, std::string app_name, std::string node_name)
    : node_(node), app_name_(std::move(app_name)), node_name_(std::move(node_name)) {}

DemoApp::~DemoApp() {
    stop();
}

bool DemoApp::start(std::string& error) {
    if (app_name_.empty()) {
        error = "el nombre de la aplicacion no puede estar vacio";
        return false;
    }
    if (thread_.joinable()) {
        error = "la aplicacion de prueba ya esta iniciada";
        return false;
    }
    stop_ = false;
    success_ = false;
    try {
        thread_ = std::thread(&DemoApp::run, this);
    } catch (const std::exception& exception) {
        error = std::string("no se pudo iniciar la aplicacion: ") + exception.what();
        return false;
    }
    return true;
}

void DemoApp::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void DemoApp::sleep_checked(unsigned milliseconds) const {
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (!stop_ && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

void DemoApp::run() {
    // El hilo de aplicacion tambien necesita una pila alternativa y SIGUSR1
    // bloqueado para que el FaultWorker pueda resolver sus SIGSEGV.
    FaultThread fault_thread;
    std::string error;
    if (!fault_thread.prepare(error)) {
        std::cerr << "[" << node_name_ << "] app " << app_name_
                  << ": FALLO " << error << "\n" << std::flush;
        success_ = false;
        return;
    }

    // Espera breve a que el descubrimiento inicial incluya a los demas nodos.
    const auto membership_deadline = Clock::now() + std::chrono::milliseconds(2500);
    while (!stop_ && Clock::now() < membership_deadline &&
           node_.network().nodes().size() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    RegionId region_id = 0;
    if (!node_.create_region(node_.pager().page_size(), region_id, error)) {
        std::cerr << "[" << node_name_ << "] app " << app_name_
                  << ": FALLO creando region: " << error << "\n" << std::flush;
        success_ = false;
        return;
    }

    RegionInfo local_region;
    if (!node_.pager().region_info(region_id, local_region)) {
        std::cerr << "[" << node_name_ << "] app " << app_name_
                  << ": FALLO consultando la region local\n" << std::flush;
        success_ = false;
        return;
    }
    if (!write_pattern(node_, local_region, node_name_, error) ||
        !verify_pattern(node_, local_region, node_name_, error)) {
        std::cerr << "[" << node_name_ << "] app " << app_name_
                  << ": FALLO en la region local: " << error << "\n" << std::flush;
        success_ = false;
        return;
    }

    const auto peer_deadline = Clock::now() + std::chrono::milliseconds(8000);
    // Se mantiene un minimo de trabajo para dar tiempo a que terminen de
    // incorporarse los nodos que aun no aparecen en la tabla.
    const auto minimum_work = Clock::now() + std::chrono::milliseconds(2500);
    std::set<RegionId> verified_regions;
    std::set<NodeId> verified_peers;
    std::string last_error;

    while (!stop_ && Clock::now() < peer_deadline) {
        bool all_peers_done = true;

        for (const auto& peer : node_.network().nodes()) {
            if (peer.id == node_.network().node_id()) continue;

            std::vector<RegionDescriptor> regions;
            if (!node_.network().query_regions(peer, regions, last_error)) {
                all_peers_done = false;
                continue;
            }

            bool found_region = false;
            bool peer_done = true;
            for (const auto& descriptor : regions) {
                if (descriptor.owner_id != peer.id ||
                    descriptor.page_size != node_.pager().page_size()) {
                    continue;
                }
                found_region = true;
                if (verified_regions.count(descriptor.region_id) != 0) continue;

                if (!node_.attach_region(descriptor, last_error)) {
                    peer_done = false;
                    continue;
                }
                RegionInfo remote_region;
                if (!node_.pager().region_info(descriptor.region_id, remote_region)) {
                    peer_done = false;
                    last_error = "region remota no disponible localmente";
                    continue;
                }
                if (!verify_pattern(node_, remote_region, peer.node_name, last_error)) {
                    peer_done = false;
                    continue;
                }
                verified_regions.insert(descriptor.region_id);
                verified_peers.insert(peer.id);
            }
            if (!found_region) peer_done = false;
            if (!peer_done) all_peers_done = false;
        }

        if (all_peers_done && Clock::now() >= minimum_work) break;
        sleep_checked(50);
    }

    success_ = true;
    std::cout << "[" << node_name_ << "] app " << app_name_ << ": OK (region "
              << region_id << ", peers=" << verified_peers.size();
    if (!last_error.empty() && verified_peers.size() == 0) {
        std::cout << ", ultimo error: " << last_error;
    }
    std::cout << ")\n" << std::flush;
}
