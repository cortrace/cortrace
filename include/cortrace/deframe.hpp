// Cortrace — FPGA raw-capture front end: nibble reassemble + TPIU deframe.
//
// The FPGA capture appliance streams the target's parallel-trace (TPIU) port as a raw
// byte sequence (two nibbles per TRACECLK period, {trace_a hi, trace_b lo}).
// Turning that into the bare ETMv4 byte stream OpenCSD wants is three byte-wise
// passes that used to run in slow host Python (deframe_to_etm.py):
//   1. nibble extract  : raw[k] -> two 4-bit nibbles
//   2. assemble        : pair nibbles into bytes under a (parity, order) phase
//   3. TPIU deframe     : 16-byte CoreSight frames -> the requested ATB stream
// This is the C++ port so cortrace-decode can consume a raw capture directly
// (orders of magnitude faster than the Python path).
//
// The (parity, order) phase is fixed for a given board + bitstream, but a fresh
// capture's phase is recovered by trying all four and scoring each by the
// number of post-deframe ETMv4 A-syncs (the only signal that proves the whole
// chain — nibble phase, TPIU frame phase, stream demux — is aligned).
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_DEFRAME_HPP
#define CORTRACE_DEFRAME_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace cortrace {

// Nibble-pairing phase. parity drops the leading nibble (0 or 1); order chooses
// whether the first nibble of a pair is the low or high half of the byte.
//
// For non-4-bit parallel ports (width 2 or 1) the capture format is the same
// (one byte per TRACECLK, {trace_b hi-nibble, trace_a lo-nibble}) but a TPIU
// byte spans 8/(2*width) TRACECLK periods, so reassembly differs. When
// width != 4, `parity` is reinterpreted as the byte-boundary phase
// (0..8/width-1) and `order` as the within-half-symbol bit order (0=lsb,1=msb).
// width == 4 keeps the original parity/order nibble-pairing exactly.
struct DeframePhase {
    int parity = 1; // matches the A7-Lite board default (parity=1, order=0)
    int order = 0;
    int width = 4; // parallel TRACED port width: 4, 2 or 1
};

struct DeframeResult {
    std::vector<uint8_t> etm; // deframed ETMv4 bytes for the requested stream
    DeframePhase phase; // the phase actually used
    int async_count = 0; // ETMv4 A-syncs found post-deframe (alignment score)
    std::size_t frames = 0; // TPIU 16-byte frames decoded
    std::size_t syncs = 0; // full TPIU sync patterns seen
};

// Multi-stream deframe: demux ALL TPIU ATB streams in a single pass so ETM
// (stream 2) and DWT/ITM (stream 1) share one timebase. Each output byte keeps
// the source index (into the ASSEMBLED byte stream) it came from, so callers
// can map any stream's byte back to the same FPGA time base array.
struct MultiDeframeResult {
    // stream_id -> deframed bytes for that ATB stream (stream 0 = padding,
    // dropped). Typical ids here: 2 = ETM, 1 = ITM/DWT.
    std::map<int, std::vector<uint8_t>> streams;
    // stream_id -> per-byte source index into the assembled stream (parallel to
    // streams[id]); used to index a shared timebase.
    std::map<int, std::vector<std::size_t>> src_index;
    DeframePhase phase;
    int async_count = 0; // ETMv4 A-syncs in stream 2 (alignment score)
    std::size_t frames = 0;
    std::size_t syncs = 0;
};

// A byte of a second ATB stream, with its place among the bytes of the main
// stream: `pos` main-stream bytes were delivered before it. A decoder that
// numbers the main-stream bytes (OpenCSD's index) can then tell which of its
// elements came before the byte.
struct AuxByte {
    uint8_t value;
    uint64_t pos;
};

// How much of a raw capture the phase search looks at before locking the winner.
constexpr std::size_t kPhaseSearchPrefix = 8u << 20;

// Deframe an assembled byte stream, demuxing every ATB stream at once.
MultiDeframeResult tpiu_deframe_multi(const std::vector<uint8_t>& data, const DeframePhase& phase);

