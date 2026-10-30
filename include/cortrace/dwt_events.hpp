// Cortrace — DWT event counter packets from the ITM/DWT stream.
//
// The DWT has 8-bit counters for CPI (extra cycles of multi-cycle instructions),
// EXC (exception overhead cycles), SLEEP (cycles asleep), LSU (extra cycles of
// loads and stores) and FOLD (folded instructions). Each time one wraps, the
// DWT sends an "event counter" packet whose payload says which: so one packet
// stands for 256 events of that kind. A counter is enabled with its
// DWT_CTRL.xxxEVTENA bit.
//
// The packets sit in the ITM/DWT stream (TPIU stream 1) between the stimulus
// packets, timestamps and comparator packets; the parser walks all of them to
// stay aligned and gives back only the event packets.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_DWT_EVENTS_HPP
#define CORTRACE_DWT_EVENTS_HPP

#include "cortrace/deframe.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cortrace {

// Bits of the event packet payload (and of DwtEvent::mask).
enum DwtEventBit : uint8_t {
    kDwtCpi = 1u << 0,
    kDwtExc = 1u << 1,
    kDwtSleep = 1u << 2,
    kDwtLsu = 1u << 3,
    kDwtFold = 1u << 4,
    kDwtPost = 1u << 5, // the cycle counter's postscaler: not an event of the core
};
constexpr uint8_t kDwtCounterBits = kDwtCpi | kDwtExc | kDwtSleep | kDwtLsu | kDwtFold;

// Events (counter wraps) that were reported together, and where.
struct DwtEvent {
    uint8_t mask = 0; // DwtEventBit
    uint64_t pos = 0; // bytes of the ETM stream delivered before the packet (AuxByte::pos)
};

class DwtEventParser {
public:
    // `accept`: the counters that were switched on. A packet with any other bit
    // is not one of ours (a stream that lost its alignment can produce them) and
    // is dropped and counted.
    explicit DwtEventParser(uint8_t accept = kDwtCounterBits | kDwtPost)
        : accept_(accept)
    {
    }

    // Walk the next bytes of the stream, in order; events are appended to `out`.
    void feed(const AuxByte* bytes, std::size_t n, std::vector<DwtEvent>& out);

    // The stream was interrupted: forget the packet in progress.
    void reset();

    uint64_t events() const { return events_; }
    uint64_t rejected() const { return rejected_; } // event-looking packets with other bits
    uint64_t overflows() const { return overflows_; } // the ITM FIFO dropped packets
    uint64_t syncs() const { return syncs_; }
    uint64_t other_packets() const { return other_; }

private:
    enum class State { Header, Payload, Continued };

    void header(const AuxByte& b);

    uint8_t accept_;
    State state_ = State::Header;
    std::size_t remaining_ = 0; // payload bytes still to come
    bool event_packet_ = false; // the packet in progress is an event counter packet
    std::size_t zeros_ = 0; // zero bytes in a row (a sync is 5 of them and 0x80)
    uint64_t events_ = 0, rejected_ = 0, overflows_ = 0, syncs_ = 0, other_ = 0;
};

} // namespace cortrace

#endif // CORTRACE_DWT_EVENTS_HPP
