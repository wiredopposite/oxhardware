#pragma once

#include <cstddef>
#include <cstdint>

namespace HostLink::Cobs {

// Worst-case encoded size for n input bytes (no delimiter).
constexpr size_t EncodedMax(size_t n) { return n + n / 254 + 1; }

// Encodes in -> out (no 0x00 in the output). Returns the encoded length.
size_t Encode(const uint8_t *in, size_t len, uint8_t *out);

// Decodes one frame (without delimiters) in place. Returns the decoded length, or 0 if
// the input is malformed.
size_t Decode(uint8_t *buf, size_t len);

}
