// Cortrace — CoverageEngine unit tests (fake decoder, no OpenCSD).
//
// SPDX-License-Identifier: MIT
#include "cortrace/chunker.hpp"
#include "cortrace/cov_engine.hpp"
#include "elf_fixture.hpp"
#include "fake_etm.hpp"
#include "test_framework.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <new>
#include <sstream>
#include <thread>
#include <vector>

using namespace cortrace;

namespace {

using namespace fake_etm;

} // namespace

TEST(engine_parallel_decode_equals_one_sink_decoding_the_whole_stream)
{
    const CodeMap m = test_map();
    Rng rng { 1 };
    const auto stream = make_stream(rng, 600);
    const int lag = 4;
    const CoverageSink ref = reference(m, stream, lag);
    CHECK(ref.health().committed > 500);

    for (int workers : { 1, 3, 8 }) {
        auto chunks = chunks_of(stream, 10000, lag);
        const std::size_t nchunks = chunks.size();
        CHECK(nchunks > 5);
        CoverageEngine eng(m, nullptr, fake_config(workers, lag));
        std::string err;
        CHECK(eng.start(err));
        for (auto& c : chunks)
            CHECK(eng.submit(std::move(c)));
        eng.finish();

        const std::string a = cortrace_test::temp_path("eq_ref.json");
        const std::string b = cortrace_test::temp_path("eq_eng.json");
        CHECK(ref.write_json(a, "fw.elf"));
        CHECK(eng.write_json(b, "fw.elf"));
        std::ifstream fa(a), fb(b);
        std::stringstream sa, sb;
        sa << fa.rdbuf();
        sb << fb.rdbuf();
        CHECK(!sa.str().empty());
        // Everything but flow_checked is equal: a chunk's first range has no
        // expectation to check against (the decoder starts there), so it
        // checks one range less per chunk boundary.
        auto without_flow_checked = [](std::string j) {
            const auto at = j.find("\"flow_checked\": ");
            const auto end = j.find(',', at);
            return j.erase(at, end - at);
        };
        CHECK(without_flow_checked(sa.str()) == without_flow_checked(sb.str()));
        CHECK(eng.health().flow_checked + nchunks >= ref.health().flow_checked);
        CHECK(eng.health().flow_checked <= ref.health().flow_checked);
        std::remove(a.c_str());
        std::remove(b.c_str());

        const CoverageHealth h = eng.health();
        CHECK_EQ(h.losses(), uint64_t(0));
        CHECK_EQ(h.committed, ref.health().committed);
        CHECK_EQ(h.quarantined, uint64_t(1)); // the last segment of the stream
        const auto st = eng.status();
        CHECK_EQ(st.chunks_done, uint64_t(nchunks));
        CHECK_EQ(st.chunks_dropped, uint64_t(0));
        CHECK_EQ(st.queued, std::size_t(0));
    }
}

TEST(engine_a_loss_just_after_a_chunk_drops_the_end_of_that_chunk)
{
    const CodeMap m = test_map();
    const int lag = 3;
    Rng rng { 2 };
    uint64_t op = 0;
    // chunk 0: 30 segments. The stream goes on with 10 more segments that are
    // chunk 1; a lost sync is the first op of the second of them, which is in
    // chunk 0's overlap (lag + 1 = 4 segments).
    auto head = make_stream(rng, 30, &op);
    auto tail = make_stream(rng, 10, &op);
    std::vector<uint64_t> starts;
    AsyncScanner().scan(tail.data(), tail.size(), 0, starts);
    CHECK(starts.size() == 10);
    tail[starts[1] + 12] = 0xFF;
    std::vector<uint8_t> stream = head;
    stream.insert(stream.end(), tail.begin(), tail.end());

    Chunk c0, c1;
    c0.seq = 0;
    c0.commit_end = head.size();
    c0.data = head;
    c0.data.insert(c0.data.end(), tail.begin(), tail.begin() + static_cast<long>(starts[lag + 1]));
    c1.seq = 1;
    c1.commit_end = tail.size();
    c1.data = tail;

    CoverageEngine eng(m, nullptr, fake_config(2, lag));
    std::string err;
    CHECK(eng.start(err));
    CHECK(eng.submit(std::move(c0)));
    CHECK(eng.submit(std::move(c1)));
    eng.finish();

    const CoverageSink ref = reference(m, stream, lag);
    const CoverageHealth h = eng.health();
    CHECK_EQ(ref.health().lost_sync, uint64_t(1));
    CHECK_EQ(h.lost_sync, uint64_t(1)); // seen by both chunks, counted by the one that owns it
    // the same segments are kept and dropped as in the sequential decode
    CHECK_EQ(h.committed, ref.health().committed);
    CHECK_EQ(h.quarantined, ref.health().quarantined);
    CHECK_EQ(h.quarantined_bytes, ref.health().quarantined_bytes);
    // chunk 0 lost its last `lag` segments to a loss that came after it
    CHECK(h.quarantined > uint64_t(lag));
}

