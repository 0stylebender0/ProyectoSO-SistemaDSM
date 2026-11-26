#include "dsm_node.hpp"
#include "memory_manager.hpp"
#include "network.hpp"
#include "pager.hpp"

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
    using namespace std::chrono_literals;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FALLO: " << message << std::endl;
            std::exit(1);
        }
    }

    uint32_t page_size() { return static_cast<uint32_t>(sysconf(_SC_PAGESIZE)); }

    struct Participant {
        Config config;
        MemoryManager memory;
        NetworkManager network;
        std::unique_ptr<DSMNode> node;

        static Config settings(const std::string& name, uint64_t pages, uint16_t bootstrap) {
            Config result;
            result.node_name = name;
            result.memory_size = pages * page_size();
            result.page_size = page_size();
            result.tcp_port = 0;
            result.bind_address = "127.0.0.1";
            result.worker_count = 2;
            result.request_timeout_ms = 1000;
            result.heartbeat_interval_ms = 100;
            result.heartbeat_timeout_ms = 1000;
            if (bootstrap) result.bootstrap_nodes = "127.0.0.1:" + std::to_string(bootstrap);
            return result;
        }

        Participant(NodeId id, const std::string& name, uint64_t pages, uint16_t bootstrap = 0)
            : config(settings(name, pages, bootstrap)),
              memory(config.memory_size, config.page_size), network(config, id) {
            std::string error;
            require(memory.reserve(error), error);
            node = std::make_unique<DSMNode>(memory, network, id);
            require(network.start(error), error);
        }

        ~Participant() { network.stop(); }

        volatile uint64_t* word(PageId page) {
            return static_cast<volatile uint64_t*>(memory.page_address(page));
        }

        RegionInfo region(RegionId id) {
            RegionInfo info;
            require(node->pager().region_info(id, info), "region de prueba");
            return info;
        }

        void write_page(PageId page, uint64_t value) {
            std::string error;
            require(node->resolve_fault(page, true, error), error);
            *word(page) = value;
        }

        void expect_page(PageId page, uint64_t value) {
            std::string error;
            require(node->resolve_fault(page, false, error), error);
            require(*word(page) == value, "contenido inesperado en la pagina " + std::to_string(page));
        }
    };

    void local_release_and_reuse() {
        const uint64_t page = page_size();
        MemoryManager memory(page * 8, static_cast<uint32_t>(page));
        std::string error;
        require(memory.reserve(error), error);

        Pager pager(memory, 42);
        RegionId first = 0, middle = 0, last = 0;
        require(pager.create_region(page * 2, first, error), error);
        require(pager.create_region(page, middle, error), error);
        require(pager.create_region(page * 3, last, error), error);

        const RegionInfo first_info = [&] {
            RegionInfo info;
            require(pager.region_info(first, info), "primera region");
            return info;
        }();
        const RegionInfo middle_info = [&] {
            RegionInfo info;
            require(pager.region_info(middle, info), "region intermedia");
            return info;
        }();
        const RegionInfo last_info = [&] {
            RegionInfo info;
            require(pager.region_info(last, info), "ultima region");
            return info;
        }();
        require(first_info.first_page == 0 && middle_info.first_page == 2 &&
                last_info.first_page == 3, "asignacion contigua inicial");
        require(pager.statistics().pages_owned == 6, "paginas propias antes de liberar");

        void* middle_address = memory.page_address(middle_info.first_page);
        require(pager.release_region(middle, error), error);
        RegionInfo released_info;
        require(!pager.region_info(middle, released_info), "la region liberada desaparece");
        PageInfo released_page;
        require(!pager.page_info(middle_info.first_page, released_page),
                "liberar la region retira sus paginas");

        RegionId reused = 0;
        require(pager.create_region(page, reused, error), error);
        RegionInfo reused_info;
        require(pager.region_info(reused, reused_info), "region reutilizada");
        require(reused != middle && reused_info.first_page == middle_info.first_page,
                "reutilizar el hueco de una pagina");
        require(memory.page_address(reused_info.first_page) == middle_address,
                "la direccion del hueco se conserva");

        std::string fault_error;
        require(pager.resolve_fault(reused_info.first_page, false, fault_error), fault_error);
        require(*static_cast<volatile uint64_t*>(
                    memory.page_address(reused_info.first_page)) == 0,
                "la pagina reutilizada se entrega limpia");

        require(pager.release_region(first, error), error);
        require(pager.release_region(reused, error), error);
        RegionId merged = 0;
        require(pager.create_region(page * 3, merged, error), error);
        RegionInfo merged_info;
        require(pager.region_info(merged, merged_info), "region fusionada");
        require(merged_info.first_page == first_info.first_page &&
                memory.page_address(merged_info.first_page) ==
                    memory.page_address(first_info.first_page),
                "fusionar rangos libres contiguos");
        require(pager.resolve_fault(merged_info.first_page, false, fault_error), fault_error);
        require(*static_cast<volatile uint64_t*>(
                    memory.page_address(merged_info.first_page)) == 0,
                "la region fusionada se entrega limpia");

        require(pager.release_region(last, error), error);
        require(pager.release_region(merged, error), error);
        require(pager.region_snapshot().empty() && pager.page_snapshot().empty(),
                "todas las paginas se retiran al liberar");
        require(pager.statistics().pages_owned == 0, "no quedan paginas propias");

        require(!pager.release_region(merged, error) && error == "region desconocida",
                "doble liberacion");
        RegionId unknown = 123456789;
        require(!pager.release_region(unknown, error) && error == "region desconocida",
                "liberar una region desconocida");
        require(!pager.release_region(0, error) && error == "region desconocida",
                "liberar el identificador cero");
    }

    void pending_fault_blocks_release() {
        const uint64_t page = page_size();
        MemoryManager memory(page * 2, static_cast<uint32_t>(page));
        std::string error;
        require(memory.reserve(error), error);

        Pager pager(memory, 7);
        RegionId region = 987654321;
        require(pager.attach_region({region, 99, page, static_cast<uint32_t>(page)}, error), error);
        pager.set_page_releaser([](NodeId, const PageData&, std::string&) { return true; });

        std::mutex mutex;
        std::condition_variable ready;
        bool started = false;
        bool release = false;
        pager.set_page_fetcher([&](const RemotePageQuery& query, PageData& data, std::string&) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                started = true;
            }
            ready.notify_all();
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [&] { return release; });
            data.page_id = query.page_id;
            data.region_id = query.region_id;
            data.region_offset = query.region_offset;
            data.version = 1;
            data.state = static_cast<uint8_t>(MSIState::SHARED);
            data.data.assign(static_cast<std::size_t>(page), 0xAB);
            data.data_size = static_cast<uint32_t>(page);
            return true;
        });

        std::string fault_error;
        bool fault_ok = false;
        std::thread fault([&] { fault_ok = pager.resolve_fault(0, false, fault_error); });
        {
            std::unique_lock<std::mutex> lock(mutex);
            require(ready.wait_for(lock, 2s, [&] { return started; }),
                    "el fallo remoto no comenzo");
        }

        require(!pager.release_region(region, error) && error == "region en uso",
                "no liberar una region con un fallo pendiente");
        RegionInfo info;
        require(pager.region_info(region, info), "la region sobrevive al intento fallido");
        require(pager.page_snapshot().size() == 1, "la pagina sobrevive al intento fallido");

        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        ready.notify_all();
        fault.join();
        require(fault_ok, fault_error);
        require(pager.release_region(region, error), error);
        require(!pager.region_info(region, info), "la region se libera tras resolver el fallo");
        require(pager.page_snapshot().empty(), "no quedan paginas tras la liberacion");
    }

    void remote_release_and_recreate() {
        const uint64_t page = page_size();
        Participant a(101, "A", 16);
        std::string error;
        RegionId first = 0;
        require(a.node->create_region(page * 3, first, error), error);
        const RegionInfo first_info = a.region(first);
        a.write_page(first_info.first_page, 100);
        a.write_page(first_info.first_page + 1, 101);
        a.write_page(first_info.first_page + 2, 102);
        void* first_address = a.memory.page_address(first_info.first_page);

        Participant b(202, "B", 16, a.network.port());
        require(b.node->attach_region_from(101, first, error), error);
        const RegionInfo b_first = b.region(first);
        b.expect_page(b_first.first_page, 100);
        b.expect_page(b_first.first_page + 1, 101);
        b.expect_page(b_first.first_page + 2, 102);

        // B escribe: el dueno debe recibir el write-back al invalidar.
        b.write_page(b_first.first_page + 1, 201);
        a.expect_page(first_info.first_page + 1, 201);

        // B devuelve la copia y el dueno conserva el contenido.
        require(b.node->pager().release_region(first, error), error);
        RegionInfo released;
        require(!b.node->pager().region_info(first, released), "B libera su copia remota");
        a.expect_page(first_info.first_page, 100);
        a.expect_page(first_info.first_page + 1, 201);
        a.expect_page(first_info.first_page + 2, 102);

        // El dueno libera la region y recrea otra del mismo tamano.
        require(a.node->pager().release_region(first, error), error);
        require(!a.node->pager().region_info(first, released), "A libera la region");

        RegionId second = 0;
        require(a.node->create_region(page * 3, second, error), error);
        const RegionInfo second_info = a.region(second);
        require(second != first && second_info.first_page == first_info.first_page &&
                a.memory.page_address(second_info.first_page) == first_address,
                "el dueno reutiliza el rango liberado");
        a.expect_page(second_info.first_page, 0);
        require(a.node->pager().statistics().pages_owned == 3,
                "las paginas propias se conservan tras recrear");

        // Liberar en el dueno invalida una copia M remota sin perder el write-back.
        require(b.node->attach_region_from(101, second, error), error);
        const RegionInfo b_second = b.region(second);
        b.write_page(b_second.first_page + 2, 302);
        require(a.node->pager().release_region(second, error),
                "A libera una region con una copia M remota");
        require(b.node->pager().release_region(second, error),
                "B puede desadjuntar una region ya liberada por el dueno");

        // Liberar en el dueno invalida una copia S remota.
        RegionId third = 0;
        require(a.node->create_region(page * 2, third, error), error);
        const RegionInfo third_info = a.region(third);
        a.write_page(third_info.first_page, 400);
        a.write_page(third_info.first_page + 1, 401);
        require(b.node->attach_region_from(101, third, error), error);
        const RegionInfo b_third = b.region(third);
        b.expect_page(b_third.first_page, 400);
        b.expect_page(b_third.first_page + 1, 401);
        require(a.node->pager().release_region(third, error),
                "A libera una region con copias S remotas");
        require(b.node->pager().release_region(third, error),
                "B desadjunta la region con copias invalidadas");

        // Una region eliminada ya no se puede volver a adjuntar.
        RegionInfo missing;
        require(!b.node->attach_region_from(101, first, error),
                "no se puede adjuntar una region eliminada");
        require(!b.node->pager().region_info(first, missing), "B no registro la region eliminada");
        RegionId unknown = 123456789;
        require(!b.node->attach_region_from(101, unknown, error),
                "no se puede adjuntar una region inexistente");

        require(a.node->pager().region_snapshot().empty(),
                "el dueno libero todas sus regiones");
        require(b.node->pager().region_snapshot().empty(),
                "el cliente libero todas sus copias");
        b.network.stop();
        a.network.stop();
    }
}

int main() {
    alarm(30);
    local_release_and_reuse();
    pending_fault_blocks_release();
    remote_release_and_recreate();
    std::cout << "test_release: OK (local, reutilizacion, fallo pendiente, remoto y recreacion)\n";
    return 0;
}
