#pragma once

#include <cstddef>
#include <cstdint>

#include "oxprog/oxprog.hpp"

namespace ProgServer {
class Parser {
public:
    size_t Feed(const uint8_t *data, size_t len);
    bool Complete() const { return Have == sizeof(OxProg::Header) + Hdr.Length && HdrOk; }
    const OxProg::Header &Head() const { return Hdr; }
    const uint8_t *Payload() const { return Buf + sizeof(OxProg::Header); }
    void Reset() { Have = 0; HdrOk = false; }

private:
    bool MagicPrefix() const;
    void Shift();

    uint8_t Buf[sizeof(OxProg::Header) + OxProg::PayloadMax];
    OxProg::Header Hdr = {};
    size_t Have = 0;
    bool HdrOk = false;
};

struct Packet {
    uint8_t Data[sizeof(OxProg::Header) + OxProg::PayloadMax];
    size_t Size = 0;
};

void Handle(const Parser &parser, Packet &reply);

void StartMailbox();

void RegisterCommands();

}
