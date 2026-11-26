#include "fault_queue.hpp"

#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

static_assert(sizeof(FaultEvent) <= PIPE_BUF, "El evento debe escribirse atomicamente");

FaultQueue::~FaultQueue() {
    close();
}

bool FaultQueue::open(std::string& error) {
    if (read_fd_ != -1) {
        error = "la cola de fallos ya esta abierta";
        return false;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        error = std::string("pipe fallo: ") + std::strerror(errno);
        return false;
    }
    read_fd_ = fds[0];
    write_fd_ = fds[1];
    const int flags = fcntl(write_fd_, F_GETFL);
    if (flags == -1 || fcntl(write_fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
        error = std::string("no se pudo configurar la cola de fallos: ") + std::strerror(errno);
        close();
        return false;
    }
    return true;
}

void FaultQueue::close() {
    if (read_fd_ != -1) ::close(read_fd_);
    if (write_fd_ != -1) ::close(write_fd_);
    read_fd_ = -1;
    write_fd_ = -1;
}

bool FaultQueue::push(const FaultEvent& event) const {
    // Una sola escritura conserva los eventos completos entre varios handlers.
    ssize_t count;
    do {
        count = write(write_fd_, &event, sizeof(event));
        if (count == -1 && errno == EAGAIN) sched_yield();
    } while (count == -1 && (errno == EINTR || errno == EAGAIN));
    return count == sizeof(event);
}

bool FaultQueue::pop(FaultEvent& event) const {
    // Solo los workers toman este lock; el handler sigue usando write sin mutex.
    std::lock_guard<std::mutex> lock(read_mutex_);
    auto* data = reinterpret_cast<char*>(&event);
    std::size_t received = 0;
    while (received < sizeof(event)) {
        const ssize_t count = read(read_fd_, data + received, sizeof(event) - received);
        if (count == -1 && errno == EINTR) continue;
        if (count <= 0) return false;
        received += static_cast<std::size_t>(count);
    }
    return true;
}
