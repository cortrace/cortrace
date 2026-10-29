// Cortrace — SeqTracker unit tests.
//
// SPDX-License-Identifier: MIT
#include "cortrace/seq_tracker.hpp"
#include "test_framework.hpp"

using namespace cortrace;
using V = SeqTracker::Verdict;

TEST(seq_tracker_in_order_frames_are_taken)
{
    SeqTracker t;
    for (uint32_t s = 100; s < 200; ++s)
        CHECK(t.next(s) == V::Take);
    CHECK_EQ(t.skipped(), uint64_t(0));
    CHECK_EQ(t.restarts(), uint64_t(0));
}

TEST(seq_tracker_a_jump_forward_is_frames_lost)
{
    SeqTracker t;
    CHECK(t.next(10) == V::Take);
    CHECK(t.next(11) == V::Take);
    CHECK(t.next(40) == V::TakeAfterGap);
    CHECK_EQ(t.lost(), uint64_t(28));
    CHECK(!t.restarted());
    CHECK(t.next(41) == V::Take);
}

TEST(seq_tracker_repeats_and_late_frames_are_skipped_and_do_not_move_it)
{
    SeqTracker t;
    CHECK(t.next(50) == V::Take);
    CHECK(t.next(51) == V::Take);
    CHECK(t.next(51) == V::Skip); // repeat
    CHECK(t.next(49) == V::Skip); // late
    CHECK(t.next(52) == V::Take); // and the stream goes on from where it was
    CHECK_EQ(t.skipped(), uint64_t(2));
}

TEST(seq_tracker_wraps_around_at_32_bits)
{
    SeqTracker t;
    CHECK(t.next(0xFFFFFFFEu) == V::Take);
    CHECK(t.next(0xFFFFFFFFu) == V::Take);
    CHECK(t.next(0u) == V::Take);
    CHECK(t.next(1u) == V::Take);
    CHECK(t.next(5u) == V::TakeAfterGap);
    CHECK_EQ(t.lost(), uint64_t(3));
}

TEST(seq_tracker_a_sequence_that_starts_over_is_followed_not_dropped)
{
    // The failure seen on the board after 7.5 minutes: the FPGA's numbers fell
    // back from about 33 million to 0 and every frame after that looked older
    // than the last one taken. The capture went on for minutes with no data.
    SeqTracker t;
    for (uint32_t s = 33000000; s < 33000100; ++s)
        CHECK(t.next(s) == V::Take);
    // the numbers are nowhere near the old ones: it is a restart at once
    CHECK(t.next(7) == V::TakeAfterGap);
    CHECK(t.restarted());
    CHECK_EQ(t.lost(), uint64_t(1)); // how many went missing is not known
    for (uint32_t s = 8; s < 5000; ++s)
        CHECK(t.next(s) == V::Take);
    CHECK_EQ(t.restarts(), uint64_t(1));
    CHECK_EQ(t.skipped(), uint64_t(0));
}

TEST(seq_tracker_a_long_run_of_slightly_late_frames_is_a_restart_too)
{
    // Not far off, so it could be reordering: wait for a whole run of them.
    SeqTracker t;
    CHECK(t.next(5000) == V::Take);
    CHECK(t.next(5001) == V::Take);
    int skipped = 0;
    uint32_t s = 4000;
    V v = V::Skip;
    for (; s < 4000 + 1000 && v == V::Skip; ++s) {
        v = t.next(s);
        skipped += v == V::Skip;
    }
    CHECK(v == V::TakeAfterGap);
    CHECK(t.restarted());
    CHECK_EQ(skipped, int(SeqTracker::kResyncAfter) - 1);
    for (uint32_t k = s; k < s + 3000; ++k) // and it goes on from there
        CHECK(t.next(k) == V::Take);
}

TEST(seq_tracker_a_huge_jump_forward_is_a_restart_not_four_billion_lost_frames)
{
    SeqTracker t;
    CHECK(t.next(100) == V::Take);
    CHECK(t.next(100 + SeqTracker::kMaxGap + 5) == V::TakeAfterGap);
    CHECK(t.restarted());
    CHECK_EQ(t.lost(), uint64_t(1));
    CHECK(t.next(100 + SeqTracker::kMaxGap + 6) == V::Take);
    CHECK_EQ(t.restarts(), uint64_t(1));
}

TEST(seq_tracker_a_few_reordered_frames_never_trigger_a_restart)
{
    SeqTracker t;
    CHECK(t.next(1000) == V::Take);
    uint32_t s = 1001;
    for (int round = 0; round < 200; ++round) {
        CHECK(t.next(s + 2) == V::TakeAfterGap); // 2 arrives first ...
        CHECK(t.next(s + 1) == V::Skip); // ... then 1: late
        CHECK(t.next(s + 3) == V::Take);
        s += 3;
    }
    CHECK_EQ(t.restarts(), uint64_t(0)); // the late frames were never 64 in a row
}
