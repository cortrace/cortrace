// Cortrace — FPGA raw-capture front end implementation.
//
// Direct C++ port of the host Python deframe path (dsl_parse.assemble +
// tpiu_official.deframe), byte-for-byte faithful so results match the
// reference. See deframe.hpp for the pipeline overview.
//
// SPDX-License-Identifier: MIT
#include "cortrace/deframe.hpp"

namespace cortrace {

namespace {
    constexpr uint32_t SYNCPATTERN = 0xFFFFFF7Fu;
    constexpr uint8_t HALFSYNC_HIGH = 0x7Fu;
    constexpr uint8_t HALFSYNC_LOW = 0xFFu;
    constexpr int TPIU_PACKET_LEN = 16;
    constexpr uint8_t NO_CHANGE = 0xFFu;
} // namespace

int count_etmv4_async(const uint8_t* data, std::size_t len)
{
    int n = 0;
    int zc = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];
        if (c == 0) {
            ++zc;
        } else if (c == 0x80 && zc >= 11) {
            ++n;
            zc = 0;
        } else {
            zc = 0;
        }
    }
    return n;
}

// Width-generic reassembly (width 2 or 1). Port of host trace_width.py:
// each capture byte = {trace_b hi-nibble, trace_a lo-nibble}; per TRACECLK the
// time-ordered half-symbols are trace_a then trace_b, each `width` bits. A TPIU
// byte spans 8/width half-symbols, filled LSB-first (upstream traceIF shift).
// phase = leading half-symbols to drop; order 1 = reverse bits within a symbol.
static std::vector<uint8_t> assemble_width(
    const uint8_t* raw, std::size_t len, const DeframePhase& phase)
{
    const int width = phase.width;
    const unsigned mask = (1u << width) - 1u;
    std::vector<uint8_t> syms;
    syms.reserve(2 * len);
    for (std::size_t k = 0; k < len; ++k) {
        syms.push_back(static_cast<uint8_t>(raw[k] & mask)); // trace_a (rising)
        syms.push_back(static_cast<uint8_t>((raw[k] >> 4) & mask)); // trace_b (falling)
    }
    const int per_byte = 8 / width; // half-symbols per TPIU byte
    const std::size_t start = static_cast<std::size_t>(phase.parity);
    std::vector<uint8_t> out;
    if (syms.size() > start)
        out.reserve((syms.size() - start) / per_byte);
    for (std::size_t k = start; k + per_byte <= syms.size(); k += per_byte) {
        unsigned acc = 0;
        for (int j = 0; j < per_byte; ++j) {
            unsigned s = syms[k + j];
            if (phase.order == 1 && width > 1) {
                unsigned r = 0;
                for (int b = 0; b < width; ++b)
                    if (s & (1u << b))
                        r |= 1u << (width - 1 - b);
                s = r;
            }
            acc |= s << (width * j);
        }
        out.push_back(static_cast<uint8_t>(acc));
    }
    return out;
}

std::vector<uint8_t> assemble_nibbles(
    const uint8_t* raw, std::size_t len, const DeframePhase& phase)
{
    if (phase.width != 4)
        return assemble_width(raw, len, phase);
    // nibble stream: for each raw byte pair, high nibble of raw[k] then low
    // nibble of raw[k+1] (the {trace_a hi, trace_b lo} capture cadence).
    std::vector<uint8_t> nibs;
    if (len >= 1)
        nibs.reserve(2 * (len - 1));
    for (std::size_t k = 0; k + 1 < len; ++k) {
        nibs.push_back((raw[k] >> 4) & 0xF);
        nibs.push_back(raw[k + 1] & 0xF);
    }

    // pair nibbles into bytes under the phase.
    std::vector<uint8_t> out;
    const std::size_t start = static_cast<std::size_t>(phase.parity);
    if (nibs.size() > start)
        out.reserve((nibs.size() - start) / 2);
    for (std::size_t k = start; k + 1 < nibs.size(); k += 2) {
        const uint8_t n0 = nibs[k];
        const uint8_t n1 = nibs[k + 1];
        out.push_back(phase.order == 0 ? static_cast<uint8_t>((n1 << 4) | n0)
                                       : static_cast<uint8_t>((n0 << 4) | n1));
    }
    return out;
}

