// Cortrace — test fixtures: TPIU streams and the raw FPGA capture bytes of them.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_TESTS_TPIU_FIXTURE_HPP
#define CORTRACE_TESTS_TPIU_FIXTURE_HPP

#include "cortrace/deframe.hpp"

#include <cstdint>
#include <vector>

namespace tpiu_fixture {

struct Rng {
    uint64_t s;
    uint32_t next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    uint8_t byte() { return static_cast<uint8_t>(next()); }
};

// One TPIU frame carrying 14 data bytes of ATB stream `id`: slot 0 changes the
// stream, the other slots carry data (an even byte holds its bit 0 in byte 15).
inline std::vector<uint8_t> frame_of(int id, const uint8_t* d)
{
    std::vector<uint8_t> f(16, 0);
    uint8_t aux = 0;
    f[0] = static_cast<uint8_t>((id << 1) | 1);
    f[1] = d[0];
    int k = 1;
    for (int slot = 1; slot < 8; ++slot) {
        const uint8_t a = d[k++];
        f[2 * slot] = a & 0xFE;
        if (a & 1)
            aux |= static_cast<uint8_t>(1u << slot);
        if (slot < 7)
            f[2 * slot + 1] = d[k++];
    }
    f[15] = aux;
    return f;
}

inline void put_sync(std::vector<uint8_t>& a)
{
    for (uint8_t b : { 0xFF, 0xFF, 0xFF, 0x7F })
        a.push_back(b);
}

// An assembled TPIU byte stream: a sync pattern, then frames of ETM (stream 2)
// and ITM (stream 1) data, now and then a halfsync filler pair, and a full
// sync again every `sync_every` frames.
inline std::vector<uint8_t> tpiu_stream(Rng& rng, int nframes, int sync_every)
{
    std::vector<uint8_t> a;
    put_sync(a);
    for (int n = 0; n < nframes; ++n) {
        if (n && n % sync_every == 0)
            put_sync(a);
        if (n % 7 == 3) { // halfsync filler pair between frames
            a.push_back(0xFF);
            a.push_back(0x7F);
        }
        uint8_t d[14];
        for (auto& x : d)
            x = rng.byte();
        if (n % 5 == 0) { // an A-sync inside the ETM data
            for (int i = 0; i < 11; ++i)
                d[i] = 0;
            d[11] = 0x80;
        }
        const auto f = frame_of(n % 3 == 1 ? 1 : 2, d);
        a.insert(a.end(), f.begin(), f.end());
    }
    return a;
}

// `payload` as ATB stream `id` in TPIU frames, a sync pattern every
// `sync_every` frames. The last frame is padded with zero bytes (which are
// harmless in an ETM stream as long as they are fewer than 11).
inline std::vector<uint8_t> tpiu_wrap(
    const std::vector<uint8_t>& payload, int id, int sync_every, std::size_t* padded = nullptr)
{
    std::vector<uint8_t> a;
    put_sync(a);
    std::size_t at = 0, n = 0;
    while (at < payload.size()) {
        if (n && n % static_cast<std::size_t>(sync_every) == 0)
            put_sync(a);
        uint8_t d[14] = { 0 };
        for (int i = 0; i < 14 && at < payload.size(); ++i)
            d[i] = payload[at++];
        const auto f = frame_of(id, d);
        a.insert(a.end(), f.begin(), f.end());
        ++n;
    }
    if (padded)
        *padded = n * 14 - payload.size();
    return a;
}

// Raw capture bytes that assemble_nibbles() turns into `a` under `phase`.
inline std::vector<uint8_t> raw_for(
    Rng& rng, const std::vector<uint8_t>& a, const cortrace::DeframePhase& phase)
{
    std::vector<uint8_t> nibs;
    for (int i = 0; i < phase.parity; ++i)
        nibs.push_back(rng.byte() & 0xF);
    for (uint8_t b : a) {
        const uint8_t lo = b & 0xF, hi = b >> 4;
        if (phase.order == 0) { // byte = (n1 << 4) | n0
            nibs.push_back(lo);
            nibs.push_back(hi);
        } else { // byte = (n0 << 4) | n1
            nibs.push_back(hi);
            nibs.push_back(lo);
        }
    }
    nibs.push_back(rng.byte() & 0xF); // the last assembled byte needs its pair's end
    nibs.push_back(rng.byte() & 0xF);
    // nibs[2k] = hi(raw[k]), nibs[2k+1] = lo(raw[k+1])
    std::vector<uint8_t> raw;
    const std::size_t n = nibs.size() / 2 + 1;
    for (std::size_t k = 0; k < n; ++k) {
        const uint8_t hi = 2 * k < nibs.size() ? nibs[2 * k] : 0;
        const uint8_t lo = k > 0 && 2 * k - 1 < nibs.size() ? nibs[2 * k - 1] : (rng.byte() & 0xF);
        raw.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return raw;
}

} // namespace tpiu_fixture

#endif // CORTRACE_TESTS_TPIU_FIXTURE_HPP
