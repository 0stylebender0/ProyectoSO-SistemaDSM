#include "signal_handler.hpp"

#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace {
    std::atomic<SignalHandler*> active{nullptr};
    static_assert(decltype(active)::is_always_lock_free);
    thread_local volatile sig_atomic_t prepared = 0;
    thread_local volatile sig_atomic_t awakened = 0;
    thread_local FaultAccess* current_access = nullptr;

    std::mutex access_mutex;
    std::unordered_map<pid_t, FaultAccess*> access_by_thread;

    pid_t current_tid() {
        return static_cast<pid_t>(syscall(SYS_gettid));
    }

    FaultAccess* fault_access_for(pid_t tid) {
        std::lock_guard<std::mutex> lock(access_mutex);
        const auto found = access_by_thread.find(tid);
        return found == access_by_thread.end() ? nullptr : found->second;
    }
}

bool FaultAccess::has_failed() const {
    return failed.load(std::memory_order_acquire);
}

const char* FaultAccess::message() const {
    return error;
}

void FaultAccess::fail(const std::string& why) {
    std::snprintf(error, sizeof(error), "%s", why.c_str());
    failed.store(true, std::memory_order_release);
}

bool begin_fault_access(FaultAccess& access) {
    if (current_access != nullptr) return false;
    access.failed.store(false, std::memory_order_relaxed);
    access.error[0] = '\0';
    access.tid = current_tid();
    try {
        std::lock_guard<std::mutex> lock(access_mutex);
        access_by_thread[access.tid] = &access;
    } catch (...) {
        return false;
    }
    current_access = &access;
    return true;
}

void end_fault_access() {
    if (current_access == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(access_mutex);
        access_by_thread.erase(current_access->tid);
    }
    current_access = nullptr;
}

FaultThread::~FaultThread() {
    reset();
}

bool FaultThread::prepare(std::string& error) {
    if (prepared) {
        error = "el hilo ya esta preparado para fallos DSM";
        return false;
    }
    stack_size_ = std::max<std::size_t>(64 * 1024, SIGSTKSZ);
    void* stack = mmap(nullptr, stack_size_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        error = std::string("mmap de pila alternativa fallo: ") + std::strerror(errno);
        return false;
    }
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    const int result = pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask_);
    if (result != 0) {
        error = std::string("no se pudo bloquear SIGUSR1: ") + std::strerror(result);
        munmap(stack, stack_size_);
        return false;
    }
    stack_t alternative{};
    alternative.ss_sp = stack;
    alternative.ss_size = stack_size_;
    if (sigaltstack(&alternative, &previous_stack_) == -1) {
        error = std::string("sigaltstack fallo: ") + std::strerror(errno);
        pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
        munmap(stack, stack_size_);
        return false;
    }
    stack_ = stack;
    prepared = 1;
    return true;
}

void FaultThread::reset() {
    if (stack_ == nullptr) return;
    prepared = 0;
    sigaltstack(&previous_stack_, nullptr);
    munmap(stack_, stack_size_);
    stack_ = nullptr;
    pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
}

SignalHandler::SignalHandler(MemoryManager& memory, FaultCallback callback, std::size_t worker_count)
    : memory_(memory), callback_(std::move(callback)), worker_count_(worker_count) {}

SignalHandler::~SignalHandler() {
    stop();
}

bool SignalHandler::start(std::string& error) {
#if !defined(__linux__) || !defined(__x86_64__)
    error = "la deteccion de escritura requiere Linux x86-64";
    return false;
#endif
    if (active.load() != nullptr) {
        error = "ya hay un handler DSM activo en el proceso";
        return false;
    }
    if (memory_.base() == nullptr) {
        error = "debe reservarse la memoria antes de iniciar el handler";
        return false;
    }
    if (worker_count_ == 0) {
        error = "FaultWorker requiere al menos un hilo";
        return false;
    }
    if (!thread_.prepare(error)) return false;
    if (!queue_.open(error)) {
        thread_.reset();
        return false;
    }
    if (!callback_) {
        permissions_.assign(memory_.page_count(), PROT_NONE);
        permission_mutexes_ = std::make_unique<std::mutex[]>(memory_.page_count());
    }
    pid_ = getpid();
    in_flight_.store(0, std::memory_order_relaxed);
    stopping_.store(false, std::memory_order_release);
    sigfillset(&wait_mask_);
    sigdelset(&wait_mask_, SIGUSR1);
    SignalHandler* expected = nullptr;
    if (!active.compare_exchange_strong(expected, this)) {
        error = "ya hay un handler DSM activo en el proceso";
        stop();
        return false;
    }

    struct sigaction wakeup{};
    wakeup.sa_handler = handle_wakeup;
    wakeup.sa_flags = SA_RESTART | SA_ONSTACK;
    sigemptyset(&wakeup.sa_mask);
    if (sigaction(SIGUSR1, &wakeup, &previous_usr1_) == -1) {
        error = std::string("sigaction SIGUSR1 fallo: ") + std::strerror(errno);
        stop();
        return false;
    }
    usr1_installed_ = true;
    struct sigaction fault{};
    fault.sa_sigaction = handle_fault;
    fault.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&fault.sa_mask);
    sigaddset(&fault.sa_mask, SIGUSR1);
    if (sigaction(SIGSEGV, &fault, &previous_segv_) == -1) {
        error = std::string("sigaction SIGSEGV fallo: ") + std::strerror(errno);
        stop();
        return false;
    }
    segv_installed_ = true;
    for (std::size_t i = 0; i < worker_count_; ++i) {
        pthread_t worker{};
        const int result = pthread_create(&worker, nullptr, worker_entry, this);
        if (result != 0) {
            error = std::string("no se pudo crear FaultWorker: ") + std::strerror(result);
            stop();
            return false;
        }
        workers_.push_back(worker);
    }
    return true;
}

