#pragma once

#include "fault_queue.hpp"

#include <pthread.h>
#include <signal.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using FaultCallback = std::function<bool(PageId, bool, std::string&)>;

// Se prepara y destruye en el mismo hilo que accede a la memoria DSM.
class FaultThread {
public:
    FaultThread() = default;
    ~FaultThread();

    FaultThread(const FaultThread&) = delete;
    FaultThread& operator=(const FaultThread&) = delete;

    bool prepare(std::string& error);
    void reset();

private:
    void* stack_ = nullptr;
    std::size_t stack_size_ = 0;
    stack_t previous_stack_{};
    sigset_t previous_mask_{};
};

class SignalHandler {
public:
    explicit SignalHandler(MemoryManager& memory, FaultCallback callback = {},
                           std::size_t worker_count = 4);
    ~SignalHandler();

    SignalHandler(const SignalHandler&) = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;

    // Reserva SIGUSR1; se inicia antes de crear hilos y se detiene despues de reunirlos.
    // start y stop deben ejecutarse en el mismo hilo.
    bool start(std::string& error);
    void stop();

private:
    static void handle_fault(int signal, siginfo_t* info, void* context);
    static void handle_wakeup(int signal);
    static void* worker_entry(void* context);
    void resolve_faults();

    MemoryManager& memory_;
    FaultCallback callback_;
    FaultQueue queue_;
    FaultThread thread_;
    std::vector<int> permissions_;
    std::unique_ptr<std::mutex[]> permission_mutexes_;
    sigset_t wait_mask_{};
    struct sigaction previous_segv_{};
    struct sigaction previous_usr1_{};
    std::vector<pthread_t> workers_;
    std::size_t worker_count_;
    pid_t pid_ = 0;
    bool segv_installed_ = false;
    bool usr1_installed_ = false;
};
