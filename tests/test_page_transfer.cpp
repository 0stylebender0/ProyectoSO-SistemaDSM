#include "config.hpp"
#include "dsm_node.hpp"
#include "memory_manager.hpp"
#include "network.hpp"
#include "node.hpp"
#include "pager.hpp"
#include "protocol.hpp"
#include "signal_handler.hpp"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
    void require(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FALLO: " << message << "\n";
            std::exit(1);
        }
    }

    uint32_t host_page_size() {
        return static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
    }

    std::string permissions_at(void* address) {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        const auto value = reinterpret_cast<uintptr_t>(address);
        while (std::getline(maps, line)) {
            unsigned long begin, end;
            char permissions[5]{};
            if (std::sscanf(line.c_str(), "%lx-%lx %4s", &begin, &end, permissions) == 3 &&
                value >= begin && value < end) {
                return permissions;
            }
        }
        return {};
    }

    Config make_config(const std::string& name, uint32_t page_size) {
        Config config;
        config.node_name = name;
        config.bind_address = "127.0.0.1";
        config.tcp_port = 0;
        config.worker_count = 2;
        config.max_connections = 16;
        config.request_timeout_ms = 2000;
        config.page_size = page_size;
        config.memory_size = static_cast<uint64_t>(page_size) * 16;
        return config;
    }

    void serialization() {
        const uint32_t page = host_page_size();

        PageRequest request;
        request.page_id = 77;
        request.region_id = 0x0102030405060708ULL;
        request.region_offset = 3;
        request.access_type = 0;
        request.current_state = static_cast<uint8_t>(MSIState::INVALID);
        request.known_version = 9;

        std::vector<uint8_t> bytes;
        std::string error;
        require(serialize_page_request(request, bytes, error), error);
        require(bytes.size() == 34, "tamano de PageRequest");
        PageRequest decoded;
        require(deserialize_page_request(bytes, decoded, error), error);
        require(decoded.page_id == request.page_id &&
                decoded.region_id == request.region_id &&
                decoded.region_offset == request.region_offset &&
                decoded.access_type == request.access_type &&
                decoded.current_state == request.current_state &&
                decoded.known_version == request.known_version,
                "ida y vuelta de PageRequest");

        PageData page_data;
        page_data.page_id = 12;
        page_data.region_id = request.region_id;
        page_data.region_offset = 4;
        page_data.version = 5;
        page_data.state = static_cast<uint8_t>(MSIState::SHARED);
        page_data.data.assign(page, 0x5A);
        page_data.data_size = page;
        require(serialize_page_data(page_data, bytes, error), error);
        PageData decoded_page;
        require(deserialize_page_data(bytes, decoded_page, error), error);
        require(decoded_page.page_id == page_data.page_id &&
                decoded_page.region_id == page_data.region_id &&
                decoded_page.region_offset == page_data.region_offset &&
                decoded_page.version == page_data.version &&
                decoded_page.state == page_data.state &&
                decoded_page.data_size == page &&
                decoded_page.data == page_data.data,
                "ida y vuelta de PageData");
        auto truncated = bytes;
        truncated.pop_back();
        require(!deserialize_page_data(truncated, decoded_page, error),
                "rechazar PageData truncado");

        RegionInfoRequest info_request;
        info_request.region_id = request.region_id;
        require(serialize_region_info_request(info_request, bytes, error), error);
        RegionInfoRequest decoded_request;
        require(deserialize_region_info_request(bytes, decoded_request, error), error);
        require(decoded_request.region_id == info_request.region_id,
                "ida y vuelta de REGION_INFO_REQUEST");

        RegionInfoResponse info_response;
        info_response.region_id = request.region_id;
        info_response.owner_id = 0x1112131415161718ULL;
        info_response.size = static_cast<uint64_t>(page) * 2;
        info_response.page_size = page;
        require(serialize_region_info_response(info_response, bytes, error), error);
        RegionInfoResponse decoded_response;
        require(deserialize_region_info_response(bytes, decoded_response, error), error);
        require(decoded_response.region_id == info_response.region_id &&
                decoded_response.owner_id == info_response.owner_id &&
                decoded_response.size == info_response.size &&
                decoded_response.page_size == info_response.page_size,
                "ida y vuelta de REGION_INFO_RESPONSE");
    }

    void remote_reads() {
        const uint32_t page = host_page_size();
        std::string error;

        NodeId a_id = 0;
        NodeId b_id = 0;
        require(generate_node_id(a_id, error), error);
        require(generate_node_id(b_id, error), error);

        Config a_config = make_config("A", page);
        MemoryManager a_memory(a_config.memory_size, a_config.page_size);
        require(a_memory.reserve(error), error);
        NetworkManager a_network(a_config, a_id);
        DSMNode a_node(a_memory, a_network, a_id);
        require(a_network.start(error), "iniciar A: " + error);

        Config b_config = make_config("B", page);
        b_config.bootstrap_nodes = "127.0.0.1:" + std::to_string(a_network.port());
        MemoryManager b_memory(b_config.memory_size, b_config.page_size);
        require(b_memory.reserve(error), error);
        NetworkManager b_network(b_config, b_id);
        DSMNode b_node(b_memory, b_network, b_id);
        require(b_network.start(error), "iniciar B: " + error);

        RegionId region_id = 0;
        require(a_node.create_region(static_cast<uint64_t>(page) * 2, region_id, error),
                error);
        RegionInfo a_region;
        require(a_node.pager().region_info(region_id, a_region), "region de A");
        const PageId a_page0 = a_region.first_page;
        const PageId a_page1 = a_page0 + 1;

        require(a_node.resolve_fault(a_page0, true, error), error);
        std::memset(a_memory.page_address(a_page0), 0xAB, page);
        require(a_node.resolve_fault(a_page1, true, error), error);
        std::memset(a_memory.page_address(a_page1), 0xCD, page);

        require(b_node.attach_region_from(a_id, region_id, error),
                "adjuntar region remota: " + error);
        RegionInfo b_region;
        require(b_node.pager().region_info(region_id, b_region), "region de B");
        const PageId b_page0 = b_region.first_page;
        const PageId b_page1 = b_page0 + 1;

        SignalHandler faults(b_memory,
            [&b_node](PageId page_id, bool write, std::string& callback_error) {
                return b_node.resolve_fault(page_id, write, callback_error);
            });
        require(faults.start(error), error);

        auto* b_data0 = static_cast<volatile unsigned char*>(b_memory.page_address(b_page0));
        for (uint32_t i = 0; i < page; ++i) {
            require(b_data0[i] == 0xAB, "patron remoto de la pagina 0");
        }

        PageInfo info;
        require(b_node.pager().page_info(b_page0, info), "pagina 0 de B");
        require(info.state == MSIState::SHARED && info.locally_present &&
                info.version == 1, "B queda SHARED con la version de A");
        require(permissions_at(b_memory.page_address(b_page0)) == "r--p",
                "B habilita solo lectura");

        require(a_node.pager().page_info(a_page0, info), "pagina 0 de A");
        require(info.state == MSIState::SHARED && info.locally_present &&
                info.version == 1, "A baja de MODIFIED a SHARED");
        require(info.sharers.size() == 1 && info.sharers.count(b_id) == 1,
                "A registra a B como sharer");
        require(permissions_at(a_memory.page_address(a_page0)) == "r--p",
                "A tambien queda en solo lectura al compartir");

        const auto* a_data0 = static_cast<const unsigned char*>(a_memory.page_address(a_page0));
        require(a_data0[0] == 0xAB && a_data0[page - 1] == 0xAB,
                "compartir no altera el contenido del dueno");

        // La segunda lectura usa la copia local: no hay fallo ni nueva solicitud.
        require(b_data0[0] == 0xAB && b_data0[page - 1] == 0xAB,
                "segunda lectura desde la copia local");
        require(a_node.pager().page_info(a_page0, info) && info.sharers.size() == 1,
                "la segunda lectura no duplica sharers");

        auto* b_data1 = static_cast<volatile unsigned char*>(b_memory.page_address(b_page1));
        for (uint32_t i = 0; i < page; ++i) {
            require(b_data1[i] == 0xCD, "patron remoto de la pagina 1");
        }
        require(a_node.pager().page_info(a_page1, info) &&
                info.state == MSIState::SHARED && info.sharers.count(b_id) == 1,
                "page_offset identifica la pagina correcta en el dueno");

        // Una region nunca materializada en el dueno se comparte en ceros.
        RegionId zero_region = 0;
        require(a_node.create_region(page, zero_region, error), error);
        require(b_node.attach_region_from(a_id, zero_region, error), error);
        RegionInfo b_zero_region;
        RegionInfo a_zero_region;
        require(a_node.pager().region_info(zero_region, a_zero_region), "region cero de A");
        require(b_node.pager().region_info(zero_region, b_zero_region), "region cero de B");
        auto* b_zero = static_cast<volatile unsigned char*>(
            b_memory.page_address(b_zero_region.first_page));
        for (uint32_t i = 0; i < page; ++i) {
            require(b_zero[i] == 0, "pagina remota materializada en cero");
        }
        require(a_node.pager().page_info(a_zero_region.first_page, info) &&
                info.state == MSIState::SHARED && info.locally_present &&
                info.version == 0, "el dueno registra la pagina compartida en cero");

        NodeInfo a_info;
        require(b_network.find_node(a_id, a_info), "B conoce a A");
        RegionDescriptor missing;
        require(!b_network.query_region(a_info, 0xDEADBEEFULL, missing, error),
                "rechazar una region inexistente");

        // La escritura remota corresponde al hito 2.5.
        require(!b_node.resolve_fault(b_page0, true, error),
                "rechazar escritura remota en 2.4");
        require(permissions_at(b_memory.page_address(b_page0)) == "r--p",
                "la escritura remota rechazada no cambia permisos");

        faults.stop();
        b_network.stop();
        a_network.stop();
    }
}  // namespace

int main() {
    alarm(20);
    serialization();
    remote_reads();
    std::cout << "test_page_transfer: OK\n";
    return 0;
}
