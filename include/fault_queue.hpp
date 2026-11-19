#pragma once

#include "memory_manager.hpp"

#include <sys/types.h>

struct FaultEvent {
    pid_t tid = 0;
    PageId page_id = 0;
    uintptr_t address = 0;
    bool write = false;
};

class FaultQueue {
public:
    FaultQueue() = default;
    ~FaultQueue();

    FaultQueue(const FaultQueue&) = delete;
    FaultQueue& operator=(const FaultQueue&) = delete;

    bool open(std::string& error);
    void close();
    bool push(const FaultEvent& event) const;
    bool pop(FaultEvent& event) const;

private:
    int read_fd_ = -1;
    int write_fd_ = -1;
};
