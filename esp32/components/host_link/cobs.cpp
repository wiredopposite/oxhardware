#include "cobs.hpp"

namespace HostLink::Cobs {
size_t Encode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t code_at = 0;
    size_t o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == 0) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
            continue;
        }
        out[o++] = in[i];
        if (++code == 0xFF) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        }
    }
    out[code_at] = code;
    return o;
}

size_t Decode(uint8_t *buf, size_t len)
{
    size_t i = 0;
    size_t o = 0;
    while (i < len) {
        const uint8_t code = buf[i++];
        if (code == 0) {
            return 0;
        }
        for (uint8_t k = 1; k < code; k++) {
            if (i >= len) {
                return 0;
            }
            buf[o++] = buf[i++];
        }
        if (code != 0xFF && i < len) {
            buf[o++] = 0;
        }
    }
    return o;
}

}
