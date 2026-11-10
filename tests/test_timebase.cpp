// Cortrace — TimeBase unit tests.
//
// SPDX-License-Identifier: MIT
#include "cortrace/callstack.hpp"
#include "cortrace/timebase.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace cortrace;

namespace {
// Write a temp time.bin of little-endian uint64 ns values and return its path.
std::string write_tb(const std::vector<uint64_t>& ns)
{
    std::string path = cortrace_test::temp_path("tb.bin");
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(ns.data(), sizeof(uint64_t), ns.size(), f);
    std::fclose(f);
    return path;
}
} // namespace

TEST(timebase_lookup_and_clamp)
{
    TimeBase tb;
    CHECK(tb.empty());
    CHECK_EQ((long)tb.ns_for(5), 0L); // empty -> 0

    std::string p = write_tb({ 100, 200, 300, 400 });
    std::size_t n = tb.load(p);
    CHECK_EQ((long)n, 4L);
    CHECK(!tb.empty());
    CHECK_EQ((long)tb.ns_for(0), 100L);
    CHECK_EQ((long)tb.ns_for(2), 300L);
    // past the end clamps to the last entry
    CHECK_EQ((long)tb.ns_for(99), 400L);
}

TEST(timebase_assign_replaces_the_table)
{
    TimeBase tb;
    CHECK(tb.empty());
    tb.assign({ 0, 24, 48 });
    CHECK_EQ(tb.size(), 3u);
    CHECK_EQ(tb.ns_for(1), 24u);
    CHECK_EQ(tb.ns_for(100), 48u); // past the end: the last entry
}
TEST(timebase_load_missing_throws)
{
    TimeBase tb;
    bool threw = false;
    try {
        tb.load("/nonexistent/path/xyz.bin");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(timebase_apply_replaces_tick_with_ns)
{
    // slice byte_index -> ns via the table; tick field is overwritten.
    std::vector<SliceEvent> slices = {
        { 0, 0, true, "f", 0 }, // byte_index 0 -> 100 ns
        { 1, 2, false, "", 0 }, // byte_index 2 -> 300 ns
    };
    TimeBase tb;
    tb.load(write_tb({ 100, 200, 300 }));
    auto out = apply_timebase(slices, tb);
    CHECK_EQ((long)out.size(), 2L);
    CHECK_EQ((long)out[0].tick, 100L);
    CHECK_EQ((long)out[1].tick, 300L);
    // track field is preserved through apply_timebase
    CHECK_EQ(out[0].track, 0);
}

TEST(timebase_apply_empty_is_identity)
{
    std::vector<SliceEvent> slices = { { 7, 0, true, "f", 1 } };
    TimeBase tb; // empty
    auto out = apply_timebase(slices, tb);
    CHECK_EQ((long)out[0].tick, 7L); // unchanged (monotonic tick order kept)
    CHECK_EQ(out[0].track, 1);
}

TEST(timebase_apply_is_non_decreasing)
{
    // Even if the table maps a later event to a smaller ns, apply_timebase
    // clamps so timestamps never go backwards (Perfetto requires monotonic).
    std::vector<SliceEvent> slices = {
        { 0, 1, true, "a", 0 }, // -> 500
        { 1, 0, false, "", 0 }, // -> 100, but must clamp up to >= 500
    };
    TimeBase tb;
    tb.load(write_tb({ 100, 500 }));
    auto out = apply_timebase(slices, tb);
    CHECK(out[1].tick >= out[0].tick);
}

TEST(etm_timestamp_raw_counts_become_ticks)
{
    // With tsgen_hz=0 the raw TSGEN count is used directly as the tick.
    // SliceEvent = { tick, byte_index, begin, name, track, etm_ts }.
    std::vector<SliceEvent> slices = {
        { 0, 0, true, "a", 0, 1000 },
        { 0, 0, false, "a", 0, 2000 },
    };
    auto out = apply_etm_timestamp(slices, 0.0);
    CHECK_EQ((long)out[0].tick, 1000L);
    CHECK_EQ((long)out[1].tick, 2000L);
}

TEST(etm_timestamp_converts_counts_to_ns)
{
    // tsgen_hz set => tick = count * 1e9 / hz. At 1 MHz, 1 count = 1000 ns.
    std::vector<SliceEvent> slices = {
        { 0, 0, true, "a", 0, 1 },
        { 0, 0, false, "a", 0, 5 },
    };
    auto out = apply_etm_timestamp(slices, 1e6);
    CHECK_EQ((long)out[0].tick, 1000L);
    CHECK_EQ((long)out[1].tick, 5000L);
}

TEST(etm_timestamp_clamps_non_decreasing)
{
    // A backwards TSGEN value (decode glitch) must never move time backwards.
    std::vector<SliceEvent> slices = {
        { 0, 0, true, "a", 0, 5000 }, { 0, 0, false, "a", 0, 4000 }, // must clamp up to >= 5000
    };
    auto out = apply_etm_timestamp(slices, 0.0);
    CHECK(out[1].tick >= out[0].tick);
    CHECK_EQ((long)out[1].tick, 5000L);
}

TEST(etm_timestamp_shared_value_keeps_order)
{
    // Many consecutive slices share one TS value (TS only refreshes at packets);
    // equal ticks are fine and preserve emission order.
    std::vector<SliceEvent> slices = {
        { 0, 0, true, "a", 0, 7 },
        { 0, 0, true, "b", 0, 7 },
        { 0, 0, false, "b", 0, 7 },
    };
    auto out = apply_etm_timestamp(slices, 0.0);
    CHECK_EQ((long)out[0].tick, 7L);
    CHECK_EQ((long)out[1].tick, 7L);
    CHECK_EQ((long)out[2].tick, 7L);
}

// ---- hybrid time base ------------------------------------------------------
namespace {
SliceEvent hy_ev(uint64_t etm_ts, uint64_t cycles)
{
    SliceEvent e;
    e.etm_ts = etm_ts;
    e.cycle_clock = cycles;
    return e;
}
} // namespace

TEST(hybrid_time_interpolates_between_anchors_by_cycles)
{
    // TSGEN 75 MHz (13.33 ns/count), CPU 150 MHz (6.67 ns/cycle): 150 cycles = 1000 ns.
    std::vector<SliceEvent> s = { hy_ev(75, 0), hy_ev(75, 150), hy_ev(75, 300), hy_ev(300, 450) };
    auto out = apply_hybrid_time(s, 75e6, 150e6);
    CHECK_EQ((long long)out[0].tick, 1000LL); // 75 counts = 1000 ns
    CHECK_EQ((long long)out[1].tick, 2000LL); // +150 cycles = +1000 ns
    CHECK_EQ((long long)out[2].tick, 3000LL);
    CHECK_EQ((long long)out[3].tick, 4000LL); // next anchor: 300 counts = 4000 ns
}

TEST(hybrid_time_jumps_to_the_next_anchor_across_a_sleep)
{
    // The cycle counter saw 3000 ns of run time but TSGEN moved 100000 ns (the
    // core slept in WFI): the next anchor is the truth for what follows.
    std::vector<SliceEvent> s = { hy_ev(75, 0), hy_ev(75, 450), hy_ev(7575, 450) };
    auto out = apply_hybrid_time(s, 75e6, 150e6);
    CHECK_EQ((long long)out[1].tick, 4000LL);
    CHECK_EQ((long long)out[2].tick, 101000LL);
}

TEST(hybrid_time_never_passes_the_next_anchor)
{
    // More cycles than TSGEN time (clock skew): clamp, stay monotonic.
    std::vector<SliceEvent> s = { hy_ev(75, 0), hy_ev(75, 3000), hy_ev(150, 3000) };
    auto out = apply_hybrid_time(s, 75e6, 150e6);
    CHECK_EQ((long long)out[1].tick, 2000LL); // clamped to the next anchor (150 counts)
    CHECK_EQ((long long)out[2].tick, 2000LL);
}

TEST(hybrid_time_places_events_before_the_first_timestamp)
{
    std::vector<SliceEvent> s = { hy_ev(0, 0), hy_ev(0, 150), hy_ev(75, 300) };
    auto out = apply_hybrid_time(s, 75e6, 150e6);
    CHECK_EQ((long long)out[2].tick, 1000LL);
    CHECK(out[0].tick <= out[1].tick);
    CHECK(out[1].tick <= out[2].tick);
}

TEST(hybrid_time_without_rates_falls_back_to_etm_timestamp)
{
    std::vector<SliceEvent> s = { hy_ev(75, 0), hy_ev(150, 100) };
    auto out = apply_hybrid_time(s, 0.0, 150e6);
    CHECK_EQ((long long)out[0].tick, 75LL); // raw counts
}

// ---- cycle-count time base ---------------------------------------------------

TEST(cycle_time_converts_cycles_to_ns)
{
    // 150 MHz: 150 cycles = 1000 ns.
    std::vector<SliceEvent> s = { hy_ev(0, 0), hy_ev(0, 150), hy_ev(0, 300) };
    auto out = apply_cycle_time(s, 150e6);
    CHECK_EQ((long long)out[0].tick, 0LL);
    CHECK_EQ((long long)out[1].tick, 1000LL);
    CHECK_EQ((long long)out[2].tick, 2000LL);
}

TEST(cycle_time_without_a_rate_keeps_raw_cycles)
{
    std::vector<SliceEvent> s = { hy_ev(0, 7), hy_ev(0, 9) };
    auto out = apply_cycle_time(s, 0.0);
    CHECK_EQ((long long)out[0].tick, 7LL);
    CHECK_EQ((long long)out[1].tick, 9LL);
}

TEST(cycle_time_never_moves_backwards)
{
    // A decode glitch makes the counter go back: hold the previous tick.
    std::vector<SliceEvent> s = { hy_ev(0, 300), hy_ev(0, 150), hy_ev(0, 450) };
    auto out = apply_cycle_time(s, 150e6);
    CHECK_EQ((long long)out[0].tick, 2000LL);
    CHECK_EQ((long long)out[1].tick, 2000LL); // clamped, not 1000
    CHECK_EQ((long long)out[2].tick, 3000LL);
}

TEST(cycle_time_of_nothing_is_nothing) { CHECK(apply_cycle_time({}, 150e6).empty()); }

// ---- hybrid corner cases -----------------------------------------------------

TEST(hybrid_time_of_nothing_is_nothing) { CHECK(apply_hybrid_time({}, 75e6, 150e6).empty()); }

TEST(hybrid_time_without_any_timestamp_is_pure_cycle_time_from_zero)
{
    std::vector<SliceEvent> s = { hy_ev(0, 100), hy_ev(0, 250), hy_ev(0, 400) };
    auto out = apply_hybrid_time(s, 75e6, 150e6);
    CHECK_EQ((long long)out[0].tick, 0LL);
    CHECK_EQ((long long)out[1].tick, 1000LL); // 150 cycles later
    CHECK_EQ((long long)out[2].tick, 2000LL);
}

TEST(hybrid_time_cycle_counter_going_back_inside_a_group_holds_the_base)
{
    // Same TS group, but the cycle counter reads lower than at the anchor.
    std::vector<SliceEvent> s = { hy_ev(75, 300), hy_ev(75, 150) };
    auto out = apply_hybrid_time(s, 75e6, 150e6);
    CHECK_EQ((long long)out[0].tick, 1000LL);
    CHECK_EQ((long long)out[1].tick, 1000LL); // not before the anchor
}

TEST(hybrid_time_group_without_timestamp_after_one_stays_monotonic)
{
    // The TS drops back to 0 after a real one (a decoder resync): the group
    // without a timestamp is placed from the first real anchor by cycles, and
    // must not land before what was already emitted.
    std::vector<SliceEvent> s = { hy_ev(75, 100), hy_ev(75, 200), hy_ev(0, 150) };
    auto out = apply_hybrid_time(s, 75e6, 150e6);
    CHECK_EQ((long long)out[0].tick, 1000LL);
    CHECK_EQ((long long)out[1].tick, 1666LL); // +100 cycles = 666 ns
    CHECK(out[2].tick >= out[1].tick);
}

TEST(hybrid_time_events_before_the_first_timestamp_clamp_at_zero)
{
    // Counting back from the first anchor would go below zero.
    std::vector<SliceEvent> s = { hy_ev(0, 0), hy_ev(75, 3000) };
    auto out = apply_hybrid_time(s, 75e6, 150e6);
    CHECK_EQ((long long)out[0].tick, 0LL);
    CHECK_EQ((long long)out[1].tick, 1000LL);
}
