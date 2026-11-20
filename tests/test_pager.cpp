#include "pager.hpp"
#include "signal_handler.hpp"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
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

    std::string permissions_at(void* address) {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        const auto value = reinterpret_cast<uintptr_t>(address);
        while (std::getline(maps, line)) {
            unsigned long begin, end;
            char permissions[5]{};
            if (std::sscanf(line.c_str(), "%lx-%lx %4s", &begin, &end, permissions) == 3 &&
                value >= begin && value < end) return permissions;
        }
        return {};
    }

    uint32_t page_size() {
        return static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
    }

    void region_layout() {
        const uint32_t page = page_size();
        MemoryManager memory(page * 8, page);
        std::string error;
        require(memory.reserve(error), error);

        Pager pager(memory, 42);

        RegionId first = 0;
        require(pager.create_region(page * 3, first, error), error);
        require(first != 0, "el RegionId no puede ser cero");

        RegionInfo info;
        require(pager.region_info(first, info), "la region debe quedar registrada");
        require(info.descriptor.owner_id == 42, "el creador es el dueno");
        require(info.first_page == 0, "la primera region inicia en la pagina 0");
        require(info.page_count == 3, "la region debe tener tres paginas");
        require(info.descriptor.size == page * 3, "tamano de region registrado");

        PageInfo page_zero;
        require(pager.page_info(0, page_zero), "la pagina 0 debe estar registrada");
        require(page_zero.region_id == first && page_zero.owner_id == 42,
                "metadatos de la pagina 0");
        require(page_zero.state == MSIState::INVALID && !page_zero.locally_present &&
                page_zero.version == 0, "estado inicial INVALID sin materializar");

        require(memory.page_id_from_address(memory.page_address(0)) == 0, "page id 0");
        require(memory.page_id_from_address(memory.page_address(2)) == 2, "page id 2");

        PageAssignment assignment;
        require(pager.assignment(2, assignment), "asignacion de la pagina 2");
        require(assignment.region_id == first && assignment.owner_id == 42 &&
                assignment.region_offset == 2, "region_offset de la pagina 2");

        require(permissions_at(memory.page_address(0)) == "---p",
                "las paginas sin tocar siguen PROT_NONE");

        RegionId second = 0;
        require(pager.create_region(page * 2, second, error), error);
        require(second != first, "dos regiones deben tener ids distintos");
        require(pager.region_info(second, info) && info.first_page == 3,
                "la segunda region es contigua");

        RegionId overflow = 0;
        require(!pager.create_region(page * 4, overflow, error),
                "rechazar una region sin espacio");
        require(!pager.create_region(0, overflow, error), "rechazar una region vacia");

        RegionDescriptor wrong = {second, 7, page * 2, page * 2};
        require(!pager.attach_region(wrong, error), "rechazar un page_size distinto");

        RegionDescriptor remote = {9001, 99, page * 2, page};
        require(pager.attach_region(remote, error), "adjuntar una region remota");
        RegionInfo remote_info;
        require(pager.region_info(9001, remote_info), "la region remota queda registrada");
        require(remote_info.first_page == 5 && remote_info.descriptor.owner_id == 99,
                "rango local de la region remota");

        PageInfo remote_page;
        require(pager.page_info(5, remote_page), "la pagina remota queda registrada");
        require(remote_page.owner_id == 99 && remote_page.state == MSIState::INVALID &&
                !remote_page.locally_present, "la pagina remota inicia INVALID");

        require(!pager.resolve_fault(5, false, error),
                "2.3 no resuelve paginas remotas");
        require(permissions_at(memory.page_address(5)) == "---p",
                "la pagina remota sigue PROT_NONE");

        require(pager.attach_region(remote, error), "adjuntar la misma region es idempotente");
    }

    void msi_local() {
        const uint32_t page = page_size();
        MemoryManager memory(page * 3, page);
        std::string error;
        require(memory.reserve(error), error);

        Pager pager(memory, 7);
        RegionId region = 0;
        require(pager.create_region(page * 3, region, error), error);

        SignalHandler faults(memory,
            [&pager](PageId page_id, bool write, std::string& callback_error) {
                return pager.resolve_fault(page_id, write, callback_error);
            });
        require(faults.start(error), error);

        auto* page_zero = static_cast<volatile unsigned char*>(memory.page_address(0));
        require(*page_zero == 0, "la lectura inicial de una pagina del dueno devuelve cero");

        PageInfo info;
        require(pager.page_info(0, info), "pagina 0");
        require(info.state == MSIState::SHARED, "I + lectura deja S");
        require(info.locally_present, "la lectura materializa la pagina");
        require(info.version == 0, "la lectura no cambia la version");
        require(permissions_at(memory.page_address(0)) == "r--p",
                "S habilita solo lectura");

        *page_zero = 73;
        require(*page_zero == 73, "la escritura continua tras el fallo");

        require(pager.page_info(0, info), "pagina 0 tras escritura");
        require(info.state == MSIState::MODIFIED, "S + escritura deja M");
        require(info.version == 1, "la primera escritura incrementa la version");
        require(permissions_at(memory.page_address(0)) == "rw-p",
                "M habilita lectura y escritura");

        require(pager.resolve_fault(0, false, error), error);
        require(pager.page_info(0, info) && info.state == MSIState::MODIFIED,
                "M + lectura sigue en M");
        require(permissions_at(memory.page_address(0)) == "rw-p",
                "M conserva el permiso de escritura");

        auto* page_one = static_cast<volatile unsigned char*>(memory.page_address(1));
        require(*page_one == 0, "lectura de la pagina 1");
        require(pager.page_info(1, info) && info.state == MSIState::SHARED,
                "la pagina 1 queda en S");
        require(permissions_at(memory.page_address(0)) == "rw-p",
                "la pagina 0 conserva M");

        auto* page_two = static_cast<volatile unsigned char*>(memory.page_address(2));
        *page_two = 19;
        require(*page_two == 19, "escritura directa sobre una pagina INVALID");
        require(pager.page_info(2, info) && info.state == MSIState::MODIFIED,
                "I + escritura deja M");
        require(info.version == 1, "la escritura directa inicia la version en 1");
        require(permissions_at(memory.page_address(2)) == "rw-p",
                "la pagina 2 permite escritura");

        faults.stop();
    }

    template <typename Function>
    bool run(const char* name, Function function) {
        const pid_t child = fork();
        if (child == -1) {
            std::cerr << "fork fallo: " << std::strerror(errno) << "\n";
            return false;
        }
        if (child == 0) {
            alarm(8);
            function();
            _exit(0);
        }
        int status = 0;
        while (waitpid(child, &status, 0) == -1) {
            if (errno != EINTR) {
                std::cerr << "waitpid fallo: " << std::strerror(errno) << "\n";
                return false;
            }
        }
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::cout << name << ": " << (ok ? "OK" : "FALLO") << std::endl;
        return ok;
    }
}

int main() {
    const rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);

    bool ok = run("Regiones, asignaciones y adjuntar remoto", region_layout);
    ok &= run("Transiciones MSI locales y permisos", msi_local);
    return ok ? 0 : 1;
}
