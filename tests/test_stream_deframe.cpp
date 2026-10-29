// Cortrace — StreamDeframer must equal the whole-capture deframer.
//
// SPDX-License-Identifier: MIT
#include "cortrace/deframe.hpp"
#include "test_framework.hpp"
#include "tpiu_fixture.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace cortrace;

namespace {

using namespace tpiu_fixture;

std::vector<uint8_t> stream_out(const std::vector<uint8_t>& raw, const DeframePhase& phase,
    int want, const std::vector<std::size_t>& pieces, std::size_t* frames = nullptr,
    std::size_t* syncs = nullptr)
{
    StreamDeframer d(phase, want);
    std::vector<uint8_t> out;
    std::size_t at = 0, i = 0;
    while (at < raw.size()) {
        const std::size_t n = std::min(pieces[i++ % pieces.size()], raw.size() - at);
        d.feed(raw.data() + at, n, out);
        at += n;
    }
    if (frames)
        *frames = d.frames();
    if (syncs)
        *syncs = d.syncs();
    return out;
}

const std::vector<std::vector<std::size_t>> kSplits = {
    { 1 << 30 }, // whole
    { 1 }, // a byte at a time
    { 2, 3 }, // odd sizes
    { 1024 }, // UDP payloads
    { 7, 1, 100, 33, 5000 },
    { 16, 15, 17 },
};

} // namespace

TEST(stream_deframe_helper_inverts_the_assembly)
{
    Rng rng { 1 };
    const auto a = tpiu_stream(rng, 20, 8);
    for (int parity = 0; parity < 2; ++parity)
        for (int order = 0; order < 2; ++order) {
            DeframePhase ph { parity, order, 4 };
            const auto raw = raw_for(rng, a, ph);
            const auto back = assemble_nibbles(raw.data(), raw.size(), ph);
            CHECK(back.size() >= a.size());
            CHECK(std::equal(a.begin(), a.end(), back.begin()));
        }
}

TEST(stream_deframe_equals_the_whole_capture_deframer_in_every_phase_and_split)
{
    Rng rng { 42 };
    for (int parity = 0; parity < 2; ++parity)
        for (int order = 0; order < 2; ++order) {
            const DeframePhase ph { parity, order, 4 };
            const auto a = tpiu_stream(rng, 600, 50);
            const auto raw = raw_for(rng, a, ph);
            for (int want : { 2, 1, -1 }) {
                const DeframeResult ref
                    = deframe_raw_capture(raw.data(), raw.size(), want, /*search=*/false, ph);
                CHECK(!ref.etm.empty());
                CHECK(ref.frames > 500);
                for (const auto& split : kSplits) {
                    std::size_t frames = 0, syncs = 0;
                    const auto got = stream_out(raw, ph, want, split, &frames, &syncs);
                    CHECK(got == ref.etm);
                    CHECK_EQ(frames, ref.frames);
                    CHECK_EQ(syncs, ref.syncs);
                }
            }
        }
}

TEST(stream_deframe_equals_the_batch_on_noise)
{
    // Not TPIU at all: the two must still agree byte for byte (and so on the
    // rare sync pattern and halfsync pairs noise happens to contain).
    Rng rng { 7 };
    std::vector<uint8_t> raw(200000);
    for (auto& b : raw)
        b = (rng.next() % 5 == 0) ? 0xFF : rng.byte();
    for (int parity = 0; parity < 2; ++parity)
        for (int order = 0; order < 2; ++order) {
            const DeframePhase ph { parity, order, 4 };
            const DeframeResult ref = deframe_raw_capture(raw.data(), raw.size(), -1, false, ph);
            for (const auto& split : kSplits)
                CHECK(stream_out(raw, ph, -1, split) == ref.etm);
        }
}

TEST(stream_deframe_reset_waits_for_the_next_sync)
{
    Rng rng { 99 };
    const DeframePhase ph { 1, 0, 4 };
    const auto a = tpiu_stream(rng, 3000, 40);
    const auto raw = raw_for(rng, a, ph);
    CHECK(raw.size() > 20000);

    // Cut the capture in the middle and drop an odd number of raw bytes, as a
    // lost packet would: the stream after the gap is deframed from its first
    // sync pattern on, and it equals a fresh deframer fed the same bytes.
    const std::size_t cut = 5000, lost = 1024 * 3 + 5;
    StreamDeframer d(ph, 2);
    std::vector<uint8_t> out;
    d.feed(raw.data(), cut, out);
    CHECK(d.synced());
    d.reset();
    CHECK(!d.synced());
    const std::size_t before = out.size();
    d.feed(raw.data() + cut + lost, raw.size() - cut - lost, out);

    StreamDeframer fresh(ph, 2);
    std::vector<uint8_t> tail;
    fresh.feed(raw.data() + cut + lost, raw.size() - cut - lost, tail);
    CHECK(out.size() - before == tail.size());
    CHECK(std::equal(tail.begin(), tail.end(), out.begin() + static_cast<long>(before)));
    CHECK(!tail.empty());

    // Nothing is emitted between the gap and the next sync pattern.
    StreamDeframer g(ph, 2);
    std::vector<uint8_t> none;
    const std::vector<uint8_t> junk(4000, 0x55);
    g.feed(junk.data(), junk.size(), none);
    CHECK(none.empty());
    CHECK(!g.synced());
}

TEST(stream_deframe_only_the_four_bit_port_is_supported)
{
    CHECK(StreamDeframer::supported(DeframePhase { 1, 0, 4 }));
    CHECK(!StreamDeframer::supported(DeframePhase { 0, 0, 2 }));
    CHECK(!StreamDeframer::supported(DeframePhase { 0, 0, 1 }));
}
