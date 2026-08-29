#include "wss_frame_codec.h"
#include "wss_upgrade.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint8_t key[WSS_CLIENT_KEY_BYTES] = "the sample nonce";
    char client_key[WSS_CLIENT_KEY_TEXT_CAPACITY];
    assert(wss_client_key_encode(key, client_key) == WSS_CODEC_RESULT_OK);
    assert(strcmp(client_key, "dGhlIHNhbXBsZSBub25jZQ==") == 0);

    char accept[WSS_ACCEPT_TEXT_CAPACITY];
    assert(wss_upgrade_expected_accept(client_key, accept) == WSS_CODEC_RESULT_OK);
    assert(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

    uint8_t request[512U];
    size_t request_length = 0U;
    assert(wss_upgrade_build_request(
               "192.168.1.10", 43125U, "/device", client_key, request, sizeof(request),
               &request_length) == WSS_CODEC_RESULT_OK);
    assert(request_length > 0U);
    assert(strstr((char *)request, "GET /device HTTP/1.1\r\n") != NULL);

    const char *response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "upgrade: websocket\r\n"
        "connection: keep-alive, Upgrade\r\n"
        "sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    assert(wss_upgrade_validate_response(
               (const uint8_t *)response, strlen(response), accept) == WSS_CODEC_RESULT_OK);
    assert(wss_upgrade_validate_response(
               (const uint8_t *)response, strlen(response), "invalid") ==
           WSS_CODEC_RESULT_PROTOCOL_ERROR);
    const char *case_changed_accept =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: S3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    assert(wss_upgrade_validate_response(
               (const uint8_t *)case_changed_accept, strlen(case_changed_accept), accept) ==
           WSS_CODEC_RESULT_PROTOCOL_ERROR);
    const char *duplicate_upgrade =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    assert(wss_upgrade_validate_response(
               (const uint8_t *)duplicate_upgrade, strlen(duplicate_upgrade), accept) ==
           WSS_CODEC_RESULT_PROTOCOL_ERROR);

    const uint8_t mask[4] = {0x37U, 0xfaU, 0x21U, 0x3dU};
    const uint8_t hello[] = "Hello";
    const uint8_t expected[] = {
        0x81U, 0x85U, 0x37U, 0xfaU, 0x21U, 0x3dU, 0x7fU, 0x9fU, 0x4dU, 0x51U, 0x58U,
    };
    uint8_t encoded[32U];
    size_t encoded_length = 0U;
    assert(wss_frame_encode_client(
               WSS_OPCODE_TEXT, true, mask, hello, sizeof(hello) - 1U, encoded,
               sizeof(encoded), &encoded_length) == WSS_CODEC_RESULT_OK);
    assert(encoded_length == sizeof(expected));
    assert(memcmp(encoded, expected, sizeof(expected)) == 0);

    const uint8_t server_header[] = {0x82U, 126U, 0x02U, 0x90U};
    wss_frame_header_t header;
    assert(wss_frame_decode_server_header(server_header, sizeof(server_header), &header) ==
           WSS_CODEC_RESULT_OK);
    assert(header.final && header.opcode == WSS_OPCODE_BINARY && header.payload_length == 656U &&
           header.header_length == sizeof(server_header));

    const uint8_t masked_server[] = {0x81U, 0x80U};
    assert(wss_frame_decode_server_header(masked_server, sizeof(masked_server), &header) ==
           WSS_CODEC_RESULT_PROTOCOL_ERROR);
    const uint8_t fragmented_ping[] = {0x09U, 0x00U};
    assert(wss_frame_decode_server_header(fragmented_ping, sizeof(fragmented_ping), &header) ==
           WSS_CODEC_RESULT_PROTOCOL_ERROR);
    const uint8_t noncanonical_length[] = {0x82U, 126U, 0x00U, 0x7dU};
    assert(wss_frame_decode_server_header(
               noncanonical_length, sizeof(noncanonical_length), &header) ==
           WSS_CODEC_RESULT_PROTOCOL_ERROR);

    puts("WSS upgrade and frame codec tests passed");
    return 0;
}
