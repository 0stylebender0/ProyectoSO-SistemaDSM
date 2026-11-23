#pragma once

#include "page.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr uint32_t DSM_MAGIC = 0x44534D31u;  // "DSM1" en ASCII
constexpr uint16_t DSM_PROTOCOL_VERSION = 4;
constexpr std::size_t DSM_HEADER_SIZE = 44;
constexpr uint32_t DSM_MAX_PAYLOAD_SIZE = 1024u * 1024u;  // 1 MiB

struct MessageHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_size;

    uint64_t message_id;
    uint64_t sender_id;
    uint64_t receiver_id;

    uint32_t flags;
    uint32_t reserved;
};

enum class MessageType : uint16_t {
    JOIN_REQUEST        = 1,
    JOIN_RESPONSE       = 2,
    NODE_ANNOUNCE       = 3,
    NODE_LIST_REQUEST   = 4,
    NODE_LIST_RESPONSE  = 5,
    PAGE_REQUEST        = 6,
    PAGE_DATA           = 7,
    INVALIDATE          = 8,
    INVALIDATE_ACK      = 9,
    PAGE_RELEASE        = 10,
    STATUS_REQUEST      = 11,
    STATUS_RESPONSE     = 12,
    ERROR_RESPONSE      = 13,
    HEARTBEAT           = 14,
    NODE_LEAVE          = 15,
    NODE_LEAVE_ACK      = 16,
    REGION_INFO_REQUEST  = 17,
    REGION_INFO_RESPONSE = 18,
    REGION_LIST_REQUEST  = 19,
    REGION_LIST_RESPONSE = 20,
};

// Banderas opcionales del payload de STATUS_REQUEST. Un payload vacio
// conserva la respuesta basica usada por dsm_status sin opciones.
constexpr uint8_t STATUS_FLAG_HISTORY = 0x01;
constexpr uint8_t STATUS_FLAG_PAGES   = 0x02;
constexpr uint8_t STATUS_FLAG_STATS   = 0x04;
constexpr uint8_t STATUS_FLAG_ALL     =
    STATUS_FLAG_HISTORY | STATUS_FLAG_PAGES | STATUS_FLAG_STATS;

enum class ErrorCode : uint16_t {
    INVALID_MESSAGE    = 1,
    UNKNOWN_NODE       = 2,
    PAGE_NOT_FOUND     = 3,
    ACCESS_DENIED      = 4,
    INVALID_STATE      = 5,
    OUT_OF_MEMORY      = 6,
    NODE_UNAVAILABLE   = 7,
    VERSION_MISMATCH   = 8,
    TIMEOUT            = 9,
    PROTOCOL_MISMATCH  = 10,
};

// En la red la pagina se identifica por (region_id, region_offset).
// page_id correlaciona la respuesta con la pagina local del solicitante.
struct PageRequest {
    PageId page_id = 0;
    RegionId region_id = 0;
    uint64_t region_offset = 0;
    uint8_t access_type = 0;    // 0 = READ, 1 = WRITE
    uint8_t current_state = 2;  // MSIState::INVALID
    uint64_t known_version = 0;
};

struct PageData {
    PageId page_id = 0;
    RegionId region_id = 0;
    uint64_t region_offset = 0;
    uint64_t version = 0;
    uint8_t state = 1;  // MSIState::SHARED
    uint32_t data_size = 0;
    std::vector<uint8_t> data;
};

struct RegionInfoRequest {
    RegionId region_id = 0;
};

struct InvalidateMessage {
    PageId page_id = 0;
    RegionId region_id = 0;
    uint64_t region_offset = 0;
    uint64_t new_version = 0;
    // Cambia el holder, no el dueno permanente de la region.
    NodeId new_owner = 0;
    uint8_t target_state = static_cast<uint8_t>(MSIState::INVALID);
};

struct RegionInfoResponse {
    RegionId region_id = 0;
    NodeId owner_id = 0;
    uint64_t size = 0;
    uint32_t page_size = 0;
};

using IODeadline = std::chrono::steady_clock::time_point;

std::array<uint8_t, DSM_HEADER_SIZE> serialize_header(const MessageHeader& header);

bool deserialize_header(
    const uint8_t* data,
    std::size_t size,
    MessageHeader& out,
    std::string& error
);

bool send_all(int fd, const void* data, std::size_t length, std::string& error,
              IODeadline deadline = IODeadline::max());
bool recv_all(int fd, void* data, std::size_t length, std::string& error,
              IODeadline deadline = IODeadline::max());

bool send_message(
    int fd,
    const MessageHeader& header,
    const std::vector<uint8_t>& payload,
    std::string& error,
    IODeadline deadline = IODeadline::max()
);

bool recv_message(
    int fd,
    MessageHeader& header,
    std::vector<uint8_t>& payload,
    std::string& error,
    IODeadline deadline = IODeadline::max()
);

bool serialize_nodes(const std::vector<NodeInfo>& nodes,
                     std::vector<uint8_t>& out, std::string& error);
bool deserialize_nodes(const std::vector<uint8_t>& data,
                       std::vector<NodeInfo>& out, std::string& error);

bool serialize_page_request(const PageRequest& request,
                            std::vector<uint8_t>& out, std::string& error);
bool deserialize_page_request(const std::vector<uint8_t>& data,
                              PageRequest& out, std::string& error);

bool serialize_page_data(const PageData& page,
                         std::vector<uint8_t>& out, std::string& error);
bool deserialize_page_data(const std::vector<uint8_t>& data,
                           PageData& out, std::string& error);

bool serialize_region_info_request(const RegionInfoRequest& request,
                                   std::vector<uint8_t>& out, std::string& error);
bool deserialize_region_info_request(const std::vector<uint8_t>& data,
                                     RegionInfoRequest& out, std::string& error);

bool serialize_region_info_response(const RegionInfoResponse& response,
                                    std::vector<uint8_t>& out, std::string& error);
bool deserialize_region_info_response(const std::vector<uint8_t>& data,
                                      RegionInfoResponse& out, std::string& error);

bool serialize_regions(const std::vector<RegionDescriptor>& regions,
                       std::vector<uint8_t>& out, std::string& error);
bool deserialize_regions(const std::vector<uint8_t>& data,
                         std::vector<RegionDescriptor>& out, std::string& error);

bool serialize_invalidate(const InvalidateMessage& message,
                          std::vector<uint8_t>& out, std::string& error);
bool deserialize_invalidate(const std::vector<uint8_t>& data,
                            InvalidateMessage& out, std::string& error);