namespace {
    // Port of _getPacket: interpret one 16-byte TPIU frame, appending the bytes
    // that belong to `want_stream` to `out`. Returns the updated current stream.
    //
    // With `aux` the bytes of the stream `aux_stream` are kept too, each with
    // the number of wanted-stream bytes delivered before it (`*main_count`,
    // which counts them as they are appended).
    int get_packet(const uint8_t* rxed, int want_stream, int cur_stream, std::vector<uint8_t>& out,
        int aux_stream = -1, std::vector<AuxByte>* aux = nullptr, uint64_t* main_count = nullptr)
    {
        int delayed = NO_CHANGE;
        uint8_t lowbits = rxed[TPIU_PACKET_LEN - 1];
        int cur = cur_stream;
        auto emit = [&](uint8_t b) {
            if (want_stream < 0 || cur == want_stream) {
                out.push_back(b);
                if (main_count)
                    ++*main_count;
            } else if (aux && cur == aux_stream) {
                aux->push_back({ b, main_count ? *main_count : 0 });
            }
        };
        for (int i = 0; i < TPIU_PACKET_LEN; i += 2) {
            if (rxed[i] & 1) {
                // stream change, before or after this data byte
                if (lowbits & 1)
                    delayed = rxed[i] >> 1;
                else
                    cur = rxed[i] >> 1;
            } else {
                if (cur) // stream 0 = padding, dropped
                    emit(static_cast<uint8_t>(rxed[i] | (lowbits & 1)));
            }
            // second byte of the pair is always data (for i < 14)
            if (i < 14 && cur)
                emit(rxed[i + 1]);
            if (delayed != NO_CHANGE) {
                cur = delayed;
                delayed = NO_CHANGE;
            }
            lowbits >>= 1;
        }
        return cur;
    }

    // Multi-stream variant of get_packet: interpret one 16-byte TPIU frame,
    // appending every data byte to its stream's buffer in `streams` and the
    // byte's source index (from `rxed_src`) to the parallel `src` buffer.
    // Returns the updated current stream.
    int get_packet_multi(const uint8_t* rxed, const std::size_t* rxed_src, int cur_stream,
        std::map<int, std::vector<uint8_t>>& streams, std::map<int, std::vector<std::size_t>>& src)
    {
        int delayed = NO_CHANGE;
        uint8_t lowbits = rxed[TPIU_PACKET_LEN - 1];
        int cur = cur_stream;
        for (int i = 0; i < TPIU_PACKET_LEN; i += 2) {
            if (rxed[i] & 1) {
                // stream change, before or after this data byte
                if (lowbits & 1)
                    delayed = rxed[i] >> 1;
                else
                    cur = rxed[i] >> 1;
            } else {
                if (cur) { // stream 0 = padding, dropped
                    const uint8_t b = static_cast<uint8_t>(rxed[i] | (lowbits & 1));
                    streams[cur].push_back(b);
                    src[cur].push_back(rxed_src[i]);
                }
            }
            // second byte of the pair is always data (for i < 14)
            if (i < 14 && cur) {
                streams[cur].push_back(rxed[i + 1]);
                src[cur].push_back(rxed_src[i + 1]);
            }
            if (delayed != NO_CHANGE) {
                cur = delayed;
                delayed = NO_CHANGE;
            }
            lowbits >>= 1;
        }
        return cur;
    }
} // namespace

StreamDeframer::StreamDeframer(const DeframePhase& phase, int want_stream)
    : parity_(phase.parity)
    , order_(phase.order)
    , want_(want_stream)
{
}

void StreamDeframer::reset()
{
    have_prev_ = false;
    prev_ = 0;
    nib_index_ = 0;
    has_pending_ = false;
    synced_ = false;
    sync_monitor_ = 0;
    byte_count_ = 0;
    got_lowbits_ = false;
    cur_stream_ = 0;
}

// One byte of tpiu_deframe()'s loop.
void StreamDeframer::tpiu_byte(uint8_t d, std::vector<uint8_t>& out)
{
    sync_monitor_ = (sync_monitor_ << 8) | d;
    if (sync_monitor_ == SYNCPATTERN) {
        synced_ = true;
        byte_count_ = 0;
        got_lowbits_ = false;
        ++syncs_;
        return;
    }
    if (!synced_)
        return;
    if (!got_lowbits_) {
        got_lowbits_ = true;
        rxed_[byte_count_] = d;
        return;
    }
    got_lowbits_ = false;
    if (d == HALFSYNC_HIGH && rxed_[byte_count_] == HALFSYNC_LOW)
        return; // halfsync filler, ignore
    ++byte_count_;
    rxed_[byte_count_] = d;
    ++byte_count_;
    if (byte_count_ == TPIU_PACKET_LEN) {
        ++frames_;
        byte_count_ = 0;
        cur_stream_
            = get_packet(rxed_, want_, cur_stream_, out, aux_stream_, aux_out_, &main_bytes_);
    }
}

