#pragma once

// Programmer protocol server (docs/07-console-protocol.md): packet framing and the
// command handlers, shared by every transport (the console's mailbox, the PC link).

#include <cstddef>
#include <cstdint>

#include "oxprog/oxprog.hpp"

namespace ProgServer {

// Collects one packet from a byte stream: skips bytes until a valid header, then takes
// its payload.
class Parser {
public:
    // Takes bytes until a packet is complete; returns how many were used.
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

// A reply packet, header included
struct Packet {
    uint8_t Data[sizeof(OxProg::Header) + OxProg::PayloadMax];
    size_t Size = 0;
};

// Answers a complete packet from parser into reply. Thread-safe.
void Handle(const Parser &parser, Packet &reply);

// Mailbox transport: polls the FPGA's console port and serves it
void StartMailbox();

// PC transport: host link command ProgXfer (a packet in, the reply out)
void RegisterCommands();

}