// Full front end (multi-stream): nibble-assemble + demux all streams. When
// `search` is true, try all four phases and keep the one whose stream 2 has the
// most ETMv4 A-syncs (the ETM stream is the alignment anchor for the whole
// capture; the other streams ride the same frames/phase).
//
// The phase is a property of the wiring, not of the data, so the search runs on
// the first `search_prefix` bytes only and the winner is then applied to the
// whole capture (4 full passes over a 700 MB capture were most of the deframe
// time). If no candidate shows an A-sync in the prefix, the whole capture is
// searched as before.
MultiDeframeResult deframe_raw_capture_multi(const uint8_t* raw, std::size_t len, bool search,
    const DeframePhase& phase, std::size_t search_prefix = kPhaseSearchPrefix);

// Assemble bytes from a raw capture under a fixed phase (no search).
std::vector<uint8_t> assemble_nibbles(
    const uint8_t* raw, std::size_t len, const DeframePhase& phase);

// TPIU-deframe an assembled byte stream, extracting `want_stream` (ETM = 2).
// Faithful port of orbuculum's tpiuDecoder.c (see tpiu_official.py).
DeframeResult tpiu_deframe(
    const std::vector<uint8_t>& data, int want_stream, const DeframePhase& phase);

// Full front end: nibble-assemble + TPIU-deframe a raw FPGA capture. If
// `search` is true, try all four phases and keep the one with the most
// post-deframe A-syncs; otherwise use `phase` as given.
// Same prefix-search rule as deframe_raw_capture_multi.
DeframeResult deframe_raw_capture(const uint8_t* raw, std::size_t len, int want_stream, bool search,
    const DeframePhase& phase, std::size_t search_prefix = kPhaseSearchPrefix);

// Count ETMv4 A-syncs (>= 11 zero bytes followed by 0x80) in a byte stream.
int count_etmv4_async(const uint8_t* data, std::size_t len);

// The same front end for a stream that never ends (cortrace-cov): raw bytes go
// in as they arrive, in pieces of any size, and the bytes of one ATB stream
// come out. Fed the whole capture in any split it gives exactly what
// deframe_raw_capture(search=false) gives, because it runs the same steps one
// byte at a time with the state kept in between.
//
// The phase must be known (it is a property of the wiring: find it once with
// deframe_raw_capture on a prefix). Only the 4-bit port is supported.
class StreamDeframer {
public:
    static bool supported(const DeframePhase& phase) { return phase.width == 4; }

    // want_stream < 0 keeps every stream. Requires supported(phase).
    StreamDeframer(const DeframePhase& phase, int want_stream);

    // Also keep the bytes of this stream (the ITM/DWT, 1) beside the wanted
    // one, see AuxByte. -1: none.
    void set_aux_stream(int id) { aux_stream_ = id; }

    // Appends the bytes of the wanted stream found in raw[0..len) to `out`,
    // and with `aux` those of the aux stream, in the order of the stream.
    void feed(const uint8_t* raw, std::size_t len, std::vector<uint8_t>& out,
        std::vector<AuxByte>* aux = nullptr);

    // Bytes of the wanted stream delivered so far (not reset by reset()).
    uint64_t main_bytes() const { return main_bytes_; }

    // The raw stream was interrupted (frames lost): forget the half-assembled
    // byte, the TPIU frame in progress and the current stream, and wait for the
    // next TPIU sync pattern. Whole frames lost keep the nibble phase, since
    // every raw byte makes one assembled byte, so the phase is unchanged.
    void reset();

    std::size_t frames() const { return frames_; } // TPIU frames decoded
    std::size_t syncs() const { return syncs_; } // full TPIU sync patterns seen
    bool synced() const { return synced_; }

private:
    void tpiu_byte(uint8_t d, std::vector<uint8_t>& out);

    int parity_, order_, want_;
    int aux_stream_ = -1;
    std::vector<AuxByte>* aux_out_ = nullptr;
    uint64_t main_bytes_ = 0;
    // nibble assembly
    bool have_prev_ = false;
    uint8_t prev_ = 0;
    uint64_t nib_index_ = 0; // nibbles produced since the start (or reset)
    bool has_pending_ = false;
    uint8_t pending_ = 0;
    // TPIU
    bool synced_ = false;
    uint32_t sync_monitor_ = 0;
    uint8_t rxed_[16] = { 0 };
    int byte_count_ = 0;
    bool got_lowbits_ = false;
    int cur_stream_ = 0;
    std::size_t frames_ = 0, syncs_ = 0;
};

} // namespace cortrace

#endif // CORTRACE_DEFRAME_HPP
