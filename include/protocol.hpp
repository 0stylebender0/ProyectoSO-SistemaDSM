#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr uint32_t DSM_MAGIC = 0x44534D31u;  // "DSM1" en ASCII
constexpr uint16_t DSM_PROTOCOL_VERSION = 1;
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
};

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

std::array<uint8_t, DSM_HEADER_SIZE> serialize_header(const MessageHeader& header);

bool deserialize_header(
    const uint8_t* data,
    std::size_t size,
    MessageHeader& out,
    std::string& error
);

bool send_all(int fd, const void* data, std::size_t length, std::string& error);
bool recv_all(int fd, void* data, std::size_t length, std::string& error);

bool send_message(
    int fd,
    const MessageHeader& header,
    const std::vector<uint8_t>& payload,
    std::string& error
);

bool recv_message(
    int fd,
    MessageHeader& header,
    std::vector<uint8_t>& payload,
    std::string& error
);
