// Cortrace — follow the sequence numbers of the FPGA's UDP frames.
//
// Each frame carries a 32-bit number that goes up by one. A jump forward is
// frames lost, one backward is a late or repeated frame. Neither is the whole
// story: the sequence can also start over (the FPGA's streamer restarts after a
// stall), and a tracker that only knows "newer" and "older" then calls every
// frame that follows older than the last one it took, and drops them all, for
// as long as the capture runs. So a long run of "older" frames is taken as a
// restart: the tracker follows the new numbers.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_SEQ_TRACKER_HPP
#define CORTRACE_SEQ_TRACKER_HPP

#include <cstdint>

namespace cortrace {

class SeqTracker {
public:
    enum class Verdict {
        Take, // the next frame (or the first one)
        TakeAfterGap, // frames are missing before it: see lost()
        Skip, // a repeat or a late frame: its place has already passed
    };

    // Forward jumps up to this many frames are loss; a bigger one is a restart.
    static constexpr uint32_t kMaxGap = 1u << 20;
    // This many frames in a row that look late: the sequence has restarted.
    static constexpr uint32_t kResyncAfter = 64;

    Verdict next(uint32_t seq);

    // Frames missing before the last TakeAfterGap frame. After a restart the
    // number is unknown and 1 is reported.
    uint64_t lost() const { return lost_; }
    // The last verdict was a restart of the sequence.
    bool restarted() const { return restarted_; }

    uint64_t skipped() const { return skipped_; }
    uint64_t restarts() const { return restarts_; }

private:
    bool have_prev_ = false;
    uint32_t prev_ = 0;
    uint32_t late_run_ = 0; // consecutive frames that looked late
    uint64_t lost_ = 0;
    bool restarted_ = false;
    uint64_t skipped_ = 0;
    uint64_t restarts_ = 0;
};

} // namespace cortrace

#endif // CORTRACE_SEQ_TRACKER_HPP
