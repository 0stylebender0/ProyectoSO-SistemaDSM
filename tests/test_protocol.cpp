#include "protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FALLO: " << message << "\n";
        ++failures;
    }
}

MessageHeader sample_header(uint16_t type, uint32_t payload_size) {
    MessageHeader header{};
    header.magic = DSM_MAGIC;
    header.version = DSM_PROTOCOL_VERSION;
    header.type = type;
    header.payload_size = payload_size;
    header.message_id = 0x0102030405060708ULL;
    header.sender_id = 0x1112131415161718ULL;
    header.receiver_id = 0x2122232425262728ULL;
    header.flags = 0xAABBCCDDu;
    header.reserved = 0;
    return header;
}

}  // namespace

int main() {
    std::string error;

    // 1. Ida y vuelta de la cabecera
    const MessageHeader original =
        sample_header(static_cast<uint16_t>(MessageType::PAGE_REQUEST), 4096);

    const auto bytes = serialize_header(original);
    check(bytes.size() == DSM_HEADER_SIZE, "tamano de cabecera");

    MessageHeader decoded{};
    const bool header_ok =
        deserialize_header(bytes.data(), bytes.size(), decoded, error);
    check(header_ok, "deserializar cabecera valida: " + error);

    check(decoded.magic == original.magic, "campo magic");
    check(decoded.version == original.version, "campo version");
    check(decoded.type == original.type, "campo type");
    check(decoded.payload_size == original.payload_size, "campo payload_size");
    check(decoded.message_id == original.message_id, "campo message_id");
    check(decoded.sender_id == original.sender_id, "campo sender_id");
    check(decoded.receiver_id == original.receiver_id, "campo receiver_id");
    check(decoded.flags == original.flags, "campo flags");

    // 2. Orden de bytes de red (big-endian)
    check(bytes[0] == 0x44 && bytes[1] == 0x53 &&
          bytes[2] == 0x4D && bytes[3] == 0x31,
          "magic en big-endian");

    check(bytes[8] == 0x00 && bytes[9] == 0x00 &&
          bytes[10] == 0x10 && bytes[11] == 0x00,
          "payload_size 4096 en big-endian");

    // 3. Mensaje completo sobre un socket
    int fds[2] = {-1, -1};
    const bool pair_ok = socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
    check(pair_ok, "crear socketpair");

    if (pair_ok) {
        std::vector<uint8_t> payload;
        payload.reserve(4096);
        for (int i = 0; i < 4096; ++i) {
            payload.push_back(static_cast<uint8_t>(i & 0xFF));
        }

        const MessageHeader outgoing =
            sample_header(static_cast<uint16_t>(MessageType::PAGE_DATA), 0);

        const bool sent = send_message(fds[0], outgoing, payload, error);
        check(sent, "send_message: " + error);

        MessageHeader incoming{};
        std::vector<uint8_t> incoming_payload;

        const bool received =
            recv_message(fds[1], incoming, incoming_payload, error);
        check(received, "recv_message: " + error);

        check(incoming.type == outgoing.type, "tipo recibido");
        check(incoming.payload_size == payload.size(), "payload_size recibido");
        check(incoming_payload == payload, "contenido recibido");

        // 4. Payload vacio
        const MessageHeader empty =
            sample_header(static_cast<uint16_t>(MessageType::STATUS_REQUEST), 0);

        const bool empty_sent = send_message(fds[0], empty, {}, error);
        check(empty_sent, "send_message vacio: " + error);

        MessageHeader empty_incoming{};
        std::vector<uint8_t> empty_payload;

        const bool empty_received =
            recv_message(fds[1], empty_incoming, empty_payload, error);
        check(empty_received, "recv_message vacio: " + error);
        check(empty_incoming.payload_size == 0, "payload_size vacio");
        check(empty_payload.empty(), "payload vacio");

        close(fds[0]);
        close(fds[1]);
    }

    // 5. Rechazos
    std::vector<uint8_t> bad_magic(DSM_HEADER_SIZE, 0);
    MessageHeader rejected{};

    check(!deserialize_header(bad_magic.data(), bad_magic.size(), rejected, error),
          "rechazar magic invalido");

    check(!deserialize_header(bytes.data(), DSM_HEADER_SIZE - 1, rejected, error),
          "rechazar cabecera incompleta");

    const auto huge = serialize_header(
        sample_header(
            static_cast<uint16_t>(MessageType::PAGE_DATA),
            DSM_MAX_PAYLOAD_SIZE + 1
        )
    );

    check(!deserialize_header(huge.data(), huge.size(), rejected, error),
          "rechazar payload excesivo");

    if (failures == 0) {
        std::cout << "test_protocol: OK\n";
        return 0;
    }

    std::cerr << "test_protocol: " << failures << " fallo(s)\n";
    return 1;
}