void StreamDeframer::feed(
    const uint8_t* raw, std::size_t len, std::vector<uint8_t>& out, std::vector<AuxByte>* aux)
{
    aux_out_ = aux;
    // assemble_nibbles(): every raw byte after the first gives the nibbles
    // hi(previous) and lo(this); the first `parity` nibbles are dropped and the
    // rest are paired, in the order the phase says.
    const uint64_t drop = static_cast<uint64_t>(parity_ < 0 ? 0 : parity_);
    for (std::size_t i = 0; i < len; ++i) {
        const uint8_t r = raw[i];
        if (have_prev_) {
            const uint8_t nibs[2]
                = { static_cast<uint8_t>((prev_ >> 4) & 0xF), static_cast<uint8_t>(r & 0xF) };
            for (const uint8_t n : nibs) {
                if (nib_index_++ < drop)
                    continue;
                if (!has_pending_) {
                    pending_ = n;
                    has_pending_ = true;
                } else {
                    has_pending_ = false;
                    tpiu_byte(order_ == 0 ? static_cast<uint8_t>((n << 4) | pending_)
                                          : static_cast<uint8_t>((pending_ << 4) | n),
                        out);
                }
            }
        }
        prev_ = r;
        have_prev_ = true;
    }
    aux_out_ = nullptr;
}

DeframeResult tpiu_deframe(
    const std::vector<uint8_t>& data, int want_stream, const DeframePhase& phase)
{
    DeframeResult r;
    r.phase = phase;

    bool state_synced = false;
    uint32_t sync_monitor = 0;
    uint8_t rxed[TPIU_PACKET_LEN] = { 0 };
    int byte_count = 0;
    bool got_lowbits = false;
    int cur_stream = 0;

    for (std::size_t pos = 0; pos < data.size(); ++pos) {
        const uint8_t d = data[pos];
        sync_monitor = (sync_monitor << 8) | d;
        if (sync_monitor == SYNCPATTERN) {
            state_synced = true;
            byte_count = 0;
            got_lowbits = false;
            ++r.syncs;
            continue;
        }
        if (!state_synced)
            continue;
        if (!got_lowbits) {
            got_lowbits = true;
            rxed[byte_count] = d;
            continue;
        }
        got_lowbits = false;
        if (d == HALFSYNC_HIGH && rxed[byte_count] == HALFSYNC_LOW)
            continue; // halfsync filler, ignore
        ++byte_count;
        rxed[byte_count] = d;
        ++byte_count;
        if (byte_count == TPIU_PACKET_LEN) {
            ++r.frames;
            byte_count = 0;
            cur_stream = get_packet(rxed, want_stream, cur_stream, r.etm);
        }
    }
    r.async_count = count_etmv4_async(r.etm.data(), r.etm.size());
    return r;
}

DeframeResult deframe_raw_capture(const uint8_t* raw, std::size_t len, int want_stream, bool search,
    const DeframePhase& phase, std::size_t search_prefix)
{
    if (!search) {
        auto data = assemble_nibbles(raw, len, phase);
        return tpiu_deframe(data, want_stream, phase);
    }

    // Pick the phase on a prefix, then deframe everything once.
    if (search_prefix > 0 && len > search_prefix) {
        const DeframeResult probe
            = deframe_raw_capture(raw, search_prefix, want_stream, true, phase, search_prefix);
        if (probe.async_count > 0)
            return deframe_raw_capture(raw, len, want_stream, false, probe.phase, search_prefix);
    }

    // Search phases; keep the best by post-deframe A-sync count, tie-broken by
    // deframed payload size. Phase count depends on width: 4-bit = 2 parity x 2
    // order; width W = (8/W) byte-boundary phases x (W>1 ? 2 : 1) bit orders.
    const int nphase = (phase.width == 4) ? 2 : (8 / phase.width);
    const int norder = (phase.width == 1) ? 1 : 2;
    DeframeResult best;
    bool have_best = false;
    for (int parity = 0; parity < nphase; ++parity) {
        for (int order = 0; order < norder; ++order) {
            DeframePhase p { parity, order, phase.width };
            auto data = assemble_nibbles(raw, len, p);
            DeframeResult r = tpiu_deframe(data, want_stream, p);
            const bool better = !have_best || r.async_count > best.async_count
                || (r.async_count == best.async_count && r.etm.size() > best.etm.size());
            if (better) {
                best = std::move(r);
                have_best = true;
            }
        }
    }
    return best;
}

