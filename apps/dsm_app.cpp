#include "dsm.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    uint64_t parse_id(const char* text) {
        const std::string value(text);
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
            throw std::invalid_argument("el identificador debe ser un entero positivo");
        }
        const auto id = std::stoull(value);
        if (id == 0) throw std::invalid_argument("el identificador no puede ser cero");
        return id;
    }

    int fail() {
        std::cerr << "DSM: " << DSM::last_error_message() << '\n';
        DSM::shutdown();
        return 1;
    }
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::cerr << "Uso: " << argv[0] << " <config> [<region_id> <owner_id>]\n";
        return 1;
    }
    DSMRegionId region_id = 0;
    DSMNodeId owner_id = 0;
    try {
        if (argc == 4) {
            region_id = parse_id(argv[2]);
            owner_id = parse_id(argv[3]);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (DSM::initialize(argv[1]) != DSMError::SUCCESS) return fail();
    const auto local = DSM::get_local_node();
    auto region = argc == 2 ? DSM::create_region(local.page_size) :
                             DSM::attach_region(region_id, owner_id);
    if (!region.valid()) return fail();

    auto* value = region.as<volatile uint64_t>();
    if (argc == 2) {
        *value = 42;
        std::cout << "Nodo: " << local.id << "\nRegion: " << region.get_id()
                  << "\nValor inicial: " << *value
                  << "\nPuedes adjuntar esta region desde otra app. Enter para terminar.\n" << std::flush;
        std::string line;
        std::getline(std::cin, line);
        std::cout << "Valor final: " << *value << '\n';
    } else {
        std::cout << "Valor remoto: " << *value << '\n';
        *value = 43;
        std::cout << "Nuevo valor: " << *value << '\n';
    }
    if (DSM::release_region(region.get_id()) != DSMError::SUCCESS) return fail();
    if (DSM::shutdown() != DSMError::SUCCESS) return fail();
    return 0;
}
