#include "dsm.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
    uint64_t parse_u64(const char* text, const char* label) {
        const std::string value(text);
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
            throw std::invalid_argument(std::string(label) + " debe ser un entero positivo");
        }
        const auto result = std::stoull(value);
        if (result == 0) throw std::invalid_argument(std::string(label) + " no puede ser cero");
        return result;
    }

    int fail() {
        std::cerr << "DSM: " << DSM::last_error_message() << '\n';
        DSM::shutdown();
        return 1;
    }

    uint64_t load(const DSMRegion& region, std::size_t offset) {
        uint64_t value = 0;
        if (!region.read(offset * sizeof(value), &value, sizeof(value))) {
            std::cerr << "DSM: " << DSM::last_error_message() << '\n';
        }
        return value;
    }

    bool store(DSMRegion& region, std::size_t offset, uint64_t value) {
        if (region.write(offset * sizeof(value), &value, sizeof(value))) return true;
        std::cerr << "DSM: " << DSM::last_error_message() << '\n';
        return false;
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Uso: " << argv[0]
                  << " <config> --create <n> [<hold-ms>] | --attach <region_id> <owner_id> <n>\n";
        return 1;
    }
    const std::string mode(argv[2]);
    const bool create = mode == "--create";
    if (!create && mode != "--attach") {
        std::cerr << "Modo desconocido: " << mode << '\n';
        return 1;
    }
    if (argc != (create ? 4 : 6) && !(create && argc == 5)) {
        std::cerr << "Numero de argumentos incorrecto\n";
        return 1;
    }

    try {
        const std::size_t count = static_cast<std::size_t>(
            create ? parse_u64(argv[3], "n") : parse_u64(argv[5], "n"));
        const uint64_t hold_ms = create && argc == 5 ? std::stoull(argv[4]) : 0;
        if (create && hold_ms > 60000) throw std::invalid_argument("hold-ms demasiado grande");
        const DSMRegionId region_id = create ? 0 : std::stoull(argv[3]);
        const DSMNodeId owner_id = create ? 0 : std::stoull(argv[4]);
        const std::size_t slots = count + 1;

        if (DSM::initialize(argv[1]) != DSMError::SUCCESS) return fail();

        DSMRegion region = create ? DSM::create_region(slots * sizeof(uint64_t))
                                   : DSM::attach_region(region_id, owner_id);
        if (!region.valid()) return fail();
        if (region.get_size() < slots * sizeof(uint64_t)) {
            std::cerr << "La region es menor que el arreglo solicitado\n";
            DSM::release_region(region.get_id());
            DSM::shutdown();
            return 1;
        }

        if (create) {
            uint64_t sum = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const uint64_t value = i + 1;
                if (!store(region, i, value)) return fail();
                sum += value;
            }
            std::cout << "[array] region=" << region.get_id()
                      << " owner=" << DSM::get_local_node().id
                      << " n=" << count << " sum=" << sum << '\n' << std::flush;
            if (hold_ms) std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
            std::cout << "[array] final result=" << load(region, count) << '\n';
        } else {
            uint64_t sum = 0;
            for (std::size_t i = 0; i < count; ++i) sum += load(region, i);
            if (!store(region, count, sum)) return fail();
            std::cout << "[array] region=" << region.get_id()
                      << " n=" << count << " sum=" << sum
                      << " wrote=" << load(region, count) << '\n';
            const auto stats = DSM::get_statistics();
            std::cout << "[array] faults=" << stats.page_faults
                      << " transfers=" << stats.page_transfers << '\n';
        }

        if (DSM::release_region(region.get_id()) != DSMError::SUCCESS) return fail();
        if (DSM::shutdown() != DSMError::SUCCESS) return fail();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
