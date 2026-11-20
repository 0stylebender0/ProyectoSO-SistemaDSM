#pragma once

#include "node.hpp"

#include <cstdint>
#include <set>
#include <unordered_map>

using PageId = uint64_t;
using RegionId = uint64_t;

enum class MSIState : uint8_t {
    MODIFIED = 0,
    SHARED   = 1,
    INVALID  = 2
};

inline const char* msi_state_name(MSIState state) {
    switch (state) {
        case MSIState::MODIFIED: return "MODIFIED";
        case MSIState::SHARED:   return "SHARED";
        case MSIState::INVALID:  return "INVALID";
    }
    return "DESCONOCIDO";
}

struct PageInfo {
    PageId page_id = 0;
    RegionId region_id = 0;
    NodeId owner_id = 0;
    MSIState state = MSIState::INVALID;
    uint64_t version = 0;
    bool locally_present = false;
    std::set<NodeId> sharers;
};

struct PageAssignment {
    PageId page_id = 0;
    RegionId region_id = 0;
    NodeId owner_id = 0;
    uint64_t region_offset = 0;
};

struct GlobalPageControl {
    PageId page_id = 0;
    RegionId region_id = 0;
    NodeId owner_id = 0;
    MSIState owner_state = MSIState::INVALID;
    uint64_t version = 0;
    std::set<NodeId> sharers;
    NodeId current_holder = 0;
    bool write_in_progress = false;
};

using LocalPageTable = std::unordered_map<PageId, PageInfo>;

// Descriptor que viajara en REGION_INFO_RESPONSE durante el hito 2.4.
struct RegionDescriptor {
    RegionId region_id = 0;
    NodeId owner_id = 0;
    uint64_t size = 0;
    uint32_t page_size = 0;
};

struct RegionInfo {
    RegionDescriptor descriptor;
    uint64_t first_page = 0;
    uint64_t page_count = 0;
};

using RegionTable = std::unordered_map<RegionId, RegionInfo>;