void SignalHandler::stop() {
    stopping_.store(true, std::memory_order_release);
    while (in_flight_.load(std::memory_order_acquire) != 0) sched_yield();
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        if (!queue_.push(FaultEvent{})) {
            std::cerr << "Error deteniendo FaultWorker: no se pudo encolar la senal de salida\n";
        }
    }
    for (pthread_t worker : workers_) pthread_join(worker, nullptr);
    workers_.clear();
    if (segv_installed_) sigaction(SIGSEGV, &previous_segv_, nullptr);
    if (usr1_installed_) sigaction(SIGUSR1, &previous_usr1_, nullptr);
    segv_installed_ = false;
    usr1_installed_ = false;
    SignalHandler* expected = this;
    active.compare_exchange_strong(expected, nullptr);
    queue_.close();
    thread_.reset();
    stopping_.store(false, std::memory_order_release);
    permissions_.clear();
    permission_mutexes_.reset();
}

void SignalHandler::handle_wakeup(int) {
    awakened = 1;
}

void SignalHandler::handle_fault(int, siginfo_t* info, void* context) {
    const int saved_errno = errno;
    SignalHandler* self = active.load();
    const PageId page_id = self->memory_.page_id_from_address(info->si_addr);
#if defined(__x86_64__)
    const auto access = static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_ERR];
#else
    const unsigned access = 0;
#endif
    if (!prepared || self->stopping_.load(std::memory_order_acquire) ||
        page_id == UINT64_MAX || info->si_code != SEGV_ACCERR || (access & 16)) {
        // Un fallo ajeno al DSM conserva el comportamiento previo del proceso.
        sigaction(SIGSEGV, &self->previous_segv_, nullptr);
        if (info->si_code <= 0) syscall(SYS_tgkill, self->pid_, syscall(SYS_gettid), SIGSEGV);
        errno = saved_errno;
        return;
    }
    FaultEvent event{};
    event.tid = current_tid();
    event.page_id = page_id;
    event.address = reinterpret_cast<uintptr_t>(info->si_addr);
    event.write = (access & 2) != 0;
    self->in_flight_.fetch_add(1, std::memory_order_relaxed);
    awakened = 0;
    if (!self->queue_.push(event)) {
        self->in_flight_.fetch_sub(1, std::memory_order_relaxed);
        sigaction(SIGSEGV, &self->previous_segv_, nullptr);
        syscall(SYS_tgkill, self->pid_, event.tid, SIGSEGV);
        return;
    }
    // SIGUSR1 queda pendiente si el worker responde antes de esta espera.
    while (!awakened) sigsuspend(&self->wait_mask_);
    if (current_access != nullptr && current_access->has_failed()) {
        siglongjmp(current_access->jump, 1);
    }
    errno = saved_errno;
}

void* SignalHandler::worker_entry(void* context) {
    static_cast<SignalHandler*>(context)->resolve_faults();
    return nullptr;
}

void SignalHandler::resolve_faults() {
    FaultEvent event;
    while (queue_.pop(event)) {
        if (event.tid == 0) return;
        std::string error;
        bool resolved = false;
        if (callback_) {
            resolved = callback_(event.page_id, event.write, error);
        } else {
            std::lock_guard<std::mutex> lock(permission_mutexes_[event.page_id]);
            int& permissions = permissions_[event.page_id];
            // Una lectura pendiente no debe quitar escritura habilitada por otro fallo.
            permissions |= PROT_READ | (event.write ? PROT_WRITE : 0);
            resolved = memory_.protect_page(event.page_id, permissions, error);
        }
        if (!resolved) {
            std::cerr << "Error resolviendo fallo DSM: " << error << "\n";
            if (FaultAccess* access = fault_access_for(event.tid)) access->fail(error);
        }
        if (syscall(SYS_tgkill, pid_, event.tid, SIGUSR1) == -1) {
            const std::string wake_error = std::strerror(errno);
            std::cerr << "Error despertando hilo DSM: " << wake_error << "\n";
            if (FaultAccess* access = fault_access_for(event.tid)) access->fail(wake_error);
        }
        in_flight_.fetch_sub(1, std::memory_order_release);
    }
    std::cerr << "Error leyendo la cola de fallos DSM\n";
}
