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

void put_u32(std::vector<uint8_t>& data, std::size_t offset, uint32_t value) {
    data[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    data[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

void put_u64(std::vector<uint8_t>& data, std::size_t offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        data[offset + static_cast<std::size_t>(i)] =
            static_cast<uint8_t>((value >> (56 - 8 * i)) & 0xFF);
    }
}

void region_messages() {
    std::string error;
    RegionInfoRequest request;
    request.region_id = 1234;
    std::vector<uint8_t> bytes;
    check(serialize_region_info_request(request, bytes, error), "serializar REGION_INFO_REQUEST");
    RegionInfoRequest decoded_request;
    check(deserialize_region_info_request(bytes, decoded_request, error) &&
          decoded_request.region_id == request.region_id, "roundtrip REGION_INFO_REQUEST");
    check(!serialize_region_info_request(RegionInfoRequest{}, bytes, error),
          "REGION_INFO_REQUEST sin region");
    for (std::size_t size : {std::size_t{0}, std::size_t{7}, std::size_t{9}}) {
        check(!deserialize_region_info_request(std::vector<uint8_t>(size), decoded_request, error),
              "REGION_INFO_REQUEST de tamano invalido");
    }
    put_u64(bytes, 0, 0);
    check(!deserialize_region_info_request(bytes, decoded_request, error),
          "REGION_INFO_REQUEST con region cero");

    RegionInfoResponse response;
    response.region_id = 10;
    response.owner_id = 20;
    response.size = 8192;
    response.page_size = 4096;
    check(serialize_region_info_response(response, bytes, error), "serializar REGION_INFO_RESPONSE");
    RegionInfoResponse decoded_response;
    check(deserialize_region_info_response(bytes, decoded_response, error) &&
          decoded_response.region_id == response.region_id &&
          decoded_response.owner_id == response.owner_id &&
          decoded_response.size == response.size &&
          decoded_response.page_size == response.page_size,
          "roundtrip REGION_INFO_RESPONSE");

    RegionInfoResponse invalid = response;
    invalid.region_id = 0;
    check(!serialize_region_info_response(invalid, bytes, error), "respuesta sin region");
    invalid = response; invalid.owner_id = 0;
    check(!serialize_region_info_response(invalid, bytes, error), "respuesta sin dueno");
    invalid = response; invalid.size = 0;
    check(!serialize_region_info_response(invalid, bytes, error), "respuesta sin tamano");
    invalid = response; invalid.page_size = 0;
    check(!serialize_region_info_response(invalid, bytes, error), "respuesta sin page_size");
    invalid = response; invalid.size = 8191;
    check(!serialize_region_info_response(invalid, bytes, error), "respuesta no multiplo de pagina");
    invalid = response; invalid.page_size = 3000;
    check(!serialize_region_info_response(invalid, bytes, error), "respuesta con page_size no potencia de dos");

    check(serialize_region_info_response(response, bytes, error), error);
    put_u64(bytes, 0, 0);
    check(!deserialize_region_info_response(bytes, decoded_response, error), "deserializar region cero");
    check(serialize_region_info_response(response, bytes, error), error);
    put_u64(bytes, 8, 0);
    check(!deserialize_region_info_response(bytes, decoded_response, error), "deserializar dueno cero");
    check(serialize_region_info_response(response, bytes, error), error);
    put_u64(bytes, 16, 0);
    check(!deserialize_region_info_response(bytes, decoded_response, error), "deserializar tamano cero");
    check(serialize_region_info_response(response, bytes, error), error);
    put_u32(bytes, 24, 0);
    check(!deserialize_region_info_response(bytes, decoded_response, error), "deserializar page_size cero");
    check(serialize_region_info_response(response, bytes, error), error);
    put_u64(bytes, 16, 7);
    check(!deserialize_region_info_response(bytes, decoded_response, error), "deserializar tamano no multiplo");
    check(serialize_region_info_response(response, bytes, error), error);
    put_u32(bytes, 24, 3000);
    check(!deserialize_region_info_response(bytes, decoded_response, error), "deserializar page_size invalido");
    for (std::size_t size : {std::size_t{0}, std::size_t{27}, std::size_t{29}}) {
        check(!deserialize_region_info_response(std::vector<uint8_t>(size), decoded_response, error),
              "REGION_INFO_RESPONSE de tamano invalido");
    }

    RegionDescriptor first{1, 10, 4096, 4096};
    RegionDescriptor second{2, 11, 8192, 4096};
    std::vector<RegionDescriptor> regions{first, second};
    check(serialize_regions(regions, bytes, error), "serializar lista de regiones");
    std::vector<RegionDescriptor> decoded_regions;
    check(deserialize_regions(bytes, decoded_regions, error) && decoded_regions.size() == 2 &&
          decoded_regions[0].region_id == 1 && decoded_regions[1].region_id == 2,
          "roundtrip de lista de regiones");

    RegionDescriptor invalid_region = first;
    invalid_region.region_id = 0;
    check(!serialize_regions({invalid_region}, bytes, error), "region sin id");
    invalid_region = first; invalid_region.owner_id = 0;
    check(!serialize_regions({invalid_region}, bytes, error), "region sin dueno");
    invalid_region = first; invalid_region.size = 0;
    check(!serialize_regions({invalid_region}, bytes, error), "region sin tamano");
    invalid_region = first; invalid_region.page_size = 0;
    check(!serialize_regions({invalid_region}, bytes, error), "region sin page_size");
    invalid_region = first; invalid_region.size = 4097;
    check(!serialize_regions({invalid_region}, bytes, error), "region no multiplo de pagina");
    invalid_region = first; invalid_region.page_size = 3000;
    check(!serialize_regions({invalid_region}, bytes, error), "region con page_size invalido");

    check(serialize_regions({first, first}, bytes, error), error);
    check(!deserialize_regions(bytes, decoded_regions, error),
          "rechazar RegionId repetido en la lista");
    check(serialize_regions(regions, bytes, error), error);
    bytes.push_back(0);
    check(!deserialize_regions(bytes, decoded_regions, error), "rechazar bytes sobrantes");
    check(!deserialize_regions(std::vector<uint8_t>(4, 1), decoded_regions, error),
          "rechazar cantidad de regiones inconsistente");
    check(!deserialize_regions(std::vector<uint8_t>(3), decoded_regions, error),
          "rechazar lista de regiones incompleta");
    check(serialize_regions({first}, bytes, error), error);
    put_u64(bytes, 4 + 16, 0);
    check(!deserialize_regions(bytes, decoded_regions, error),
          "rechazar descriptor invalido en la lista");
}

void page_and_invalidate_messages() {
    std::string error;
    PageRequest request;
    request.page_id = 7;
    request.region_id = 8;
    request.region_offset = 1;
    request.access_type = 1;
    request.current_state = static_cast<uint8_t>(MSIState::SHARED);
    request.known_version = 3;
    std::vector<uint8_t> bytes;
    check(serialize_page_request(request, bytes, error), "serializar PAGE_REQUEST");
    PageRequest decoded_request;
    check(deserialize_page_request(bytes, decoded_request, error) &&
          decoded_request.page_id == request.page_id &&
          decoded_request.region_id == request.region_id &&
          decoded_request.region_offset == request.region_offset &&
          decoded_request.access_type == request.access_type &&
          decoded_request.current_state == request.current_state &&
          decoded_request.known_version == request.known_version,
          "roundtrip PAGE_REQUEST");

    PageRequest invalid = request;
    invalid.access_type = 2;
    check(!serialize_page_request(invalid, bytes, error), "PAGE_REQUEST con access_type invalido");
    invalid = request; invalid.current_state = 3;
    check(!serialize_page_request(invalid, bytes, error), "PAGE_REQUEST con estado invalido");
    check(serialize_page_request(request, bytes, error), error);
    bytes[24] = 2;
    check(!deserialize_page_request(bytes, decoded_request, error), "deserializar access_type invalido");
    check(serialize_page_request(request, bytes, error), error);
    bytes[25] = 3;
    check(!deserialize_page_request(bytes, decoded_request, error), "deserializar estado invalido");
    for (std::size_t size : {std::size_t{0}, std::size_t{33}, std::size_t{35}}) {
        check(!deserialize_page_request(std::vector<uint8_t>(size), decoded_request, error),
              "PAGE_REQUEST de tamano invalido");
    }

    PageData page;
    page.page_id = 7;
    page.region_id = 8;
    page.region_offset = 1;
    page.version = 4;
    page.state = static_cast<uint8_t>(MSIState::SHARED);
    page.data.assign(16, 0x5A);
    page.data_size = 16;
    check(serialize_page_data(page, bytes, error), "serializar PAGE_DATA");
    PageData decoded_page;
    check(deserialize_page_data(bytes, decoded_page, error) &&
          decoded_page.page_id == page.page_id && decoded_page.region_id == page.region_id &&
          decoded_page.region_offset == page.region_offset && decoded_page.version == page.version &&
          decoded_page.state == page.state && decoded_page.data == page.data,
          "roundtrip PAGE_DATA");
    PageData invalid_page = PageData{};
    invalid_page.state = 3;
    check(!serialize_page_data(invalid_page, bytes, error), "PAGE_DATA con estado invalido");
    check(serialize_page_data(page, bytes, error), error);
    put_u32(bytes, 33, 15);
    check(!deserialize_page_data(bytes, decoded_page, error), "PAGE_DATA con data_size inconsistente");
    check(serialize_page_data(page, bytes, error), error);
    bytes[32] = 3;
    check(!deserialize_page_data(bytes, decoded_page, error), "PAGE_DATA con estado invalido");
    check(!deserialize_page_data(std::vector<uint8_t>(36), decoded_page, error),
          "PAGE_DATA incompleto");
    check(serialize_page_data(page, bytes, error), error);
    bytes.push_back(0);
    check(!deserialize_page_data(bytes, decoded_page, error), "PAGE_DATA con bytes sobrantes");

    InvalidateMessage message;
    message.page_id = 7;
    message.region_id = 8;
    message.region_offset = 1;
    message.new_version = 5;
    message.new_owner = 9;
    message.target_state = static_cast<uint8_t>(MSIState::INVALID);
    check(serialize_invalidate(message, bytes, error), "serializar INVALIDATE");
    InvalidateMessage decoded_message;
    check(deserialize_invalidate(bytes, decoded_message, error) &&
          decoded_message.page_id == message.page_id && decoded_message.region_id == message.region_id &&
          decoded_message.region_offset == message.region_offset &&
          decoded_message.new_version == message.new_version &&
          decoded_message.new_owner == message.new_owner &&
          decoded_message.target_state == message.target_state,
          "roundtrip INVALIDATE");
    InvalidateMessage invalid_message = message;
    invalid_message.region_id = 0;
    check(!serialize_invalidate(invalid_message, bytes, error), "INVALIDATE sin region");
    invalid_message = message; invalid_message.new_owner = 0;
    check(!serialize_invalidate(invalid_message, bytes, error), "INVALIDATE sin destinatario");
    invalid_message = message; invalid_message.target_state = static_cast<uint8_t>(MSIState::MODIFIED);
    check(!serialize_invalidate(invalid_message, bytes, error), "INVALIDATE con estado invalido");
    check(serialize_invalidate(message, bytes, error), error);
    put_u64(bytes, 8, 0);
    check(!deserialize_invalidate(bytes, decoded_message, error), "deserializar INVALIDATE sin region");
    check(serialize_invalidate(message, bytes, error), error);
    put_u64(bytes, 32, 0);
    check(!deserialize_invalidate(bytes, decoded_message, error), "deserializar INVALIDATE sin destinatario");
    check(serialize_invalidate(message, bytes, error), error);
    bytes[40] = static_cast<uint8_t>(MSIState::MODIFIED);
    check(!deserialize_invalidate(bytes, decoded_message, error), "deserializar INVALIDATE con estado invalido");
    for (std::size_t size : {std::size_t{0}, std::size_t{40}, std::size_t{42}}) {
        check(!deserialize_invalidate(std::vector<uint8_t>(size), decoded_message, error),
              "INVALIDATE de tamano invalido");
    }
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

    region_messages();
    page_and_invalidate_messages();

    if (failures == 0) {
        std::cout << "test_protocol: OK\n";
        return 0;
    }

    std::cerr << "test_protocol: " << failures << " fallo(s)\n";
    return 1;
}
