#include "dsm_node.hpp"
#include "signal_handler.hpp"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
    enum class Op { CREATE, ATTACH, READ, WRITE, INFO, TRY_READ, TRY_WRITE, STALL, DROP_ACK, QUIT };
    struct Command {
        Op op;
        RegionId region = 0;
        NodeId owner = 0;
        uint64_t index = 0;
        uint64_t value = 0;
    };
    struct Result {
        bool ok = false;
        uint64_t value = 0;
        uint64_t version = 0;
        NodeId holder = 0;
        MSIState state = MSIState::INVALID;
        bool busy = false;
    };

    void require(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FALLO: " << message << std::endl;
            std::exit(1);
        }
    }

    bool transfer(int fd, void* buffer, std::size_t size, bool writing) {
        auto* data = static_cast<char*>(buffer);
        while (size) {
            const ssize_t count = writing ? write(fd, data, size) : read(fd, data, size);
            if (count == -1 && errno == EINTR) continue;
            if (count <= 0) return false;
            data += count;
            size -= static_cast<std::size_t>(count);
        }
        return true;
    }

    void child_node(int input, int output, NodeId id, uint16_t bootstrap, unsigned workers) {
        alarm(30);
        const auto page_size = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
        Config config;
        config.node_name = std::to_string(id);
        config.bind_address = "127.0.0.1";
        config.tcp_port = 0;
        config.worker_count = workers;
        config.request_timeout_ms = 300;
        if (bootstrap) config.bootstrap_nodes = "127.0.0.1:" + std::to_string(bootstrap);
        MemoryManager memory(page_size * 8, page_size);
        std::string error;
        require(memory.reserve(error), error);
        NetworkManager network(config, id);
        DSMNode node(memory, network, id);
        std::atomic<bool> stall{false};
        std::atomic<bool> drop_ack{false};
        network.set_invalidation_handler(
            [&](const InvalidateMessage& message, NodeId sender, PageData& data, std::string& why) {
                if (stall.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(650));
                    why = "sin respuesta";
                    return false;
                }
                return node.pager().invalidate_page(message, sender, data, why);
            });
        node.pager().set_page_invalidator(
            [&](NodeId target, const InvalidateMessage& message, PageData& data, std::string& why) {
                NodeInfo peer;
                if (!network.find_node(target, peer) || !network.invalidate(peer, message, data, why)) {
                    return false;
                }
                if (drop_ack.exchange(false)) {
                    why = "ACK perdido despues de retirar la pagina";
                    return false;
                }
                return true;
            });
        SignalHandler faults(memory, [&](PageId page, bool write, std::string& why) {
            return node.resolve_fault(page, write, why);
        });
        require(faults.start(error), error);
        require(network.start(error), error);
        Result ready;
        ready.ok = true;
        ready.value = network.port();
        require(transfer(output, &ready, sizeof(ready), true), "avisar inicio");
        Command command;
        while (transfer(input, &command, sizeof(command), false)) {
            Result result;
            RegionInfo region;
            if (command.op == Op::QUIT) break;
            if (command.op == Op::CREATE) {
                RegionId id_created = 0;
                result.ok = node.create_region(page_size, id_created, error);
                result.value = id_created;
            } else if (command.op == Op::ATTACH) {
                result.ok = node.attach_region_from(command.owner, command.region, error);
            } else if (command.op == Op::STALL) {
                stall = command.value != 0;
                result.ok = true;
            } else if (command.op == Op::DROP_ACK) {
                drop_ack = true;
                result.ok = true;
            } else if (node.pager().region_info(command.region, region)) {
                const PageId page = region.first_page;
                auto* data = static_cast<volatile uint64_t*>(memory.page_address(page));
                result.ok = true;
                if (command.op == Op::READ) result.value = data[command.index];
                if (command.op == Op::WRITE) data[command.index] = command.value;
                if (command.op == Op::TRY_READ || command.op == Op::TRY_WRITE) {
                    result.ok = node.resolve_fault(page, command.op == Op::TRY_WRITE, error);
                }
                PageInfo info;
                require(node.pager().page_info(page, info), "consultar pagina");
                result.state = info.state;
                result.version = info.version;
                GlobalPageControl control;
                if (node.pager().control_info(page, control)) {
                    result.holder = control.current_holder;
                    result.busy = control.write_in_progress;
                }
            }
            require(transfer(output, &result, sizeof(result), true), "responder comando");
        }
        network.stop();
        faults.stop();
    }

    struct Node {
        pid_t pid;
        int input;
        int output;
        uint16_t port;
        NodeId id;

        Node(NodeId node_id, uint16_t bootstrap, unsigned workers) : id(node_id) {
            int commands[2], responses[2];
            require(pipe(commands) == 0 && pipe(responses) == 0, "crear pipes de prueba");
            pid = fork();
            require(pid != -1, "crear proceso de nodo");
            if (pid == 0) {
                close(commands[1]);
                close(responses[0]);
                child_node(commands[0], responses[1], id, bootstrap, workers);
                _exit(0);
            }
            close(commands[0]);
            close(responses[1]);
            input = commands[1];
            output = responses[0];
            Result ready = receive();
            require(ready.ok, "nodo listo");
            port = static_cast<uint16_t>(ready.value);
        }
        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;
        ~Node() {
            Command quit{Op::QUIT};
            transfer(input, &quit, sizeof(quit), true);
            close(input);
            close(output);
            int status = 0;
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
            require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "cierre del nodo");
        }
        void send(Command command) {
            require(transfer(input, &command, sizeof(command), true), "enviar comando");
        }
        Result receive() {
            Result result;
            require(transfer(output, &result, sizeof(result), false), "recibir respuesta");
            return result;
        }
        Result call(Op op, RegionId region = 0, uint64_t value = 0, uint64_t index = 0,
                    NodeId owner = 0) {
            send({op, region, owner, index, value});
            return receive();
        }
    };

    void serialization() {
        InvalidateMessage message;
        message.page_id = 19;
        message.region_id = 0x0102030405060708ULL;
        message.region_offset = 5;
        message.new_version = 9;
        message.new_owner = 7;
        std::string error;
        std::vector<uint8_t> bytes;
        require(serialize_invalidate(message, bytes, error), error);
        require(bytes.size() == 41 && bytes[8] == 1 && bytes[15] == 8 && bytes[31] == 9,
                "INVALIDATE en big-endian sin padding");
        InvalidateMessage decoded;
        require(deserialize_invalidate(bytes, decoded, error) && decoded.page_id == 19 &&
                decoded.region_id == message.region_id && decoded.region_offset == 5 &&
                decoded.new_version == 9 && decoded.new_owner == 7 && decoded.target_state == 2,
                "ida y vuelta de INVALIDATE/ACK");
        bytes.pop_back();
        require(!deserialize_invalidate(bytes, decoded, error), "rechazar INVALIDATE truncado");
        message.target_state = static_cast<uint8_t>(MSIState::MODIFIED);
        require(!serialize_invalidate(message, bytes, error), "INVALIDATE no puede conceder M");
    }

    void msi(unsigned workers) {
        Node a(101, 0, workers);
        Node b(202, a.port, workers);
        Node c(303, a.port, workers);
        const RegionId region = a.call(Op::CREATE).value;
        require(b.call(Op::CREATE).ok && c.call(Op::CREATE).ok && c.call(Op::CREATE).ok,
                "reservar offsets locales diferentes");
        require(b.call(Op::ATTACH, region, 0, 0, a.id).ok &&
                c.call(Op::ATTACH, region, 0, 0, a.id).ok, "adjuntar region remota");
        require(a.call(Op::WRITE, region, 1).version == 1, "A escribe 1 en M version 1");
        require(b.call(Op::READ, region).value == 1 && c.call(Op::READ, region).value == 1,
                "B y C leen el valor de A");
        require(b.call(Op::WRITE, region, 2).version == 2, "B escribe 2 en M version 2");
        require(a.call(Op::INFO, region).state == MSIState::INVALID &&
                c.call(Op::INFO, region).state == MSIState::INVALID, "A y C quedan invalidados");
        require(a.call(Op::INFO, region).holder == b.id, "el directorio registra al holder B");
        require(a.call(Op::READ, region).value == 2 && c.call(Op::READ, region).value == 2,
                "A y C recuperan el write-back de B");
        require(b.call(Op::INFO, region).state == MSIState::SHARED,
                "la lectura de otro nodo baja M a S");
        require(c.call(Op::WRITE, region, 3).version == 3, "C obtiene M version 3");
        require(b.call(Op::READ, region).value == 3, "la copia invalidada de B vuelve a solicitarse");
        require(a.call(Op::WRITE, region, 4).version == 4, "el dueno tambien invalida antes de escribir");
        require(b.call(Op::READ, region).value == 4 && c.call(Op::READ, region).value == 4,
                "los lectores ven la nueva escritura del dueno");

        b.send({Op::WRITE, region, 0, 0, 5});
        c.send({Op::WRITE, region, 0, 0, 6});
        require(b.receive().ok && c.receive().ok, "dos escritores concurrentes terminan");
        const auto b_state = b.call(Op::INFO, region).state;
        const auto c_state = c.call(Op::INFO, region).state;
        require(!(b_state == MSIState::MODIFIED && c_state == MSIState::MODIFIED),
                "no hay dos escritores en M");
        const uint64_t latest = a.call(Op::READ, region).value;
        require((latest == 5 || latest == 6) && b.call(Op::READ, region).value == latest &&
                c.call(Op::READ, region).value == latest, "todos observan el ultimo escritor");

        require(b.call(Op::WRITE, region, 55, 1).ok, "B modifica otro dato de la misma pagina");
        require(a.call(Op::DROP_ACK).ok, "simular perdida de ACK");
        require(!c.call(Op::TRY_WRITE, region).ok, "no conceder M si falta el ACK");
        require(!a.call(Op::INFO, region).busy, "la perdida de ACK libera write_in_progress");
        require(c.call(Op::WRITE, region, 7).ok && c.call(Op::READ, region, 0, 1).value == 55,
                "reintentar write-back retirado sin perder cambios");
        require(a.call(Op::READ, region).value == 7 && b.call(Op::READ, region).value == 7,
                "recuperar escritura despues de ACK perdido");

        require(c.call(Op::STALL, 0, 1).ok, "simular nodo sin respuesta");
        const auto before = std::chrono::steady_clock::now();
        require(!b.call(Op::TRY_WRITE, region).ok, "timeout de INVALIDATE sin conceder M");
        require(std::chrono::steady_clock::now() - before < std::chrono::seconds(2),
                "el timeout limita la espera");
        require(b.call(Op::INFO, region).state != MSIState::MODIFIED,
                "un timeout no habilita escritura");
        require(c.call(Op::STALL, 0, 0).ok, "restablecer nodo");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(!a.call(Op::INFO, region).busy, "un timeout libera write_in_progress");
        require(b.call(Op::WRITE, region, 8).ok && a.call(Op::READ, region).value == 8 &&
                c.call(Op::READ, region).value == 8, "continuar tras el timeout");
        std::cout << "MSI con " << workers << " worker(s): OK" << std::endl;
    }

    void invalidation_during_transfer() {
        const auto size = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
        MemoryManager memory(size, size);
        std::string error;
        require(memory.reserve(error), error);
        Pager replica(memory, 2);
        require(replica.attach_region({50, 1, size, size}, error), error);
        unsigned requests = 0;
        replica.set_page_fetcher([&](const RemotePageQuery& query, PageData& data, std::string& why) {
            ++requests;
            data.page_id = query.page_id;
            data.region_id = query.region_id;
            data.region_offset = query.region_offset;
            data.version = requests == 1 ? 1 : 3;
            data.state = static_cast<uint8_t>(MSIState::MODIFIED);
            data.data.assign(size, requests == 1 ? 0x11 : 0x33);
            data.data_size = size;
            if (requests == 1) {
                InvalidateMessage message;
                message.region_id = 50;
                message.new_owner = 3;
                message.new_version = 2;
                PageData retired;
                return replica.invalidate_page(message, 1, retired, why);
            }
            return true;
        });
        require(replica.resolve_fault(0, true, error), error);
        require(requests == 2 && *static_cast<unsigned char*>(memory.base()) == 0x33,
                "descartar una concesion M invalidada durante la descarga");
        InvalidateMessage message;
        message.region_id = 50;
        message.new_owner = 3;
        message.new_version = 4;
        PageData retired;
        require(!replica.invalidate_page(message, 99, retired, error),
                "solo el dueno puede invalidar la pagina");
        message.new_version = 2;
        require(!replica.invalidate_page(message, 1, retired, error), "rechazar version antigua");
        message.new_version = 4;
        require(replica.invalidate_page(message, 1, retired, error) &&
                retired.version == 3 && retired.data.size() == size && retired.data[0] == 0x33,
                "retirar contenido M antes del ACK");
        PageData retry;
        require(replica.invalidate_page(message, 1, retry, error) && retry.data == retired.data,
                "repetir invalidacion sin perder el write-back");
    }
}

int main() {
    const rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    alarm(35);
    serialization();
    invalidation_during_transfer();
    msi(1);
    msi(4);
    return 0;
}
