// Cortrace — Chunker unit tests.
//
// SPDX-License-Identifier: MIT
#include "cortrace/chunker.hpp"
#include "test_framework.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace cortrace;

namespace {

struct Rng {
    uint64_t s;
    uint32_t next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
};

// `n` segments, each an A-sync and 30..400 bytes that never hold 11 zeros.
std::vector<uint8_t> segments(Rng& rng, int n)
{
    std::vector<uint8_t> s;
    for (int i = 0; i < n; ++i) {
        s.insert(s.end(), 11, 0x00);
        s.push_back(0x80);
        const int len = 30 + static_cast<int>(rng.next() % 370);
        for (int k = 0; k < len; ++k)
            s.push_back(static_cast<uint8_t>(1 + rng.next() % 255));
    }
    return s;
}

std::vector<std::size_t> async_offsets(const std::vector<uint8_t>& d)
{
    std::vector<uint64_t> o;
    AsyncScanner().scan(d.data(), d.size(), 0, o);
    return std::vector<std::size_t>(o.begin(), o.end());
}

struct Collected {
    std::vector<Chunk> chunks;
    Chunker::Emit sink()
    {
        return [this](Chunk&& c) { chunks.push_back(std::move(c)); };
    }
};

void feed_split(Chunker& ch, const std::vector<uint8_t>& d, std::size_t piece)
{
    for (std::size_t at = 0; at < d.size(); at += piece)
        ch.feed(d.data() + at, std::min(piece, d.size() - at));
}

// The commit regions, joined.
std::vector<uint8_t> commit_regions(const std::vector<Chunk>& chunks)
{
    std::vector<uint8_t> out;
    for (const auto& c : chunks)
        out.insert(out.end(), c.data.begin(), c.data.begin() + static_cast<long>(c.commit_end));
    return out;
}

} // namespace

TEST(chunker_commit_regions_tile_the_stream_and_overlap_is_whole_segments)
{
    Rng rng { 5 };
    const auto stream = segments(rng, 400); // ~80 KB
    Collected got;
    Chunker ch(8000, 3, got.sink());
    feed_split(ch, stream, 1000);
    ch.finish();

    CHECK(got.chunks.size() >= 8);
    CHECK(commit_regions(got.chunks) == stream); // every byte once, in order
    for (std::size_t i = 0; i < got.chunks.size(); ++i) {
        const Chunk& c = got.chunks[i];
        CHECK_EQ(c.seq, uint64_t(i));
        CHECK(!c.tail_untrusted);
        const auto syncs = async_offsets(c.data);
        CHECK(!syncs.empty() && syncs[0] == 0); // starts at an A-sync
        // the commit region ends at an A-sync
        CHECK(std::find(syncs.begin(), syncs.end(), c.commit_end) != syncs.end()
            || c.commit_end == c.data.size());
        if (i + 1 < got.chunks.size()) {
            CHECK(c.commit_end >= 8000);
            // exactly 3 segments follow the commit region, all of them whole
            const std::size_t after = static_cast<std::size_t>(std::count_if(
                syncs.begin(), syncs.end(), [&](std::size_t s) { return s >= c.commit_end; }));
            CHECK_EQ(after, std::size_t(3));
            // and they are the start of the next chunk
            const Chunk& n = got.chunks[i + 1];
            CHECK(std::equal(
                c.data.begin() + static_cast<long>(c.commit_end), c.data.end(), n.data.begin()));
        } else {
            CHECK_EQ(c.commit_end, c.data.size());
        }
    }
}

TEST(chunker_result_does_not_depend_on_how_the_stream_is_fed)
{
    Rng rng { 11 };
    const auto stream = segments(rng, 300);
    Collected ref;
    {
        Chunker ch(5000, 2, ref.sink());
        ch.feed(stream.data(), stream.size());
        ch.finish();
    }
    CHECK(ref.chunks.size() > 5);
    for (std::size_t piece :
        { std::size_t(1), std::size_t(3), std::size_t(12), std::size_t(999) }) {
        Collected got;
        Chunker ch(5000, 2, got.sink());
        feed_split(ch, stream, piece);
        ch.finish();
        CHECK_EQ(got.chunks.size(), ref.chunks.size());
        for (std::size_t i = 0; i < std::min(got.chunks.size(), ref.chunks.size()); ++i) {
            CHECK(got.chunks[i].data == ref.chunks[i].data);
            CHECK_EQ(got.chunks[i].commit_end, ref.chunks[i].commit_end);
        }
    }
}

