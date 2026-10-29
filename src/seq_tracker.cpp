// Cortrace — SeqTracker implementation (see seq_tracker.hpp).
//
// SPDX-License-Identifier: MIT
#include "cortrace/seq_tracker.hpp"

namespace cortrace {

SeqTracker::Verdict SeqTracker::next(uint32_t seq)
{
    restarted_ = false;
    lost_ = 0;
    if (!have_prev_) {
        have_prev_ = true;
        prev_ = seq;
        return Verdict::Take;
    }
    const uint32_t d = seq - prev_; // modulo 2^32
    if (d == 1) {
        prev_ = seq;
        late_run_ = 0;
        return Verdict::Take;
    }
    if (d >= 2 && d <= kMaxGap) { // a few frames missing
        prev_ = seq;
        late_run_ = 0;
        lost_ = d - 1;
        return Verdict::TakeAfterGap;
    }
    // d is 0 (a repeat), a little behind (a late frame), or a long way off.
    const bool behind = d == 0 || d >= (0u - kMaxGap);
    if (behind && ++late_run_ < kResyncAfter) {
        ++skipped_;
        return Verdict::Skip;
    }
    // Far ahead, or behind for a whole run of frames: this is not the sequence
    // we were following. Follow the new numbers.
    prev_ = seq;
    late_run_ = 0;
    lost_ = 1;
    restarted_ = true;
    ++restarts_;
    return Verdict::TakeAfterGap;
}

} // namespace cortrace