MultiDeframeResult tpiu_deframe_multi(const std::vector<uint8_t>& data, const DeframePhase& phase)
{
    MultiDeframeResult r;
    r.phase = phase;

    bool state_synced = false;
    uint32_t sync_monitor = 0;
    uint8_t rxed[TPIU_PACKET_LEN] = { 0 };
    std::size_t rxed_src[TPIU_PACKET_LEN] = { 0 };
    int byte_count = 0;
    bool got_lowbits = false;
    int cur_stream = 0;

    for (std::size_t pos = 0; pos < data.size(); ++pos) {
        const uint8_t d = data[pos];
        sync_monitor = (sync_monitor << 8) | d;
        if (sync_monitor == SYNCPATTERN) {
            state_synced = true;
            byte_count = 0;
            got_lowbits = false;
            ++r.syncs;
            continue;
        }
        if (!state_synced)
            continue;
        if (!got_lowbits) {
            got_lowbits = true;
            rxed[byte_count] = d;
            rxed_src[byte_count] = pos;
            continue;
        }
        got_lowbits = false;
        if (d == HALFSYNC_HIGH && rxed[byte_count] == HALFSYNC_LOW)
            continue; // halfsync filler, ignore
        ++byte_count;
        rxed[byte_count] = d;
        rxed_src[byte_count] = pos;
        ++byte_count;
        if (byte_count == TPIU_PACKET_LEN) {
            ++r.frames;
            byte_count = 0;
            cur_stream = get_packet_multi(rxed, rxed_src, cur_stream, r.streams, r.src_index);
        }
    }
    // Alignment score: A-syncs in the ETM stream (id 2), if present.
    auto it = r.streams.find(2);
    if (it != r.streams.end())
        r.async_count = count_etmv4_async(it->second.data(), it->second.size());
    return r;
}

MultiDeframeResult deframe_raw_capture_multi(const uint8_t* raw, std::size_t len, bool search,
    const DeframePhase& phase, std::size_t search_prefix)
{
    if (!search) {
        auto data = assemble_nibbles(raw, len, phase);
        return tpiu_deframe_multi(data, phase);
    }

    // Pick the phase on a prefix, then deframe everything once.
    if (search_prefix > 0 && len > search_prefix) {
        const MultiDeframeResult probe
            = deframe_raw_capture_multi(raw, search_prefix, true, phase, search_prefix);
        if (probe.async_count > 0)
            return deframe_raw_capture_multi(raw, len, false, probe.phase, search_prefix);
    }

    // Search phases (width-aware, see deframe_raw_capture); keep the one whose
    // ETM stream (id 2) has the most A-syncs -- the ETM stream is the alignment
    // anchor; the other streams ride the same frames/phase.
    const int nphase = (phase.width == 4) ? 2 : (8 / phase.width);
    const int norder = (phase.width == 1) ? 1 : 2;
    MultiDeframeResult best;
    bool have_best = false;
    for (int parity = 0; parity < nphase; ++parity) {
        for (int order = 0; order < norder; ++order) {
            DeframePhase p { parity, order, phase.width };
            auto data = assemble_nibbles(raw, len, p);
            MultiDeframeResult r = tpiu_deframe_multi(data, p);
            std::size_t etm_sz = 0;
            auto it = r.streams.find(2);
            if (it != r.streams.end())
                etm_sz = it->second.size();
            std::size_t best_etm_sz = 0;
            if (have_best) {
                auto bit = best.streams.find(2);
                if (bit != best.streams.end())
                    best_etm_sz = bit->second.size();
            }
            const bool better = !have_best || r.async_count > best.async_count
                || (r.async_count == best.async_count && etm_sz > best_etm_sz);
            if (better) {
                best = std::move(r);
                have_best = true;
            }
        }
    }
    return best;
}

} // namespace cortrace