TEST(chunker_drops_what_comes_before_the_first_async)
{
    Rng rng { 3 };
    const auto stream = segments(rng, 40);
    std::vector<uint8_t> junk(777, 0x55);
    junk.insert(junk.end(), 7, 0x00); // zeros that are not an A-sync
    std::vector<uint8_t> in = junk;
    in.insert(in.end(), stream.begin(), stream.end());
    for (std::size_t piece : { std::size_t(1), std::size_t(64), in.size() }) {
        Collected got;
        Chunker ch(2000, 1, got.sink());
        feed_split(ch, in, piece);
        ch.finish();
        CHECK(commit_regions(got.chunks) == stream);
        CHECK_EQ(ch.dropped_bytes(), uint64_t(junk.size()));
    }
}

TEST(chunker_discontinuity_flushes_untrusted_and_restarts_at_the_next_async)
{
    Rng rng { 21 };
    const auto first = segments(rng, 60);
    const auto second = segments(rng, 60);
    const std::vector<uint8_t> hole(500, 0x42); // what arrives after the gap, mid-segment
    Collected got;
    Chunker ch(3000, 1, got.sink());
    ch.feed(first.data(), first.size());
    const std::size_t before = got.chunks.size();
    CHECK(before >= 1);
    ch.discontinuity();
    CHECK_EQ(got.chunks.size(), before + 1);
    const Chunk& last = got.chunks.back();
    CHECK(last.tail_untrusted);
    CHECK_EQ(last.commit_end, last.data.size()); // no overlap: nothing follows it
    CHECK_EQ(ch.held_bytes(), std::size_t(0));
    // the first stream is fully accounted for
    CHECK(commit_regions(got.chunks) == first);

    ch.feed(hole.data(), hole.size());
    ch.feed(second.data(), second.size());
    ch.finish();
    CHECK_EQ(ch.dropped_bytes(), uint64_t(hole.size()));
    std::vector<Chunk> after(got.chunks.begin() + static_cast<long>(before) + 1, got.chunks.end());
    CHECK(commit_regions(after) == second);
    for (const auto& c : after)
        CHECK(!c.tail_untrusted);
    // a discontinuity with nothing held emits nothing
    const std::size_t n = got.chunks.size();
    ch.discontinuity();
    CHECK_EQ(got.chunks.size(), n);
}

TEST(chunker_small_streams_and_zero_overlap)
{
    Rng rng { 8 };
    {
        Collected got;
        Chunker ch(1000, 2, got.sink());
        ch.finish(); // nothing fed
        CHECK(got.chunks.empty());
        const auto one = segments(rng, 1);
        ch.feed(one.data(), one.size());
        CHECK(got.chunks.empty()); // not long enough to cut
        ch.finish();
        CHECK_EQ(got.chunks.size(), std::size_t(1));
        CHECK(got.chunks[0].data == one);
        CHECK_EQ(got.chunks[0].seq, uint64_t(0));
        CHECK_EQ(ch.chunks(), uint64_t(1));
    }
    {
        const auto stream = segments(rng, 120);
        Collected got;
        Chunker ch(1500, 0, got.sink());
        feed_split(ch, stream, 700);
        ch.finish();
        CHECK(got.chunks.size() > 3);
        for (const auto& c : got.chunks)
            CHECK_EQ(c.commit_end, c.data.size()); // no overlap
        CHECK(commit_regions(got.chunks) == stream);
    }
}

TEST(chunker_gives_up_when_no_async_comes)
{
    Rng rng { 13 };
    auto stream = segments(rng, 3);
    std::vector<uint8_t> noise(100000, 0x77); // far more than 64 chunk sizes of 1000
    Collected got;
    Chunker ch(1000, 1, got.sink());
    ch.feed(stream.data(), stream.size());
    ch.feed(noise.data(), noise.size());
    CHECK(ch.dropped_bytes() >= stream.size()); // what was held is thrown away
    CHECK_EQ(ch.held_bytes(), std::size_t(0));
    const auto next = segments(rng, 30);
    ch.feed(next.data(), next.size());
    ch.finish();
    CHECK(!got.chunks.empty());
    CHECK(commit_regions(got.chunks) == next); // it starts again at the next A-sync
}
