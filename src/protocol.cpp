#include "protocol.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace {
    void write_u16(uint8_t* out, uint16_t value) {
        out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(value & 0xFF);
    }

    void write_u32(uint8_t* out, uint32_t value) {
        out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
        out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        out[3] = static_cast<uint8_t>(value & 0xFF);
    }

    void write_u64(uint8_t* out, uint64_t value) {
        out[0] = static_cast<uint8_t>((value >> 56) & 0xFF);
        out[1] = static_cast<uint8_t>((value >> 48) & 0xFF);
        out[2] = static_cast<uint8_t>((value >> 40) & 0xFF);
        out[3] = static_cast<uint8_t>((value >> 32) & 0xFF);
        out[4] = static_cast<uint8_t>((value >> 24) & 0xFF);
        out[5] = static_cast<uint8_t>((value >> 16) & 0xFF);
        out[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
        out[7] = static_cast<uint8_t>(value & 0xFF);
    }

    uint16_t read_u16(const uint8_t* in) {
        return static_cast<uint16_t>(
            (static_cast<uint16_t>(in[0]) << 8) |
            static_cast<uint16_t>(in[1])
        );
    }

    uint32_t read_u32(const uint8_t* in) {
        return (static_cast<uint32_t>(in[0]) << 24) |
               (static_cast<uint32_t>(in[1]) << 16) |
               (static_cast<uint32_t>(in[2]) << 8) |
               static_cast<uint32_t>(in[3]);
    }

    uint64_t read_u64(const uint8_t* in) {
        return (static_cast<uint64_t>(in[0]) << 56) |
               (static_cast<uint64_t>(in[1]) << 48) |
               (static_cast<uint64_t>(in[2]) << 40) |
               (static_cast<uint64_t>(in[3]) << 32) |
               (static_cast<uint64_t>(in[4]) << 24) |
               (static_cast<uint64_t>(in[5]) << 16) |
               (static_cast<uint64_t>(in[6]) << 8) |
               static_cast<uint64_t>(in[7]);
    }
}  // namespace

std::array<uint8_t, DSM_HEADER_SIZE> serialize_header(const MessageHeader& header) {
    std::array<uint8_t, DSM_HEADER_SIZE> buffer{};

    write_u32(buffer.data() + 0, header.magic);
    write_u16(buffer.data() + 4, header.version);
    write_u16(buffer.data() + 6, header.type);
    write_u32(buffer.data() + 8, header.payload_size);
    write_u64(buffer.data() + 12, header.message_id);
    write_u64(buffer.data() + 20, header.sender_id);
    write_u64(buffer.data() + 28, header.receiver_id);
    write_u32(buffer.data() + 36, header.flags);
    write_u32(buffer.data() + 40, header.reserved);

    return buffer;
}

bool deserialize_header(
    const uint8_t* data,
    std::size_t size,
    MessageHeader& out,
    std::string& error
) {
    if (data == nullptr || size < DSM_HEADER_SIZE) {
        error = "cabecera incompleta";
        return false;
    }

    out.magic = read_u32(data + 0);
    out.version = read_u16(data + 4);
    out.type = read_u16(data + 6);
    out.payload_size = read_u32(data + 8);
    out.message_id = read_u64(data + 12);
    out.sender_id = read_u64(data + 20);
    out.receiver_id = read_u64(data + 28);
    out.flags = read_u32(data + 36);
    out.reserved = read_u32(data + 40);

    if (out.magic != DSM_MAGIC) {
        error = "magic invalido: no es un mensaje DSM";
        return false;
    }

    if (out.version != DSM_PROTOCOL_VERSION) {
        error = "version de protocolo incompatible";
        return false;
    }

    if (out.payload_size > DSM_MAX_PAYLOAD_SIZE) {
        error = "payload_size excede el maximo permitido";
        return false;
    }

    return true;
}

bool send_all(int fd, const void* data, std::size_t length, std::string& error) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    std::size_t sent = 0;

    while (sent < length) {
        const ssize_t n = ::send(
            fd,
            bytes + sent,
            length - sent,
            MSG_NOSIGNAL
        );

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("send fallo: ") + std::strerror(errno);
            return false;
        }

        if (n == 0) {
            error = "send devolvio 0";
            return false;
        }

        sent += static_cast<std::size_t>(n);
    }

    return true;
}

bool recv_all(int fd, void* data, std::size_t length, std::string& error) {
    uint8_t* bytes = static_cast<uint8_t*>(data);
    std::size_t received = 0;

    while (received < length) {
        const ssize_t n = ::recv(fd, bytes + received, length - received, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("recv fallo: ") + std::strerror(errno);
            return false;
        }

        if (n == 0) {
            error = "conexion cerrada por el otro extremo";
            return false;
        }

        received += static_cast<std::size_t>(n);
    }

    return true;
}

bool send_message(
    int fd,
    const MessageHeader& header,
    const std::vector<uint8_t>& payload,
    std::string& error
) {
    if (payload.size() > DSM_MAX_PAYLOAD_SIZE) {
        error = "payload excede el maximo permitido";
        return false;
    }

    MessageHeader wire = header;
    wire.payload_size = static_cast<uint32_t>(payload.size());

    const auto header_bytes = serialize_header(wire);

    if (!send_all(fd, header_bytes.data(), header_bytes.size(), error)) {
        return false;
    }

    if (!payload.empty() &&
        !send_all(fd, payload.data(), payload.size(), error)) {
        return false;
    }

    return true;
}

bool recv_message(
    int fd,
    MessageHeader& header,
    std::vector<uint8_t>& payload,
    std::string& error
) {
    std::array<uint8_t, DSM_HEADER_SIZE> header_bytes{};

    if (!recv_all(fd, header_bytes.data(), header_bytes.size(), error)) {
        return false;
    }

    if (!deserialize_header(
            header_bytes.data(),
            header_bytes.size(),
            header,
            error)) {
        return false;
    }

    payload.resize(header.payload_size);

    if (header.payload_size > 0 &&
        !recv_all(fd, payload.data(), payload.size(), error)) {
        return false;
    }

    return true;
}
