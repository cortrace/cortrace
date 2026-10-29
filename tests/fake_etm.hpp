// Cortrace — test fixtures: a fake ETM stream and decoder, no OpenCSD needed.
//
// The stream is A-syncs and op bytes; an op byte makes one instruction range,
// numbered so that the ranges form a valid loop whichever chunk the stream is
// cut into. See make_stream.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_TESTS_FAKE_ETM_HPP
#define CORTRACE_TESTS_FAKE_ETM_HPP

#include "cortrace/chunker.hpp"
#include "cortrace/cov_engine.hpp"
#include "tpiu_fixture.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace fake_etm {

using namespace cortrace;
using tpiu_fixture::Rng;

inline Insn mk(
    uint32_t addr, BranchKind kind = BranchKind::None, bool cond = false, uint32_t target = 0)
{
    Insn in;
    in.addr = addr;
    in.size = 2;
    in.kind = kind;
    in.cond = cond;
    in.target = target;
    in.waypoint = kind != BranchKind::None;
    return in;
}

// 0x100 nop | 0x102 BEQ 0x110 | 0x104 nop | 0x106 BL 0x120 | 0x108 nop | 0x10a B 0x100
// 0x110 nop | 0x112 BX LR | 0x120 nop | 0x122 BX LR
inline CodeMap test_map()
{
    return CodeMap({ mk(0x100), mk(0x102, BranchKind::Direct, true, 0x110), mk(0x104),
        mk(0x106, BranchKind::DirectCall, false, 0x120), mk(0x108),
        mk(0x10a, BranchKind::Direct, false, 0x100), mk(0x110), mk(0x112, BranchKind::Return),
        mk(0x120), mk(0x122, BranchKind::Return) });
}

// The fake "ETM stream": A-syncs (eleven 0x00, 0x80) and op bytes. An op byte
// b makes the range  cycle[b % 4]; the generator numbers them so the ranges
// form the loop A B C D A B ... (a not-taken BEQ, a BL, the callee's return, a
// B back), whatever chunk the stream is cut into. 0xFF is a lost sync, 0xFE a
// fatal decoder error.
inline Element range(uint32_t st, uint32_t en, BranchKind br, bool exec, uint64_t idx, bool cond)
{
    Element e = Element::instr_range(st, en, br, exec, idx);
    e.last_instr_sz = 2;
    e.last_instr_cond = cond;
    e.num_instr = (en - st) / 2;
    return e;
}

class FakeDecoder final : public IDecoder {
public:
    bool add_memory_image(uint32_t, const std::string&) override { return true; }
    bool add_elf(const std::string&) override { return true; }
    void set_sink(ElementSink s) override { sink_ = std::move(s); }
    bool process(const uint8_t* d, std::size_t n) override
    {
        for (std::size_t i = 0; i < n; ++i) {
            const uint8_t b = d[i];
            if (b == 0x00) {
                ++zeros_;
                continue;
            }
            const std::size_t z = zeros_;
            zeros_ = 0;
            if (b == 0x80 && z >= 11)
                continue; // an A-sync
            switch (b) {
            case 0xFE:
                return false;
            case 0xFF:
                sink_(Element::simple(ElementKind::NoSync, i));
                continue;
            default:
                break;
            }
            switch (b % 4) {
            case 0:
                sink_(range(0x100, 0x104, BranchKind::Direct, false, i, true));
                break;
            case 1:
                sink_(range(0x104, 0x108, BranchKind::DirectCall, true, i, false));
                break;
            case 2:
                sink_(range(0x120, 0x124, BranchKind::Return, true, i, false));
                break;
            default:
                sink_(range(0x108, 0x10c, BranchKind::Direct, true, i, false));
                break;
            }
        }
        return true;
    }
    void flush() override { }

private:
    ElementSink sink_;
    std::size_t zeros_ = 0;
};

// `segs` segments of 20..300 ops each, numbered through the loop.
inline std::vector<uint8_t> make_stream(Rng& rng, int segs, uint64_t* op = nullptr)
{
    std::vector<uint8_t> s;
    uint64_t n = op ? *op : 0;
    for (int i = 0; i < segs; ++i) {
        s.insert(s.end(), 11, 0x00);
        s.push_back(0x80);
        const int ops = 20 + static_cast<int>(rng.next() % 280);
        for (int k = 0; k < ops; ++k, ++n)
            s.push_back(static_cast<uint8_t>(4 * (1 + rng.next() % 60) + n % 4));
    }
    if (op)
        *op = n;
    return s;
}

inline CoverageEngine::Config fake_config(int workers, int lag)
{
    CoverageEngine::Config cfg;
    cfg.workers = workers;
    cfg.lag = lag;
    cfg.make_decoder = [] { return std::make_unique<FakeDecoder>(); };
    return cfg;
}

// The stream decoded in one piece by one sink.
inline CoverageSink reference(const CodeMap& m, const std::vector<uint8_t>& stream, int lag)
{
    CoverageSink s(m, lag);
    std::vector<uint64_t> starts;
    AsyncScanner().scan(stream.data(), stream.size(), 0, starts);
    for (auto a : starts)
        s.add_segment_start(a);
    FakeDecoder d;
    d.set_sink([&](const Element& e) { s.on_element(e); });
    d.process(stream.data(), stream.size());
    s.finish(stream.size());
    return s;
}

inline std::vector<Chunk> chunks_of(const std::vector<uint8_t>& stream, std::size_t target, int lag)
{
    std::vector<Chunk> out;
    Chunker ch(target, lag + 1, [&](Chunk&& c) { out.push_back(std::move(c)); });
    ch.feed(stream.data(), stream.size());
    ch.finish();
    return out;
}

} // namespace fake_etm

#endif // CORTRACE_TESTS_FAKE_ETM_HPP
