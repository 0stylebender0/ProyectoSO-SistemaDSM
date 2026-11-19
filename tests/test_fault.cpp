#include "signal_handler.hpp"

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
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

    void read_then_write() {
        const uint32_t page_size = static_cast<uint32_t>(sysconf(_SC_PAGESIZE)) * 2;
        MemoryManager memory(page_size * 3, page_size);
        std::string error;
        require(memory.page_address(0) == nullptr, "pagina sin reserva");
        require(!memory.protect_page(0, PROT_READ, error), "proteger sin reserva");
        require(memory.reserve(error), error);
        require(!memory.reserve(error), "rechazar una segunda reserva");
        require(memory.page_address(3) == nullptr, "limite superior de pagina");
        require(!memory.protect_page(UINT64_MAX, PROT_READ, error), "rechazar pagina invalida");
        require(memory.page_id_from_address(nullptr) == UINT64_MAX, "direccion externa");
        auto* base = static_cast<char*>(memory.base());
        require(memory.page_id_from_address(base + memory.size()) == UINT64_MAX,
                "limite superior de direccion");
        require(memory.page_id_from_address(base + page_size * 2 - 1) == 1,
                "calcular pagina en su ultimo byte");

        sigset_t original_mask;
        pthread_sigmask(SIG_SETMASK, nullptr, &original_mask);
        stack_t original_stack{};
        sigaltstack(nullptr, &original_stack);
        struct sigaction original_segv{}, original_usr1{};
        sigaction(SIGSEGV, nullptr, &original_segv);
        sigaction(SIGUSR1, nullptr, &original_usr1);
        SignalHandler faults(memory);
        require(faults.start(error), error);
        sigset_t mask;
        pthread_sigmask(SIG_SETMASK, nullptr, &mask);
        require(sigismember(&mask, SIGUSR1) == 1, "SIGUSR1 bloqueado en ejecucion normal");
        stack_t stack{};
        sigaltstack(nullptr, &stack);
        require(!(stack.ss_flags & SS_DISABLE) && stack.ss_sp != original_stack.ss_sp,
                "pila alternativa instalada");
        SignalHandler duplicate(memory);
        require(!duplicate.start(error), "un solo handler DSM por proceso");

        auto* page = static_cast<volatile unsigned char*>(memory.page_address(1));
        errno = E2BIG;
        const unsigned char initial = page[page_size - 1];
        require(initial == 0, "lectura inicial devuelve cero");
        require(errno == E2BIG, "el handler conserva errno");
        require(permissions_at(memory.page_address(1)) == "r--p", "lectura habilita solo READ");
        require(permissions_at(memory.page_address(0)) == "---p", "otra pagina sigue PROT_NONE");
        page[page_size - 1] = 73;
        require(page[page_size - 1] == 73, "escritura despues de lectura");
        require(permissions_at(memory.page_address(1)) == "rw-p", "escritura habilita READ|WRITE");
        auto* other = static_cast<volatile unsigned char*>(memory.page_address(2));
        other[7] = 42;
        require(other[7] == 42, "escritura directa sobre PROT_NONE");

        faults.stop();
        faults.stop();
        pthread_sigmask(SIG_SETMASK, nullptr, &mask);
        require(sigismember(&mask, SIGUSR1) == sigismember(&original_mask, SIGUSR1),
                "restaurar mascara de senales");
        sigaltstack(nullptr, &stack);
        require(stack.ss_sp == original_stack.ss_sp && stack.ss_flags == original_stack.ss_flags,
                "restaurar pila alternativa");
        struct sigaction restored{};
        sigaction(SIGSEGV, nullptr, &restored);
        require(restored.sa_sigaction == original_segv.sa_sigaction, "restaurar SIGSEGV");
        sigaction(SIGUSR1, nullptr, &restored);
        require(restored.sa_handler == original_usr1.sa_handler, "restaurar SIGUSR1");
        require(faults.start(error), "reiniciar handler: " + error);
        require(memory.protect_page(0, PROT_NONE, error), error);
        require(*static_cast<volatile char*>(memory.page_address(0)) == 0,
                "resolver despues de reiniciar el worker");
    }

    void concurrent_faults(bool same_page) {
        constexpr unsigned count = 8;
        const uint32_t page_size = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
        MemoryManager memory(page_size * count, page_size);
        std::string error;
        require(memory.reserve(error), error);
        SignalHandler faults(memory);
        require(faults.start(error), error);
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([&, i] {
                FaultThread thread;
                std::string thread_error;
                require(thread.prepare(thread_error), thread_error);
                ++ready;
                while (!go.load()) std::this_thread::yield();
                auto* page = static_cast<volatile unsigned char*>(
                    memory.page_address(same_page ? 0 : i));
                require(page[i] == 0, "lectura concurrente devuelve cero");
                page[i] = static_cast<unsigned char>(i + 1);
                require(page[i] == i + 1, "hilo continua tras su fallo de escritura");
            });
        }
        while (ready.load() != count) std::this_thread::yield();
        go = true;
        for (auto& thread : threads) thread.join();
        for (unsigned i = 0; i < count; ++i) {
            auto* page = static_cast<volatile unsigned char*>(memory.page_address(same_page ? 0 : i));
            require(page[i] == i + 1, "conservar todos los valores escritos");
            require(permissions_at(memory.page_address(same_page ? 0 : i)) == "rw-p",
                    "una lectura concurrente no retira WRITE");
        }
    }

    void repeated_faults() {
        constexpr unsigned count = 1024;
        const uint32_t page_size = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
        MemoryManager memory(page_size * count, page_size);
        std::string error;
        require(memory.reserve(error), error);
        SignalHandler faults(memory);
        require(faults.start(error), error);
        for (unsigned i = 0; i < count; ++i) {
            auto* page = static_cast<volatile unsigned char*>(memory.page_address(i));
            require(*page == 0, "lectura repetida");
            *page = 19;
            require(*page == 19, "escritura repetida");
        }
    }

    void external_fault() {
        MemoryManager memory(4096, 4096);
        std::string error;
        require(memory.reserve(error), error);
        SignalHandler faults(memory);
        require(faults.start(error), error);
        void* outside = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        require(outside != MAP_FAILED, "crear pagina ajena al DSM");
        *static_cast<volatile char*>(outside) = 1;
    }

    void unprepared_thread() {
        MemoryManager memory(4096, 4096);
        std::string error;
        require(memory.reserve(error), error);
        SignalHandler faults(memory);
        require(faults.start(error), error);
        std::thread thread([&] { *static_cast<volatile char*>(memory.base()) = 1; });
        thread.join();
    }

    template <typename Function>
    bool run(const char* name, Function function, int expected_signal = 0) {
        const pid_t child = fork();
        if (child == -1) return false;
        if (child == 0) {
            alarm(8);
            function();
            _exit(0);
        }
        int status;
        while (waitpid(child, &status, 0) == -1) {
            if (errno != EINTR) return false;
        }
        const bool ok = expected_signal ? WIFSIGNALED(status) && WTERMSIG(status) == expected_signal :
                                         WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::cout << name << ": " << (ok ? "OK" : "FALLO") << std::endl;
        return ok;
    }
}

int main() {
    const rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    bool ok = run("Lectura, escritura, limites y restauracion", read_then_write);
    ok &= run("Fallos simultaneos en paginas distintas", [] { concurrent_faults(false); });
    ok &= run("Fallos simultaneos en la misma pagina", [] { concurrent_faults(true); });
    ok &= run("Fallos repetidos sin perder despertares", repeated_faults);
    ok &= run("SIGSEGV externo conserva su comportamiento", external_fault, SIGSEGV);
    ok &= run("Un hilo sin pila alternativa no se registra implicitamente", unprepared_thread, SIGSEGV);
    return ok ? 0 : 1;
}
