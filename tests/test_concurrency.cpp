#include "dsm_node.hpp"
#include "signal_handler.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
    using namespace std::chrono_literals;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FALLO: " << message << std::endl;
            std::exit(1);
        }
    }

    template <typename Predicate>
    void wait_for(Predicate predicate, const char* message) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        require(predicate(), message);
    }

    uint32_t page_size() { return static_cast<uint32_t>(sysconf(_SC_PAGESIZE)); }

    Config configuration(unsigned workers) {
        Config config;
        config.bind_address = "127.0.0.1";
        config.tcp_port = 0;
        config.worker_count = workers;
        config.request_timeout_ms = 3000;
        config.memory_size = page_size() * 4;
        config.page_size = page_size();
        return config;
    }

    bool transfer(int fd, void* data, std::size_t size, bool writing) {
        auto* bytes = static_cast<char*>(data);
        while (size) {
            const auto count = writing ? write(fd, bytes, size) : read(fd, bytes, size);
            if (count == -1 && errno == EINTR) continue;
            if (count <= 0) return false;
            bytes += count;
            size -= static_cast<std::size_t>(count);
        }
        return true;
    }

    enum class OwnerCommand { OPEN_GATE, REQUESTS, MEMBERS, QUIT };
    struct OwnerInfo { uint16_t port; RegionId region; };

    void owner_process(int input, int output, unsigned workers) {
        alarm(25);
        Config config = configuration(workers);
        MemoryManager memory(config.memory_size, config.page_size);
        std::string error;
        require(memory.reserve(error), error);
        NetworkManager network(config, 111);
        DSMNode node(memory, network, 111);
        RegionId region;
        require(node.create_region(page_size(), region, error), error);
        require(node.resolve_fault(0, true, error), error);
        *static_cast<uint64_t*>(memory.base()) = 100;
        std::mutex gate_mutex;
        std::condition_variable gate_ready;
        bool open = false;
        std::atomic<uint64_t> requests{0};
        network.set_page_request_handler([&](const PageRequest& request, NodeId sender,
                                               PageData& data, std::string& why) {
            ++requests;
            std::unique_lock<std::mutex> lock(gate_mutex);
            if (!gate_ready.wait_for(lock, 5s, [&] { return open; })) return false;
            lock.unlock();
            return node.pager().serve_page_request(request, sender, data, why);
        });
        require(network.start(error), error);
        OwnerInfo info{network.port(), region};
        require(transfer(output, &info, sizeof(info), true), "inicio del dueno");
        OwnerCommand command;
        while (transfer(input, &command, sizeof(command), false)) {
            if (command == OwnerCommand::QUIT) break;
            uint64_t result = 0;
            if (command == OwnerCommand::OPEN_GATE) {
                std::lock_guard<std::mutex> lock(gate_mutex);
                open = true;
                gate_ready.notify_all();
            } else if (command == OwnerCommand::REQUESTS) result = requests.load();
            else {
                result = network.nodes().size();
                require(network.history().size() >= result, "historial consistente con participantes");
            }
            require(transfer(output, &result, sizeof(result), true), "consulta al dueno");
        }
        network.stop();
    }

    struct Owner {
        pid_t pid;
        int input, output;
        OwnerInfo info{};

        explicit Owner(unsigned workers) {
            int commands[2], responses[2];
            require(pipe(commands) == 0 && pipe(responses) == 0, "pipes del dueno");
            pid = fork();
            require(pid != -1, "proceso del dueno");
            if (pid == 0) {
                close(commands[1]); close(responses[0]);
                owner_process(commands[0], responses[1], workers);
                _exit(0);
            }
            close(commands[0]); close(responses[1]);
            input = commands[1]; output = responses[0];
            require(transfer(output, &info, sizeof(info), false), "leer inicio del dueno");
        }
        Owner(const Owner&) = delete;
        Owner& operator=(const Owner&) = delete;
        uint64_t call(OwnerCommand command) {
            uint64_t value;
            require(transfer(input, &command, sizeof(command), true) &&
                    transfer(output, &value, sizeof(value), false), "comando al dueno");
            return value;
        }
        ~Owner() {
            OwnerCommand quit = OwnerCommand::QUIT;
            transfer(input, &quit, sizeof(quit), true);
            close(input); close(output);
            int status = 0;
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
            require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "cierre del dueno");
        }
    };

    void remote_faults_and_join(unsigned workers) {
        Owner owner(workers);
        Config config = configuration(workers);
        config.bootstrap_nodes = "127.0.0.1:" + std::to_string(owner.info.port);
        MemoryManager memory(config.memory_size, config.page_size);
        std::string error;
        require(memory.reserve(error), error);
        NetworkManager network(config, 222);
        DSMNode node(memory, network, 222);
        SignalHandler faults(memory, [&](PageId page, bool write, std::string& why) {
            return node.resolve_fault(page, write, why);
        });
        require(faults.start(error), error);
        require(network.start(error), error);
        require(node.attach_region_from(111, owner.info.region, error), error);
        auto* value = static_cast<volatile uint64_t*>(memory.base());
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> readers;
        for (unsigned i = 0; i < 4; ++i) {
            readers.emplace_back([&] {
                FaultThread thread;
                std::string why;
                require(thread.prepare(why), why);
                ++ready;
                while (!go.load()) std::this_thread::yield();
                require(*value == 100, "lectura remota por SIGSEGV");
            });
        }
        wait_for([&] { return ready == 4; }, "cuatro lectores listos");
        go = true;
        wait_for([&] { return node.pager().statistics().coalesced_faults == 3; },
                 "tres fallos esperando la misma solicitud");
        wait_for([&] { return owner.call(OwnerCommand::REQUESTS) != 0; }, "PAGE_REQUEST llega al dueno");
        require(owner.call(OwnerCommand::REQUESTS) == 1, "solo un PAGE_REQUEST para cuatro fallos");

        NetworkManager joining(config, 333);
        std::atomic<bool> joined{false};
        std::thread join([&] {
            std::string why;
            require(joining.start(why), why);
            joined = true;
        });
        wait_for([&] {
            network.history();
            node.pager().statistics();
            node.pager().region_snapshot();
            return joined.load();
        }, "JOIN avanza con PAGE_REQUEST pendiente");
        join.join();
        require(owner.call(OwnerCommand::MEMBERS) == 3, "dueno conoce los tres nodos");
        owner.call(OwnerCommand::OPEN_GATE);
        for (auto& reader : readers) reader.join();
        require(node.pager().statistics().page_requests == 1 &&
                node.pager().statistics().page_transfers == 1, "transferencia deduplicada");
        wait_for([&] { return network.nodes().size() == 3; }, "anuncio concurrente al cliente");
        require(network.history().size() >= 4, "historial protegido durante JOIN");

        std::mutex application_mutex;
        std::vector<std::thread> writers;
        for (unsigned i = 0; i < 8; ++i) {
            writers.emplace_back([&] {
                FaultThread thread;
                std::string why;
                require(thread.prepare(why), why);
                for (unsigned j = 0; j < 1000; ++j) {
                    // MSI no convierte un incremento compuesto en una operacion atomica.
                    std::lock_guard<std::mutex> lock(application_mutex);
                    *value = *value + 1;
                }
            });
        }
        for (auto& writer : writers) writer.join();
        require(*value == 8100, "ocho escritores conservan todos los incrementos");
        require(node.pager().statistics().page_requests == 2, "una unica promocion a M");
        joining.stop();
        network.stop();
        faults.stop();
        std::cout << "Fallos, deduplicacion y JOIN con " << workers << " worker(s): OK" << std::endl;
    }

    void independent_pages() {
        MemoryManager memory(page_size() * 4, page_size());
        std::string error;
        require(memory.reserve(error), error);
        Pager pager(memory, 2);
        require(pager.attach_region({55, 99, page_size() * 2, page_size()}, error), error);
        std::mutex gate_mutex;
        std::condition_variable gate_ready;
        bool open = false;
        std::atomic<bool> first_started{false}, second_done{false};
        pager.set_page_fetcher([&](const RemotePageQuery& query, PageData& data, std::string&) {
            if (query.page_id == 0) {
                first_started = true;
                std::unique_lock<std::mutex> lock(gate_mutex);
                require(gate_ready.wait_for(lock, 5s, [&] { return open; }), "abrir primera pagina");
            }
            data.page_id = query.page_id; data.region_id = query.region_id;
            data.region_offset = query.region_offset;
            data.data.assign(page_size(), static_cast<uint8_t>(query.page_id + 1));
            data.data_size = page_size();
            return true;
        });
        SignalHandler faults(memory, [&](PageId id, bool write, std::string& why) {
            return pager.resolve_fault(id, write, why);
        });
        require(faults.start(error), error);
        auto read_page = [&](PageId id) {
            FaultThread thread;
            std::string why;
            require(thread.prepare(why), why);
            require(*static_cast<volatile uint8_t*>(memory.page_address(id)) == id + 1,
                    "contenido de pagina independiente");
        };
        std::thread first([&] { read_page(0); });
        wait_for([&] { return first_started.load(); }, "primer fallo pendiente");
        RegionId local_region;
        require(pager.create_region(page_size() * 2, local_region, error) &&
                pager.page_snapshot().size() == 4 && pager.region_snapshot().size() == 2,
                "registrar regiones y consultar tablas mientras una pagina espera");
        std::thread second([&] { read_page(1); second_done = true; });
        wait_for([&] { return second_done.load(); }, "otra pagina avanza mientras la primera espera");
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            open = true;
            gate_ready.notify_all();
        }
        first.join(); second.join();
    }

    void reciprocal_requests(unsigned workers) {
        Config config = configuration(workers);
        MemoryManager a_memory(config.memory_size, config.page_size);
        MemoryManager b_memory(config.memory_size, config.page_size);
        std::string error;
        require(a_memory.reserve(error) && b_memory.reserve(error), error);
        NetworkManager a_network(config, 444);
        DSMNode a(a_memory, a_network, 444);
        std::atomic<bool> armed{false};
        std::mutex mutex;
        std::condition_variable ready;
        unsigned arrivals = 0;
        auto serve = [&](DSMNode& node, const PageRequest& request, NodeId sender,
                         PageData& data, std::string& why) {
            if (armed) {
                std::unique_lock<std::mutex> lock(mutex);
                ++arrivals;
                ready.notify_all();
                require(ready.wait_for(lock, 2s, [&] { return arrivals == 2; }),
                        "ambos directorios atienden PAGE_REQUEST al mismo tiempo");
            }
            return node.pager().serve_page_request(request, sender, data, why);
        };
        a_network.set_page_request_handler([&](const PageRequest& request, NodeId sender,
                                                PageData& data, std::string& why) {
            return serve(a, request, sender, data, why);
        });
        require(a_network.start(error), error);
        config.bootstrap_nodes = "127.0.0.1:" + std::to_string(a_network.port());
        NetworkManager b_network(config, 555);
        DSMNode b(b_memory, b_network, 555);
        b_network.set_page_request_handler([&](const PageRequest& request, NodeId sender,
                                                PageData& data, std::string& why) {
            return serve(b, request, sender, data, why);
        });
        require(b_network.start(error), error);
        RegionId a_region, b_region;
        require(a.create_region(page_size(), a_region, error) &&
                b.create_region(page_size(), b_region, error), error);
        require(a.resolve_fault(0, true, error) && b.resolve_fault(0, true, error), error);
        *static_cast<uint64_t*>(a_memory.base()) = 10;
        *static_cast<uint64_t*>(b_memory.base()) = 20;
        require(a.attach_region_from(555, b_region, error) &&
                b.attach_region_from(444, a_region, error), error);
        require(a.resolve_fault(1, false, error) && b.resolve_fault(1, false, error), error);
        armed = true;
        std::thread first([&] {
            std::string why;
            require(a.resolve_fault(1, true, why), why);
            *static_cast<uint64_t*>(a_memory.page_address(1)) = 21;
        });
        std::thread second([&] {
            std::string why;
            require(b.resolve_fault(1, true, why), why);
            *static_cast<uint64_t*>(b_memory.page_address(1)) = 11;
        });
        first.join(); second.join();
        require(a.resolve_fault(0, false, error) && b.resolve_fault(0, false, error), error);
        require(*static_cast<uint64_t*>(a_memory.base()) == 11 &&
                *static_cast<uint64_t*>(b_memory.base()) == 21,
                "invalidaciones reciprocas recuperan ambos write-backs");
        b_network.stop(); a_network.stop();
        std::cout << "Invalidaciones reciprocas con " << workers << " worker(s): OK" << std::endl;
    }

    void shared_failure() {
        MemoryManager memory(page_size(), page_size());
        std::string error;
        require(memory.reserve(error), error);
        Pager pager(memory, 2);
        require(pager.attach_region({66, 99, page_size(), page_size()}, error), error);
        std::atomic<bool> release{false};
        pager.set_page_fetcher([&](const RemotePageQuery&, PageData&, std::string& why) {
            while (!release.load()) std::this_thread::yield();
            why = "fallo remoto simulado";
            return false;
        });
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 4; ++i) threads.emplace_back([&] {
            std::string why;
            require(!pager.resolve_fault(0, false, why) && why == "fallo remoto simulado",
                    "todos reciben el resultado del fallo pendiente");
        });
        wait_for([&] { return pager.statistics().coalesced_faults == 3; }, "esperas compartidas");
        release = true;
        for (auto& thread : threads) thread.join();
        require(pager.statistics().page_requests == 1, "el error no dispara solicitudes repetidas");
    }

    void partial_message_timeout() {
        int fds[2];
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
        MessageHeader header{};
        header.magic = DSM_MAGIC; header.version = DSM_PROTOCOL_VERSION;
        const auto bytes = serialize_header(header);
        std::atomic<bool> done{false};
        std::thread sender([&] {
            for (auto byte : bytes) {
                if (done || send(fds[0], &byte, 1, MSG_NOSIGNAL) != 1) break;
                std::this_thread::sleep_for(10ms);
            }
        });
        std::string error;
        std::vector<uint8_t> payload;
        const auto start = std::chrono::steady_clock::now();
        require(!recv_message(fds[1], header, payload, error, start + 60ms) &&
                error.find("timeout") != std::string::npos, "timeout de cabecera parcial");
        require(std::chrono::steady_clock::now() - start < 300ms, "los fragmentos no reinician el timeout");
        done = true;
        sender.join(); close(fds[0]); close(fds[1]);
    }

    void missing_invalidation_ack() {
        Config config = configuration(1);
        config.request_timeout_ms = 300;
        NetworkManager client(config, 123);
        std::string error;
        require(client.start(error), error);
        const int listener = socket(AF_INET, SOCK_STREAM, 0);
        require(listener != -1, "socket de peer sin ACK");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                listen(listener, 1) == 0, "escuchar peer sin ACK");
        socklen_t size = sizeof(address);
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "puerto del peer");
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;
        std::thread peer([&] {
            const int fd = accept(listener, nullptr, nullptr);
            require(fd != -1, "aceptar INVALIDATE");
            MessageHeader request{};
            std::vector<uint8_t> bytes;
            std::string why;
            require(recv_message(fd, request, bytes, why), why);
            InvalidateMessage message;
            require(deserialize_invalidate(bytes, message, why), why);
            PageData data;
            data.page_id = message.page_id; data.region_id = message.region_id;
            data.region_offset = message.region_offset;
            data.version = message.new_version - 1;
            data.data.assign(page_size(), 7);
            require(serialize_page_data(data, bytes, why), why);
            MessageHeader reply{};
            reply.magic = DSM_MAGIC; reply.version = DSM_PROTOCOL_VERSION;
            reply.type = static_cast<uint16_t>(MessageType::PAGE_DATA);
            reply.message_id = request.message_id; reply.sender_id = 321; reply.receiver_id = 123;
            require(send_message(fd, reply, bytes, why), why);
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait_for(lock, 2s, [&] { return done; });
            close(fd);
        });
        NodeInfo target;
        target.id = 321; target.ip_address = "127.0.0.1"; target.tcp_port = ntohs(address.sin_port);
        InvalidateMessage message;
        message.region_id = 77; message.new_owner = 123; message.new_version = 2;
        PageData writeback;
        const auto start = std::chrono::steady_clock::now();
        require(!client.invalidate(target, message, writeback, error) &&
                error.find("timeout") != std::string::npos, "PAGE_DATA sin ACK no confirma la invalidacion");
        require(std::chrono::steady_clock::now() - start < 2s, "INVALIDATE_ACK tiene timeout");
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
            ready.notify_all();
        }
        peer.join(); close(listener); client.stop();
    }
}

int main() {
    alarm(35);
    remote_faults_and_join(1);
    remote_faults_and_join(4);
    reciprocal_requests(1);
    reciprocal_requests(4);
    independent_pages();
    shared_failure();
    partial_message_timeout();
    missing_invalidation_ack();
    std::cout << "test_concurrency: OK" << std::endl;
}
