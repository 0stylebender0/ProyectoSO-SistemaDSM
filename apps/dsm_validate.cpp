#include "dsm.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
    constexpr uint64_t FNV_PRIME = 1099511628211ULL;

    struct Options {
        std::string config;
        uint32_t pages = 4;
        uint32_t threads = 2;
        uint32_t regions = 1;
        uint32_t expect_nodes = 1;
        uint32_t expect_peers = 0;
        uint32_t membership_timeout_ms = 15000;
        uint32_t verify_timeout_ms = 20000;
        uint32_t hold_ms = 0;
        uint64_t seed = 20261126;
    };

    struct PeerSummary {
        DSMNodeId id = 0;
        std::string name;
        std::size_t regions = 0;
        std::size_t bytes = 0;
    };

    void usage(const char* program) {
        std::cout
            << "Uso: " << program << " <config> [opciones]\n"
            << "  --pages <n>                 Paginas por region propia (default 4)\n"
            << "  --threads <n>               Hilos de escritura y verificacion (default 2)\n"
            << "  --regions <n>               Regiones propias; 0 solo verifica (default 1)\n"
            << "  --expect-nodes <n>          Participantes totales esperados (default 1)\n"
            << "  --expect-peers <n>          Pares con regiones a verificar (default 0)\n"
            << "  --membership-timeout-ms <n> Espera maxima por participantes (default 15000)\n"
            << "  --verify-timeout-ms <n>     Espera maxima de verificacion (default 20000)\n"
            << "  --hold-ms <n>               Permanecer activo tras exito (default 0)\n"
            << "  --seed <n>                  Semilla del patron determinista (default 20261126)\n"
            << "  --help                      Muestra esta ayuda\n";
    }

    bool parse_u64(const std::string& text, uint64_t minimum, uint64_t maximum,
                   uint64_t& out) {
        if (text.empty()) return false;
        for (char value : text) {
            if (value < '0' || value > '9') return false;
        }
        try {
            std::size_t consumed = 0;
            const unsigned long long parsed = std::stoull(text, &consumed, 10);
            if (consumed != text.size() || parsed < minimum || parsed > maximum) return false;
            out = static_cast<uint64_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parse_u32(const std::string& text, uint32_t minimum, uint32_t maximum,
                   uint32_t& out) {
        uint64_t value = 0;
        if (!parse_u64(text, minimum, maximum, value)) return false;
        out = static_cast<uint32_t>(value);
        return true;
    }

    bool parse_args(int argc, char** argv, Options& opt) {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help" || argument == "-h") {
                usage(argv[0]);
                std::exit(0);
            }
            if (argument.empty() || argument.front() != '-') {
                if (!opt.config.empty()) {
                    std::cerr << "Solo se admite un archivo de configuracion\n";
                    return false;
                }
                opt.config = argument;
                continue;
            }
            if (i + 1 >= argc) {
                std::cerr << "Falta el valor de " << argument << "\n";
                return false;
            }
            const std::string value = argv[++i];
            bool valid = true;
            if (argument == "--pages") {
                valid = parse_u32(value, 1, 1024, opt.pages);
            } else if (argument == "--threads") {
                valid = parse_u32(value, 1, 64, opt.threads);
            } else if (argument == "--regions") {
                valid = parse_u32(value, 0, 64, opt.regions);
            } else if (argument == "--expect-nodes") {
                valid = parse_u32(value, 1, 1024, opt.expect_nodes);
            } else if (argument == "--expect-peers") {
                valid = parse_u32(value, 0, 1024, opt.expect_peers);
            } else if (argument == "--membership-timeout-ms") {
                valid = parse_u32(value, 1, 600000, opt.membership_timeout_ms);
            } else if (argument == "--verify-timeout-ms") {
                valid = parse_u32(value, 1, 600000, opt.verify_timeout_ms);
            } else if (argument == "--hold-ms") {
                valid = parse_u32(value, 0, 600000, opt.hold_ms);
            } else if (argument == "--seed") {
                valid = parse_u64(value, 0, std::numeric_limits<uint64_t>::max(), opt.seed);
            } else {
                std::cerr << "Opcion desconocida: " << argument << "\n";
                return false;
            }
            if (!valid) {
                std::cerr << "Valor invalido para " << argument << ": " << value << "\n";
                return false;
            }
        }
        if (opt.config.empty()) {
            std::cerr << "Falta el archivo de configuracion\n";
            return false;
        }
        return true;
    }

    void mix(uint64_t& hash, uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFFULL;
            hash *= FNV_PRIME;
        }
    }

    unsigned char pattern_byte(uint64_t seed, DSMRegionId region_id, uint64_t offset) {
        uint64_t hash = FNV_OFFSET;
        mix(hash, seed);
        mix(hash, region_id);
        mix(hash, offset);
        return static_cast<unsigned char>((hash >> 32) & 0xFF);
    }

    uint64_t page_checksum(const unsigned char* data, uint32_t size) {
        uint64_t hash = FNV_OFFSET;
        for (uint32_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= FNV_PRIME;
        }
        return hash;
    }

    uint64_t expected_page_checksum(uint64_t seed, DSMRegionId region_id,
                                    uint64_t page_index, uint32_t page_size) {
        uint64_t hash = FNV_OFFSET;
        const uint64_t base = page_index * page_size;
        for (uint32_t i = 0; i < page_size; ++i) {
            hash ^= pattern_byte(seed, region_id, base + i);
            hash *= FNV_PRIME;
        }
        return hash;
    }

    uint64_t fold_checksums(const std::vector<uint64_t>& page_hashes) {
        uint64_t hash = FNV_OFFSET;
        for (uint64_t page : page_hashes) {
            hash ^= page;
            hash *= FNV_PRIME;
        }
        return hash;
    }

    uint64_t expected_region_checksum(uint64_t seed, DSMRegionId region_id,
                                      uint64_t page_count, uint32_t page_size) {
        std::vector<uint64_t> pages;
        pages.reserve(static_cast<std::size_t>(page_count));
        for (uint64_t page = 0; page < page_count; ++page) {
            pages.push_back(expected_page_checksum(seed, region_id, page, page_size));
        }
        return fold_checksums(pages);
    }

    std::string hex64(uint64_t value) {
        std::ostringstream out;
        out << std::hex << std::setw(16) << std::setfill('0') << value;
        return out.str();
    }

    void print_checksum(const char* source, DSMRegionId region_id, DSMNodeId owner_id,
                        uint64_t pages, uint64_t bytes, uint64_t checksum,
                        const std::string& peer = {}) {
        std::cout << "[checksum] source=" << source
                  << " region=" << region_id
                  << " owner=" << owner_id
                  << " pages=" << pages
                  << " bytes=" << bytes
                  << " value=0x" << hex64(checksum);
        if (!peer.empty()) std::cout << " peer=" << peer;
        std::cout << '\n';
    }

    bool write_local_regions(std::vector<DSMRegion>& regions, uint32_t threads,
                             uint64_t seed, uint32_t page_size, std::string& error) {
        if (regions.empty()) return true;

        std::atomic<bool> failed{false};
        std::mutex error_mutex;
        std::string first_error;
        auto fail = [&](const std::string& message) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (first_error.empty()) first_error = message;
            failed = true;
        };

        std::vector<std::thread> workers;
        try {
            for (uint32_t thread_id = 0; thread_id < threads; ++thread_id) {
                workers.emplace_back([&, thread_id] {
                    DSMThread guard;
                    if (!guard.valid()) {
                        fail("no se pudo preparar DSMThread: " + DSM::last_error_message());
                        return;
                    }
                    std::vector<unsigned char> bytes(page_size);
                    for (auto& region : regions) {
                        if (failed.load()) return;
                        const uint64_t pages = region.get_size() / page_size;
                        for (uint64_t page = thread_id; page < pages; page += threads) {
                            const uint64_t base_offset = page * page_size;
                            for (uint32_t i = 0; i < page_size; ++i) {
                                bytes[i] = pattern_byte(seed, region.get_id(), base_offset + i);
                            }
                            if (!region.write(static_cast<std::size_t>(base_offset),
                                              bytes.data(), page_size)) {
                                fail("no se pudo escribir la region " +
                                     std::to_string(region.get_id()) + ": " +
                                     DSM::last_error_message());
                                return;
                            }
                        }
                    }
                });
            }
        } catch (const std::exception& exception) {
            fail(std::string("no se pudo crear un hilo de escritura: ") + exception.what());
        }
        for (auto& worker : workers) worker.join();
        if (failed) {
            error = first_error;
            return false;
        }
        return true;
    }

    bool verify_regions(const std::vector<DSMRegion>& regions, uint32_t threads,
                        uint64_t seed, uint32_t page_size, Clock::time_point deadline,
                        std::vector<uint64_t>& checksums, std::string& error) {
        checksums.assign(regions.size(), 0);
        if (regions.empty()) return true;

        std::vector<std::vector<uint64_t>> page_hashes(regions.size());
        std::vector<std::vector<unsigned char>> page_ok(regions.size());
        for (std::size_t index = 0; index < regions.size(); ++index) {
            const auto count = static_cast<std::size_t>(regions[index].get_size() / page_size);
            page_hashes[index].assign(count, 0);
            page_ok[index].assign(count, 0);
        }

        std::atomic<bool> failed{false};
        std::mutex error_mutex;
        std::string first_error;
        auto fail = [&](const std::string& message) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (first_error.empty()) first_error = message;
            failed = true;
        };

        std::vector<std::thread> workers;
        try {
            for (uint32_t thread_id = 0; thread_id < threads; ++thread_id) {
                workers.emplace_back([&, thread_id] {
                    DSMThread guard;
                    if (!guard.valid()) {
                        fail("no se pudo preparar DSMThread: " + DSM::last_error_message());
                        return;
                    }
                    std::vector<unsigned char> bytes(page_size);
                    for (std::size_t index = 0; index < regions.size(); ++index) {
                        if (failed.load()) return;
                        const uint64_t pages = regions[index].get_size() / page_size;
                        for (uint64_t page = thread_id; page < pages; page += threads) {
                            const uint64_t expected = expected_page_checksum(
                                seed, regions[index].get_id(), page, page_size);
                            uint64_t actual = 0;
                            bool matched = false;
                            while (Clock::now() < deadline) {
                                if (!regions[index].read(static_cast<std::size_t>(page * page_size),
                                                         bytes.data(), page_size)) {
                                    fail("no se pudo leer la region " +
                                         std::to_string(regions[index].get_id()) + ": " +
                                         DSM::last_error_message());
                                    return;
                                }
                                actual = page_checksum(bytes.data(), page_size);
                                if (actual == expected) {
                                    matched = true;
                                    break;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                            if (!matched) {
                                fail("checksum distinto en region " +
                                     std::to_string(regions[index].get_id()) +
                                     ", pagina " + std::to_string(page) +
                                     " (esperado 0x" + hex64(expected) +
                                     ", obtenido 0x" + hex64(actual) + ")");
                                return;
                            }
                            page_hashes[index][static_cast<std::size_t>(page)] = actual;
                            page_ok[index][static_cast<std::size_t>(page)] = 1;
                        }
                    }
                });
            }
        } catch (const std::exception& exception) {
            fail(std::string("no se pudo crear un hilo de verificacion: ") + exception.what());
        }
        for (auto& worker : workers) worker.join();
        if (failed) {
            error = first_error;
            return false;
        }

        for (std::size_t index = 0; index < regions.size(); ++index) {
            for (unsigned char state : page_ok[index]) {
                if (state == 0) {
                    error = "quedaron paginas sin verificar en la region " +
                            std::to_string(regions[index].get_id());
                    return false;
                }
            }
            const uint64_t page_count = regions[index].get_size() / page_size;
            const uint64_t expected = expected_region_checksum(
                seed, regions[index].get_id(), page_count, page_size);
            const uint64_t actual = fold_checksums(page_hashes[index]);
            if (actual != expected) {
                error = "checksum de region distinto en " +
                        std::to_string(regions[index].get_id());
                return false;
            }
            checksums[index] = actual;
        }
        return true;
    }

    bool wait_for_nodes(uint32_t expected, uint32_t timeout_ms, std::string& error) {
        if (expected <= 1) return true;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (Clock::now() < deadline) {
            const auto nodes = DSM::get_nodes();
            if (nodes.size() >= expected) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        error = "se esperaban " + std::to_string(expected) + " participantes y se conocen " +
                std::to_string(DSM::get_nodes().size());
        return false;
    }

    bool verify_peers(const Options& opt, const DSMNodeInfo& self, uint32_t page_size,
                      std::vector<PeerSummary>& peers, std::string& error) {
        if (opt.expect_peers == 0) return true;

        const auto deadline = Clock::now() + std::chrono::milliseconds(opt.verify_timeout_ms);
        std::set<DSMNodeId> verified;
        std::map<DSMRegionId, DSMRegion> attached;
        std::string last_error;

        while (Clock::now() < deadline &&
               verified.size() < static_cast<std::size_t>(opt.expect_peers)) {
            const auto nodes = DSM::get_nodes();
            for (const auto& peer : nodes) {
                if (peer.id == self.id || verified.count(peer.id) != 0) continue;

                const auto remote = DSM::get_remote_regions(peer.id);
                if (DSM::last_error() != DSMError::SUCCESS) {
                    last_error = peer.node_name + ": " + DSM::last_error_message();
                    continue;
                }
                // Un participante sin regiones propias no aporta checksums;
                // se reintenta por si todavia esta creando su region.
                if (remote.empty()) {
                    last_error = peer.node_name + ": sin regiones propias";
                    continue;
                }

                bool ready = true;
                std::vector<DSMRegion> handles;
                std::vector<DSMRegionInfo> owned;
                for (const auto& info : remote) {
                    if (info.page_size != page_size || info.size == 0 ||
                        info.size % page_size != 0) {
                        last_error = peer.node_name + ": region " +
                                     std::to_string(info.id) +
                                     " incompatible con page_size local";
                        ready = false;
                        break;
                    }
                    const auto found = attached.find(info.id);
                    if (found != attached.end()) {
                        handles.push_back(found->second);
                        owned.push_back(info);
                        continue;
                    }
                    DSMRegion handle = DSM::attach_region(info.id, info.owner_id);
                    if (!handle.valid()) {
                        last_error = peer.node_name + ": " + DSM::last_error_message();
                        ready = false;
                        break;
                    }
                    attached.emplace(info.id, handle);
                    handles.push_back(handle);
                    owned.push_back(info);
                }
                if (!ready) continue;

                const auto peer_deadline = std::min(
                    deadline, Clock::now() + std::chrono::milliseconds(2000));
                std::vector<uint64_t> checksums;
                std::string verify_error;
                if (!verify_regions(handles, opt.threads, opt.seed, page_size,
                                    peer_deadline, checksums, verify_error)) {
                    last_error = peer.node_name + ": " + verify_error;
                    continue;
                }

                std::size_t bytes = 0;
                for (std::size_t index = 0; index < owned.size(); ++index) {
                    print_checksum("remote", owned[index].id, owned[index].owner_id,
                                   owned[index].size / page_size, owned[index].size,
                                   checksums[index], peer.node_name);
                    bytes += owned[index].size;
                }
                verified.insert(peer.id);
                peers.push_back({peer.id, peer.node_name, owned.size(), bytes});
                std::cout << "[validate] peer name=" << peer.node_name
                          << " id=" << peer.id
                          << " regions=" << owned.size()
                          << " verified\n" << std::flush;
            }
            if (verified.size() >= static_cast<std::size_t>(opt.expect_peers)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (verified.size() < static_cast<std::size_t>(opt.expect_peers)) {
            error = "se esperaban " + std::to_string(opt.expect_peers) +
                    " pares con regiones verificadas y se validaron " +
                    std::to_string(verified.size());
            if (!last_error.empty()) error += "; ultimo error: " + last_error;
            return false;
        }
        return true;
    }

    bool run(const Options& opt, std::string& error) {
        const DSMNodeInfo self = DSM::get_local_node();
        if (self.id == 0 || self.page_size == 0) {
            error = "no se pudo consultar el nodo local";
            return false;
        }
        if (opt.pages > std::numeric_limits<std::size_t>::max() / self.page_size) {
            error = "el tamano de region solicitado es demasiado grande";
            return false;
        }
        const std::size_t region_size =
            static_cast<std::size_t>(opt.pages) * self.page_size;

        std::cout << "[validate] node=" << self.node_name
                  << " id=" << self.id
                  << " page_size=" << self.page_size
                  << " pages=" << opt.pages
                  << " threads=" << opt.threads
                  << " regions=" << opt.regions
                  << " seed=" << opt.seed << '\n' << std::flush;

        std::vector<DSMRegion> local;
        for (uint32_t index = 0; index < opt.regions; ++index) {
            DSMRegion region = DSM::create_region(region_size);
            if (!region.valid()) {
                error = "no se pudo crear la region " + std::to_string(index) + ": " +
                        DSM::last_error_message();
                return false;
            }
            local.push_back(region);
        }

        if (!write_local_regions(local, opt.threads, opt.seed, self.page_size, error)) {
            return false;
        }
        const auto local_deadline =
            Clock::now() + std::chrono::milliseconds(opt.verify_timeout_ms);
        std::vector<uint64_t> local_checksums;
        if (!verify_regions(local, opt.threads, opt.seed, self.page_size,
                            local_deadline, local_checksums, error)) {
            error = "verificacion local: " + error;
            return false;
        }
        for (std::size_t index = 0; index < local.size(); ++index) {
            print_checksum("local", local[index].get_id(), self.id, opt.pages,
                           region_size, local_checksums[index]);
        }
        std::cout << "[validate] local regions=" << local.size()
                  << " pages=" << (static_cast<uint64_t>(local.size()) * opt.pages)
                  << " bytes=" << (static_cast<uint64_t>(local.size()) * region_size)
                  << '\n' << std::flush;

        if (!wait_for_nodes(opt.expect_nodes, opt.membership_timeout_ms, error)) {
            return false;
        }

        std::vector<PeerSummary> peers;
        if (!verify_peers(opt, self, self.page_size, peers, error)) {
            error = "verificacion remota: " + error;
            return false;
        }
        std::size_t remote_regions = 0;
        std::size_t remote_bytes = 0;
        for (const auto& peer : peers) {
            remote_regions += peer.regions;
            remote_bytes += peer.bytes;
        }
        std::cout << "[validate] remote peers=" << peers.size()
                  << " regions=" << remote_regions
                  << " bytes=" << remote_bytes << '\n' << std::flush;
        return true;
    }
}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        usage(argv[0]);
        return 1;
    }
    if (DSM::initialize(options.config) != DSMError::SUCCESS) {
        std::cerr << "[validate] RESULT FALLO: no se pudo inicializar DSM: "
                  << DSM::last_error_message() << '\n';
        return 1;
    }

    std::string error;
    if (!run(options, error)) {
        std::cerr << "[validate] RESULT FALLO: " << error << '\n' << std::flush;
        DSM::shutdown();
        return 1;
    }

    std::cout << "[validate] RESULT OK\n" << std::flush;
    if (options.hold_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.hold_ms));
    }
    // La validacion ya termino. Si un par se retiro antes de que este nodo
    // devolviera sus copias de solo lectura, shutdown lo informa; eso no
    // invalida los checksums ya verificados.
    const DSMError code = DSM::shutdown();
    if (code != DSMError::SUCCESS) {
        std::cerr << "[validate] aviso: shutdown: " << DSM::last_error_message() << '\n';
    }
    return 0;
}
