// Cortrace — CovPipeline: raw FPGA bytes -> deframe -> chunks -> engine.
//
// SPDX-License-Identifier: MIT
#include "cortrace/cov_pipeline.hpp"
#include "fake_etm.hpp"
#include "test_framework.hpp"
#include "tpiu_fixture.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace cortrace;
using namespace fake_etm;
using namespace tpiu_fixture;

namespace {

const DeframePhase kPhase { 1, 0, 4 };

// The fake ETM stream (padded to whole TPIU frames) and the raw capture of it.
struct Capture {
    std::vector<uint8_t> etm; // what the deframer must give back
    std::vector<uint8_t> raw;
};

Capture capture_of(Rng& rng, int segs, int sync_every = 40)
{
    Capture c;
    c.etm = make_stream(rng, segs);
    std::size_t pad = 0;
    const auto tpiu = tpiu_wrap(c.etm, 2, sync_every, &pad);
    c.etm.insert(c.etm.end(), pad, 0x00); // the zero padding is part of the stream
    c.raw = raw_for(rng, tpiu, kPhase);
    return c;
}

std::string file_text(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string without(std::string j, const char* key)
{
    const auto at = j.find(key);
    return at == std::string::npos ? j : j.erase(at, j.find(',', at) - at);
}

} // namespace

TEST(pipeline_raw_capture_gives_the_same_coverage_as_decoding_the_etm_stream)
{
    const CodeMap m = test_map();
    Rng rng { 17 };
    const Capture cap = capture_of(rng, 500);
    const int lag = 3;
    const CoverageSink ref = reference(m, cap.etm, lag);

    for (std::size_t piece : { std::size_t(1024), std::size_t(7), std::size_t(100000) }) {
        for (int workers : { 1, 4 }) {
            CoverageEngine eng(m, nullptr, fake_config(workers, lag));
            std::string err;
            CHECK(eng.start(err));
            CovPipeline pipe(eng, kPhase, 2, 8000, lag, /*wait=*/true);
            for (std::size_t at = 0; at < cap.raw.size(); at += piece)
                pipe.feed_raw(cap.raw.data() + at, std::min(piece, cap.raw.size() - at));
            pipe.finish();
            eng.finish();

            const std::string a = cortrace_test::temp_path("pl_ref.json");
            const std::string b = cortrace_test::temp_path("pl_eng.json");
            CHECK(ref.write_json(a, "fw.elf"));
            CHECK(eng.write_json(b, "fw.elf"));
            CHECK(!file_text(a).empty());
            // Every count of what was executed, the segments and the losses are
            // equal. Only how many checks could be made differs: a chunk's first
            // range has no flow to check, and a chunk that starts inside a call
            // has no shadow stack for the return that ends it.
            auto same = [](const std::string& x, const std::string& y) {
                auto bare = [](std::string j) {
                    return without(without(j, "\"flow_checked\": "), "\"return_checked\": ");
                };
                return bare(x) == bare(y);
            };
            CHECK(same(file_text(a), file_text(b)));
            std::remove(a.c_str());
            std::remove(b.c_str());
            CHECK_EQ(eng.health().losses(), uint64_t(0));
            CHECK_EQ(pipe.etm_bytes(), uint64_t(cap.etm.size()));
            CHECK_EQ(pipe.raw_bytes(), uint64_t(cap.raw.size()));
            CHECK(pipe.chunks() > 3);
        }
    }
}

TEST(pipeline_lost_frames_cost_only_what_surrounds_them)
{
    const CodeMap m = test_map();
    Rng rng { 23 };
    const Capture cap = capture_of(rng, 700, /*sync_every=*/12);
    const int lag = 3;
    const CoverageSink ref = reference(m, cap.etm, lag);

    // 3 packets of 1024 B are missing in the middle
    const std::size_t cut = 40 * 1024, lost = 3 * 1024;
    CHECK(cap.raw.size() > cut + lost + 20000);
    CoverageEngine eng(m, nullptr, fake_config(3, lag));
    std::string err;
    CHECK(eng.start(err));
    CovPipeline pipe(eng, kPhase, 2, 6000, lag, true);
    for (std::size_t at = 0; at < cut; at += 1024)
        pipe.feed_raw(cap.raw.data() + at, 1024);
    pipe.gap(3);
    for (std::size_t at = cut + lost; at < cap.raw.size(); at += 1024)
        pipe.feed_raw(cap.raw.data() + at, std::min<std::size_t>(1024, cap.raw.size() - at));
    pipe.finish();
    eng.finish();

    const CoverageHealth h = eng.health();
    CHECK_EQ(h.udp_gaps, uint64_t(1));
    CHECK_EQ(h.udp_lost_frames, uint64_t(3));
    CHECK(h.losses() >= 1);
    CHECK(h.dropped_bytes > 0); // from the gap to the next A-sync
    CHECK(h.committed > 100); // both sides of the gap are used
    CHECK(h.quarantined >= uint64_t(lag)); // the end of what came before it is not
    // Nothing is counted that the whole stream does not have: every
    // instruction's hits are at most those of the sequential decode.
    CHECK(h.committed < ref.health().committed);
    CHECK(eng.status().instructions_hit <= ref.hits().size());
    const std::string f = cortrace_test::temp_path("pl_gap.json");
    CHECK(eng.write_json(f, "fw.elf"));
    std::ifstream in(f);
    std::stringstream ss;
    ss << in.rdbuf();
    std::remove(f.c_str());
    // [addr, size, flags, hits, segs] rows, in code-map order
    const std::string text = ss.str();
    std::size_t at = text.find("\"insns\": [") + 10, row = 0;
    std::size_t over = 0;
    while (row < ref.hits().size()) {
        at = text.find('[', at + 1);
        unsigned long long addr, size, flags, hits, segs;
        CHECK_EQ(std::sscanf(text.c_str() + at, "[%llu, %llu, %llu, %llu, %llu]", &addr, &size,
                     &flags, &hits, &segs),
            5);
        if (hits > ref.hits()[row])
            ++over;
        ++row;
    }
    CHECK_EQ(over, std::size_t(0));
}

TEST(pipeline_find_phase_picks_the_wiring_and_refuses_noise)
{
    Rng rng { 31 };
    for (int parity = 0; parity < 2; ++parity)
        for (int order = 0; order < 2; ++order) {
            const DeframePhase ph { parity, order, 4 };
            auto etm = make_stream(rng, 60);
            std::size_t pad = 0;
            const auto raw = raw_for(rng, tpiu_wrap(etm, 2, 30, &pad), ph);
            DeframePhase found;
            CHECK(find_deframe_phase(raw.data(), raw.size(), 2, 4, found));
            CHECK_EQ(found.parity, parity);
            CHECK_EQ(found.order, order);
        }
    std::vector<uint8_t> noise(50000, 0x55);
    DeframePhase none;
    CHECK(!find_deframe_phase(noise.data(), noise.size(), 2, 4, none));
}
