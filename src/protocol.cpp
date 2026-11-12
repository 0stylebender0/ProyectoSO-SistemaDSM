#include "protocol.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <set>
#include <utility>

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

bool serialize_nodes(const std::vector<NodeInfo>& nodes,
                     std::vector<uint8_t>& out, std::string& error) {
    if (nodes.size() > (DSM_MAX_PAYLOAD_SIZE - 4) / 30) {
        error = "demasiados nodos para un mensaje";
        return false;
    }
    std::vector<uint8_t> bytes(4);
    write_u32(bytes.data(), static_cast<uint32_t>(nodes.size()));
    std::set<NodeId> ids;
    for (const auto& node : nodes) {
        if (!validate_node(node, error)) return false;
        if (!ids.insert(node.id).second) {
            error = "NodeId repetido en el mensaje";
            return false;
        }
        const std::size_t offset = bytes.size();
        if (offset + 30 + node.node_name.size() > DSM_MAX_PAYLOAD_SIZE) {
            error = "lista de nodos excede el maximo permitido";
            return false;
        }
        bytes.resize(offset + 30 + node.node_name.size());
        in_addr address{};
        inet_pton(AF_INET, node.ip_address.c_str(), &address);
        // Cada registro tiene 30 bytes fijos, seguidos por el nombre sin terminador.
        write_u64(bytes.data() + offset, node.id);
        write_u32(bytes.data() + offset + 8, ntohl(address.s_addr));
        write_u16(bytes.data() + offset + 12, node.tcp_port);
        write_u16(bytes.data() + offset + 14, node.heartbeat_port);
        write_u64(bytes.data() + offset + 16, node.memory_size);
        write_u32(bytes.data() + offset + 24, node.page_size);
        write_u16(bytes.data() + offset + 28, static_cast<uint16_t>(node.node_name.size()));
        std::memcpy(bytes.data() + offset + 30, node.node_name.data(), node.node_name.size());
    }
    out = std::move(bytes);
    return true;
}

bool deserialize_nodes(const std::vector<uint8_t>& data,
                       std::vector<NodeInfo>& out, std::string& error) {
    if (data.size() < 4 || data.size() > DSM_MAX_PAYLOAD_SIZE) {
        error = "lista de nodos incompleta o demasiado grande";
        return false;
    }
    const uint32_t count = read_u32(data.data());
    if (count > (data.size() - 4) / 30) {
        error = "cantidad de nodos no coincide con el mensaje";
        return false;
    }
    std::vector<NodeInfo> nodes;
    std::set<NodeId> ids;
    std::size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (data.size() - offset < 30) {
            error = "registro de nodo incompleto";
            return false;
        }
        const uint8_t* record = data.data() + offset;
        NodeInfo node;
        node.id = read_u64(record);
        in_addr address{};
        address.s_addr = htonl(read_u32(record + 8));
        char text[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &address, text, sizeof(text));
        node.ip_address = text;
        node.tcp_port = read_u16(record + 12);
        node.heartbeat_port = read_u16(record + 14);
        node.memory_size = read_u64(record + 16);
        node.page_size = read_u32(record + 24);
        const uint16_t name_size = read_u16(record + 28);
        offset += 30;
        if (name_size > data.size() - offset) {
            error = "nombre de nodo incompleto";
            return false;
        }
        node.node_name.assign(reinterpret_cast<const char*>(data.data() + offset), name_size);
        offset += name_size;
        if (!validate_node(node, error)) return false;
        if (!ids.insert(node.id).second) {
            error = "NodeId repetido en el mensaje";
            return false;
        }
        nodes.push_back(node);
    }
    if (offset != data.size()) {
        error = "bytes sobrantes en lista de nodos";
        return false;
    }
    out = std::move(nodes);
    return true;
}
