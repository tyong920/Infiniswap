#include "infiniswap_protocol.h"

#include <stdint.h>
#include <stdio.h>

int main(void)
{
  static const uint8_t current_hello[] = {
    0x49, 0x53, 0x57, 0x50, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x75,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
    0x0a, 0x05, 0x01, 0x01, 0x00, 0x00, 0x07, 0xd0,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    'c', 'o', 'n', 's', 'u', 'm', 'e', 'r', '-', 'a',
    'k', 'e', 'y', '-', 'a'
  };
  struct is_protocol_message decoded;
  uint8_t frame[IS_PROTOCOL_MAX_FRAME_SIZE];
  size_t frame_size = 0;
  uint16_t negotiated_minor = UINT16_MAX;
  uint64_t negotiated_capabilities = 0;

  if (IS_PROTOCOL_MINOR_CURRENT != 0 || IS_PROTOCOL_MINOR_NEXT != 1) {
    fprintf(stderr, "previous-minor test was compiled with the wrong version\n");
    return 1;
  }
  if (is_protocol_decode(current_hello, sizeof(current_hello), &decoded) !=
          IS_PROTOCOL_OK ||
      is_protocol_negotiate(
          IS_PROTOCOL_MINOR_CURRENT, decoded.header.capabilities,
          decoded.header.required_capabilities, decoded.header.minor,
          decoded.header.capabilities, decoded.header.required_capabilities,
          &negotiated_minor, &negotiated_capabilities) != IS_PROTOCOL_OK ||
      negotiated_minor != IS_PROTOCOL_MINOR_CURRENT ||
      negotiated_capabilities != decoded.header.capabilities) {
    fprintf(stderr, "1.0 codec rejected the frozen 1.1 HELLO vector\n");
    return 1;
  }
  decoded.header.minor = IS_PROTOCOL_MINOR_CURRENT + 2U;
  if (is_protocol_encode(&decoded, frame, sizeof(frame), &frame_size) !=
      IS_PROTOCOL_UNSUPPORTED_VERSION) {
    fprintf(stderr, "1.0 codec accepted a non-adjacent minor\n");
    return 1;
  }
  return 0;
}