TEST(engine_untrusted_tail_fatal_decoder_and_udp_gaps_are_losses)
{
    const CodeMap m = test_map();
    Rng rng { 3 };
    uint64_t op = 0;
    auto s0 = make_stream(rng, 12, &op);
    auto s1 = make_stream(rng, 12, &op);

    // the chunks on their own, to see what each of them adds
    auto run = [&](const Chunk& chunk, const char* what) {
        CoverageEngine eng(m, nullptr, fake_config(1, 2));
        std::string err;
        CHECK(eng.start(err));
        Chunk copy = chunk;
        CHECK(eng.submit(std::move(copy)));
        eng.finish();
        (void)what;
        return std::make_pair(eng.health(), eng.status().instructions_hit);
    };
    Chunk a;
    a.seq = 0;
    a.data = s0;
    a.commit_end = s0.size();
    a.tail_untrusted = true; // frames were lost right after it
    const auto ra = run(a, "untrusted");
    // The end of a chunk whose stream broke cannot be trusted: the last
    // segments, those still waiting for their lag (2) and the open one, are
    // dropped. The earlier ones were confirmed by what followed them.
    CHECK_EQ(ra.first.committed, uint64_t(9));
    CHECK_EQ(ra.first.quarantined, uint64_t(3));
    CHECK(ra.second > 0);

    Chunk b;
    b.seq = 1;
    b.data = s1;
    b.data[40] = 0xFE; // the decoder gives up in the first segment
    b.commit_end = b.data.size();
    const auto rb = run(b, "fatal");
    CHECK_EQ(rb.first.decode_fatal, uint64_t(1));
    // The decoder stopped in segment 0, which is dropped; the 11 after it were
    // never decoded, so they are neither kept nor counted as dropped. Nothing of
    // the chunk is kept, and the fatal error is the loss that says so.
    CHECK_EQ(rb.first.committed, uint64_t(0));
    CHECK_EQ(rb.first.quarantined, uint64_t(1));
    CHECK_EQ(rb.second, uint64_t(0));

    // all of it together, with the events the chunks cannot show
    Chunk b2 = b;
    CoverageEngine eng(m, nullptr, fake_config(2, 2));
    std::string err;
    CHECK(eng.start(err));
    eng.note_udp_gap(5);
    eng.note_dropped_bytes(1234);
    CHECK(eng.submit(std::move(a)));
    CHECK(eng.submit(std::move(b2)));
    eng.finish();
    const CoverageHealth h = eng.health();
    CHECK_EQ(h.udp_gaps, uint64_t(1));
    CHECK_EQ(h.udp_lost_frames, uint64_t(5));
    CHECK_EQ(h.dropped_bytes, uint64_t(1234));
    CHECK_EQ(h.decode_fatal, uint64_t(1));
    CHECK_EQ(h.losses(), uint64_t(2)); // the gap and the fatal chunk
    CHECK_EQ(h.committed, uint64_t(9));
    CHECK_EQ(h.quarantined, uint64_t(3 + 1));
}

TEST(engine_drops_chunks_when_the_workers_fall_behind_and_says_so)
{
    const CodeMap m = test_map();
    Rng rng { 4 };
    auto cfg = fake_config(1, 2);
    cfg.queue_chunks = 2;
    // a decoder that is slow, so the single worker holds up the queue
    cfg.make_decoder = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        return std::make_unique<FakeDecoder>();
    };
    CoverageEngine eng(m, nullptr, cfg);
    std::string err;
    CHECK(eng.start(err));
    int dropped = 0;
    uint64_t op = 0;
    for (int i = 0; i < 12; ++i) {
        auto s = make_stream(rng, 3, &op);
        Chunk c;
        c.seq = static_cast<uint64_t>(i);
        c.commit_end = s.size();
        c.data = std::move(s);
        if (!eng.submit(std::move(c)))
            ++dropped;
    }
    CHECK(dropped > 0);
    eng.finish();
    const CoverageHealth h = eng.health();
    CHECK_EQ(h.dropped_chunks, static_cast<uint64_t>(dropped));
    CHECK(h.dropped_bytes > 0);
    CHECK_EQ(eng.status().chunks_dropped, static_cast<uint64_t>(dropped));
    CHECK_EQ(eng.status().chunks_done, uint64_t(12 - dropped));
    // once finished, nothing more is taken
    Chunk late;
    late.data = { 1 };
    CHECK(!eng.submit(std::move(late)));
}

