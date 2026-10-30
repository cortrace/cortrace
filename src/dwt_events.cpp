// Cortrace — DWT event packet parser (see dwt_events.hpp).
//
// SPDX-License-Identifier: MIT
#include "cortrace/dwt_events.hpp"

namespace cortrace {

namespace {
    // The SS field of a source packet header: payload size in bytes (0: not a source packet).
    std::size_t source_size(uint8_t h)
    {
        switch (h & 3) {
        case 1:
            return 1;
        case 2:
            return 2;
        case 3:
            return 4;
        default:
            return 0;
        }
    }
} // namespace

void DwtEventParser::reset()
{
    state_ = State::Header;
    remaining_ = 0;
    event_packet_ = false;
    zeros_ = 0;
}

// The first byte of a packet.
void DwtEventParser::header(const AuxByte& b)
{
    const uint8_t h = b.value;
    if (h == 0) { // sync or idle
        ++zeros_;
        return;
    }
    const std::size_t zeros = zeros_;
    zeros_ = 0;
    if (h == 0x80 && zeros >= 5) { // a synchronisation packet: 47 zero bits and a one
        ++syncs_;
        return;
    }
    if (const std::size_t size = source_size(h)) {
        // bit 2 set: hardware source (DWT), discriminator in bits 7:3; event counters are 0
        event_packet_ = (h & 0x04) != 0 && (h >> 3) == 0 && size == 1;
        remaining_ = size;
        state_ = State::Payload;
        return;
    }
    if (h == 0x70) { // overflow: the ITM FIFO dropped packets
        ++overflows_;
        return;
    }
    if ((h & 0xCF) == 0xC0 || h == 0x94 || h == 0xB4) { // local ts 1, global ts 1 and 2
        state_ = State::Continued;
        return;
    }
    if ((h & 0x8F) == 0x00) // local timestamp 2: one byte
        return;
    if ((h & 0x0B) == 0x08) { // extension packet: continued while bit 7 is set
        if (h & 0x80)
            state_ = State::Continued;
        return;
    }
    ++other_;
}

void DwtEventParser::feed(const AuxByte* bytes, std::size_t n, std::vector<DwtEvent>& out)
{
    for (std::size_t i = 0; i < n; ++i) {
        const AuxByte& b = bytes[i];
        switch (state_) {
        case State::Header:
            header(b);
            break;
        case State::Continued: // the payload of a timestamp: ends with a byte whose bit 7 is clear
            if (!(b.value & 0x80))
                state_ = State::Header;
            break;
        case State::Payload:
            if (--remaining_ == 0) {
                state_ = State::Header;
                if (event_packet_) {
                    event_packet_ = false;
                    if (b.value != 0 && (b.value & ~accept_) == 0) {
                        ++events_;
                        out.push_back({ b.value, b.pos });
                    } else {
                        ++rejected_;
                    }
                }
            }
            break;
        }
    }
}

} // namespace cortrace
