#include "dsm.hpp"

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace {
    void require(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FALLO: " << message << ": " << DSM::last_error_message() << std::endl;
            std::exit(1);
        }
    }

    bool transfer(int fd, void* buffer, std::size_t size, bool sending) {
        auto* bytes = static_cast<char*>(buffer);
        while (size) {
            const ssize_t count = sending ? write(fd, bytes, size) : read(fd, bytes, size);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            bytes += count;
            size -= static_cast<std::size_t>(count);
        }
        return true;
    }

    struct Configuration {
        std::string directory;
        std::string path;
        uint16_t port;
        std::size_t page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));

        explicit Configuration(uint16_t bootstrap = 0) {
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            require(fd != -1, "reservar puerto de prueba");
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            require(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind de prueba");
            socklen_t length = sizeof(address);
            require(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0, "consultar puerto");
            port = ntohs(address.sin_port);
            close(fd);
            char pattern[] = "/tmp/dsm-api-XXXXXX";
            const char* created = mkdtemp(pattern);
            require(created != nullptr, "crear directorio temporal");
            directory = created;
            path = directory + "/dsm.conf";
            std::ofstream config(path);
            config << "node_name=api\nmemory_size=" << page_size * 8 << "\npage_size=" << page_size
                   << "\nbind_address=127.0.0.1\ntcp_port=" << port
                   << "\nworker_count=1\nrequest_timeout_ms=600\n";
            if (bootstrap) config << "bootstrap_nodes=127.0.0.1:" << bootstrap << '\n';
            config.close();
            require(config.good(), "guardar configuracion");
        }
        ~Configuration() { std::filesystem::remove_all(directory); }
    };

    void local_lifecycle() {
        require(!DSM::create_region(1).valid() && DSM::last_error() == DSMError::NOT_INITIALIZED,
                "crear antes de initialize");
        require(DSM::initialize("/no-existe/dsm.conf") == DSMError::INITIALIZATION_ERROR,
                "configuracion inexistente");
        Configuration config;
        require(DSM::initialize(config.path) == DSMError::SUCCESS, "initialize");
        require(DSM::initialize(config.path) == DSMError::ALREADY_INITIALIZED, "doble initialize");
        const auto self = DSM::get_local_node();
        require(self.id != 0 && self.tcp_port == config.port && self.page_size == config.page_size,
                "datos del nodo local");
        require(DSM::get_nodes().size() == 1, "tabla local");
        require(!DSM::create_region(1, DSMAccess::READ).valid() &&
                DSM::last_error() == DSMError::INVALID_ACCESS, "READ no promete permisos inexistentes");
        require(!DSM::create_region(0).valid() && DSM::last_error() == DSMError::INVALID_REGION,
                "region de cero bytes");
        require(!DSM::create_region(std::numeric_limits<std::size_t>::max()).valid() &&
                DSM::last_error() == DSMError::OUT_OF_MEMORY, "desbordamiento de tamano");
        auto first = DSM::create_region(1);
        require(first.valid() && first.get_size() == config.page_size, "redondeo a pagina");
        auto second = DSM::create_region(config.page_size);
        require(second.valid(), "segunda region");
        auto copy = first;
        void* address = first.data();
        *first.as<volatile uint64_t>() = 123;
        require(*first.as<volatile uint64_t>() == 123, "lectura y escritura locales");
        require(DSM::attach_region(first.get_id(), self.id).data() == address, "adjuntar region local");
        require(DSM::get_statistics().pages_owned == 2, "estadisticas de regiones");
        require(DSM::release_region(first.get_id()) == DSMError::SUCCESS, "liberar region");
        require(!copy.valid() && copy.data() == nullptr && !first.valid(), "invalidar copias del handle");
        require(DSM::release_region(first.get_id()) == DSMError::INVALID_REGION, "doble liberacion");
        require(DSM::release_region(second.get_id()) == DSMError::SUCCESS, "liberar rango contiguo");
        auto whole = DSM::create_region(config.page_size * 8);
        require(whole.valid() && whole.data() == address, "fusionar y reutilizar rangos libres");
        require(*whole.as<volatile uint64_t>() == 0, "limpiar memoria reutilizada");
        require(!DSM::create_region(1).valid() && DSM::last_error() == DSMError::OUT_OF_MEMORY,
                "respetar memoria reservada");
        require(DSM::shutdown() == DSMError::SUCCESS, "shutdown con region activa");
        require(!whole.valid() && whole.data() == nullptr, "shutdown invalida handles");
        require(DSM::shutdown() == DSMError::SUCCESS, "shutdown repetido");
        require(DSM::initialize(config.path) == DSMError::SUCCESS, "reinicializar");
        require(!whole.valid(), "un handle anterior no revive");
        require(DSM::shutdown() == DSMError::SUCCESS, "cerrar reinicializacion");

        const int occupied = socket(AF_INET, SOCK_STREAM, 0);
        require(occupied != -1, "socket para puerto ocupado");
        sockaddr_in address_in{};
        address_in.sin_family = AF_INET;
        address_in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_in.sin_port = htons(config.port);
        require(bind(occupied, reinterpret_cast<sockaddr*>(&address_in), sizeof(address_in)) == 0,
                "ocupar puerto");
        require(DSM::initialize(config.path) == DSMError::NETWORK_ERROR, "fallo de red al inicializar");
        close(occupied);
        require(DSM::initialize(config.path) == DSMError::SUCCESS, "limpiar inicializacion fallida");
        require(DSM::shutdown() == DSMError::SUCCESS, "cerrar tras fallo de initialize");
        std::cout << "API local, liberacion y reutilizacion: OK\n";
    }

    void thread_lifecycle() {
        Configuration config;
        require(DSM::initialize(config.path) == DSMError::SUCCESS, "initialize para hilos");
        auto region = DSM::create_region(1);
        require(region.valid(), "region para otro hilo");
        std::mutex mutex;
        std::condition_variable ready;
        bool registered = false;
        bool finish = false;
        std::thread thread([&] {
            DSMThread guard;
            require(guard.valid(), "preparar pila alternativa");
            {
                DSMThread nested;
                require(nested.valid(), "guardia anidada");
                *region.as<volatile uint64_t>() = 456;
            }
            require(*region.as<volatile uint64_t>() == 456, "guardia externa sigue activa");
            require(DSM::shutdown() == DSMError::INVALID_THREAD, "shutdown en otro hilo");
            std::unique_lock<std::mutex> lock(mutex);
            registered = true;
            ready.notify_all();
            ready.wait(lock, [&] { return finish; });
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [&] { return registered; });
        }
        require(DSM::last_error() == DSMError::SUCCESS, "errores independientes por hilo");
        require(DSM::shutdown() == DSMError::BUSY, "guardia impide apagar el runtime");
        {
            std::lock_guard<std::mutex> lock(mutex);
            finish = true;
            ready.notify_all();
        }
        thread.join();
        require(*region.as<volatile uint64_t>() == 456, "escritura desde hilo registrado");
        require(DSM::shutdown() == DSMError::SUCCESS, "shutdown despues de reunir hilos");
        std::cout << "API y guardias de hilo: OK\n";
    }

    enum class Operation { READ, RECREATE, QUIT };
    struct Reply { DSMNodeId owner; DSMRegionId region; uint64_t value; };

    void owner_process(int input, int output, const Configuration& config) {
        alarm(30);
        require(DSM::initialize(config.path) == DSMError::SUCCESS, "initialize del dueno");
        auto region = DSM::create_region(config.page_size);
        require(region.valid(), "region del dueno");
        *region.as<volatile uint64_t>() = 42;
        Reply reply{DSM::get_local_node().id, region.get_id(), 42};
        require(transfer(output, &reply, sizeof(reply), true), "publicar region");
        Operation op;
        while (transfer(input, &op, sizeof(op), false) && op != Operation::QUIT) {
            if (op == Operation::RECREATE) {
                void* address = region.data();
                require(DSM::release_region(region.get_id()) == DSMError::SUCCESS, "borrar region del dueno");
                region = DSM::create_region(config.page_size);
                require(region.valid() && region.data() == address, "reutilizar memoria del dueno");
            }
            reply.region = region.get_id();
            reply.value = *region.as<volatile uint64_t>();
            require(transfer(output, &reply, sizeof(reply), true), "responder desde dueno");
        }
        require(DSM::shutdown() == DSMError::SUCCESS, "shutdown del dueno");
    }

    void remote_lifecycle() {
        Configuration owner_config;
        int commands[2], responses[2];
        require(pipe(commands) == 0 && pipe(responses) == 0, "pipes de nodos");
        std::cout << std::flush;
        const pid_t child = fork();
        require(child != -1, "proceso remoto");
        if (child == 0) {
            close(commands[1]);
            close(responses[0]);
            owner_process(commands[0], responses[1], owner_config);
            _exit(0);
        }
        close(commands[0]);
        close(responses[1]);
        Reply reply{};
        require(transfer(responses[0], &reply, sizeof(reply), false), "recibir region remota");
        auto ask = [&](Operation op) {
            require(transfer(commands[1], &op, sizeof(op), true), "enviar consulta al dueno");
            require(transfer(responses[0], &reply, sizeof(reply), false), "respuesta del dueno");
        };
        Configuration local_config(owner_config.port);
        require(DSM::initialize(local_config.path) == DSMError::SUCCESS, "initialize con bootstrap");
        require(DSM::get_nodes().size() == 2, "descubrimiento usando API");
        auto remote = DSM::attach_region(reply.region, reply.owner);
        require(remote.valid(), "attach remoto");
        require(*remote.as<volatile uint64_t>() == 42, "lectura remota bajo demanda");
        *remote.as<volatile uint64_t>() = 99;
        void* address = remote.data();
        auto copy = remote;
        require(DSM::release_region(remote.get_id()) == DSMError::SUCCESS, "devolver pagina M al liberar");
        require(!copy.valid() && copy.data() == nullptr, "invalidar handle remoto");
        ask(Operation::READ);
        require(reply.value == 99, "dueno conserva escritura del nodo que libero");
        auto local = DSM::create_region(local_config.page_size);
        require(local.valid() && local.data() == address, "reusar rango de region remota");
        require(*local.as<volatile uint64_t>() == 0, "rango remoto reutilizado vacio");
        require(DSM::release_region(local.get_id()) == DSMError::SUCCESS, "liberar rango reutilizado");

        remote = DSM::attach_region(reply.region, reply.owner);
        require(remote.valid() && *remote.as<volatile uint64_t>() == 99, "volver a adjuntar");
        *remote.as<volatile uint64_t>() = 101;
        require(kill(child, SIGSTOP) == 0, "pausar dueno para probar timeout");
        int stopped;
        require(waitpid(child, &stopped, WUNTRACED) == child && WIFSTOPPED(stopped), "dueno detenido");
        require(DSM::release_region(remote.get_id()) == DSMError::NETWORK_ERROR,
                "timeout de PAGE_RELEASE");
        require(remote.valid(), "fallo de liberacion conserva el handle para reintentar");
        require(kill(child, SIGCONT) == 0, "reanudar dueno");
        require(DSM::release_region(remote.get_id()) == DSMError::SUCCESS, "reintentar liberacion");
        ask(Operation::READ);
        require(reply.value == 101, "write-back conservado tras timeout");

        remote = DSM::attach_region(reply.region, reply.owner);
        require(remote.valid() && *remote.as<volatile uint64_t>() == 101, "copia S antes de borrar");
        const auto old_id = remote.get_id();
        ask(Operation::RECREATE);
        require(reply.region != old_id && reply.value == 0, "dueno borra y crea otra region limpia");
        require(DSM::release_region(old_id) == DSMError::SUCCESS, "desadjuntar region ya borrada por dueno");
        remote = DSM::attach_region(reply.region, reply.owner);
        require(remote.valid() && *remote.as<volatile uint64_t>() == 0, "adjuntar nueva region");
        *remote.as<volatile uint64_t>() = 777;
        require(DSM::shutdown() == DSMError::SUCCESS, "shutdown devuelve paginas M remotas");
        require(!remote.valid(), "shutdown invalida handle remoto");
        ask(Operation::READ);
        require(reply.value == 777, "write-back durante shutdown");
        auto quit = Operation::QUIT;
        require(transfer(commands[1], &quit, sizeof(quit), true), "terminar dueno");
        close(commands[1]);
        close(responses[0]);
        int status;
        require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "dueno termino limpio");
        std::cout << "API remota, write-back, timeout y shutdown: OK\n";
    }

    void external_app(const char* executable) {
        Configuration config;
        const pid_t child = fork();
        require(child != -1, "crear app externa");
        if (child == 0) {
            require(freopen("/dev/null", "r", stdin) != nullptr, "stdin de app externa");
            execl(executable, executable, config.path.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        int status;
        require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "app externa funciona solo con dsm.hpp");
    }
}

int main(int argc, char** argv) {
    alarm(35);
    local_lifecycle();
    thread_lifecycle();
    remote_lifecycle();
    if (argc == 2) external_app(argv[1]);
    std::cout << "test_api: OK\n";
    return 0;
}