TEST(engine_start_failures_and_a_decoder_that_cannot_be_made)
{
    const CodeMap m = test_map();
    {
        CoverageEngine::Config cfg;
        cfg.workers = 2;
        CoverageEngine eng(m, nullptr, cfg); // default decoder, no program image
        std::string err;
        CHECK(!eng.start(err));
        CHECK(!err.empty());
    }
    {
        auto cfg = fake_config(2, 1);
        cfg.make_decoder = [] { return std::unique_ptr<IDecoder>(); };
        CoverageEngine eng(m, nullptr, cfg);
        std::string err;
        CHECK(eng.start(err));
        CHECK(eng.start(err)); // a second start changes nothing
        Rng rng { 6 };
        auto s = make_stream(rng, 3);
        Chunk c;
        c.commit_end = s.size();
        c.data = std::move(s);
        CHECK(eng.submit(std::move(c)));
        eng.finish();
        eng.finish(); // and so does a second finish
        CHECK_EQ(eng.health().decode_fatal, uint64_t(1));
    }
    {
        auto cfg = fake_config(1, 1);
        cfg.make_decoder = []() -> std::unique_ptr<IDecoder> { throw std::bad_alloc(); };
        CoverageEngine eng(m, nullptr, cfg);
        std::string err;
        CHECK(eng.start(err));
        Chunk c;
        c.data = { 1, 2, 3 };
        c.commit_end = 3;
        CHECK(eng.submit(std::move(c)));
        eng.finish();
        CHECK_EQ(eng.health().decode_fatal, uint64_t(1));
    }
}

TEST(engine_status_and_snapshot_while_running)
{
    const CodeMap m = test_map();
    Rng rng { 9 };
    CoverageEngine eng(m, nullptr, fake_config(2, 2));
    std::string err;
    CHECK(eng.start(err));
    uint64_t op = 0;
    for (int i = 0; i < 6; ++i) {
        auto s = make_stream(rng, 8, &op);
        Chunk c;
        c.seq = static_cast<uint64_t>(i);
        c.commit_end = s.size();
        c.data = std::move(s);
        CHECK(eng.submit(std::move(c)));
    }
    const std::string p = cortrace_test::temp_path("snap.json");
    CHECK(eng.write_json(p, "fw.elf")); // a snapshot while the workers run
    eng.finish();
    const auto st = eng.status();
    CHECK_EQ(st.workers, 2);
    CHECK_EQ(st.chunks_done, uint64_t(6));
    CHECK(st.bytes_done > 0);
    CHECK(st.instructions_hit >= 6);
    CHECK_EQ(st.busy, 0);
    std::remove(p.c_str());
}

#if defined(CORTRACE_HAVE_OPENCSD)
TEST(engine_default_decoder_is_opencsd_reading_the_shared_program_image)
{
    // The real decoder, wired the way cortrace-cov wires it: one ElfMemory, one
    // OpenCSD decoder per chunk. The stream is A-syncs only: it must decode
    // without a fatal error and give back a coverage with no hits.
    const CodeMap m = test_map();
    std::string err;
    auto elf = ElfMemory::from_bytes(elf_fixture::elf_with_segments({ { 0x100, 64, 64 } }), err);
    CHECK(elf != nullptr);
    CoverageEngine::Config cfg;
    cfg.workers = 2;
    cfg.lag = 1;
    CoverageEngine eng(m, elf, cfg);
    CHECK(eng.start(err));
    Rng rng { 12 };
    for (int i = 0; i < 4; ++i) {
        std::vector<uint8_t> s;
        for (int k = 0; k < 6; ++k) {
            s.insert(s.end(), 11, 0x00);
            s.push_back(0x80);
            for (int b = 0; b < 8; ++b)
                s.push_back(
                    static_cast<uint8_t>(0x01 + rng.next() % 3)); // not a packet that gives a range
        }
        Chunk c;
        c.seq = static_cast<uint64_t>(i);
        c.commit_end = s.size();
        c.data = std::move(s);
        CHECK(eng.submit(std::move(c)));
    }
    eng.finish();
    CHECK_EQ(eng.status().chunks_done, uint64_t(4));
    CHECK_EQ(eng.health().decode_fatal, uint64_t(0));
    CHECK_EQ(eng.status().instructions_hit, uint64_t(0));
}
#endif
