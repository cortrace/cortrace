// Cortrace — object-code coverage from the decoded instruction flow.
//
// CoverageSink sits next to the call-stack machine on the element stream and
// marks every instruction an InstrRange covers in a CodeMap. Its rule is to
// never report as executed what the trace does not prove, because the capture
// link has no CRC and a flipped bit can still decode:
//
//   * The ETM stream is cut into segments at its A-sync packets (the decoder
//     re-synchronises there). A segment's hits are kept aside and merged only
//     when the segment is complete and clean.
//   * A segment is dirty if it holds a loss (lost sync, address not
//     accessible, a later trace-on, overflow) or a range that disagrees with
//     the static code: outside the code, not on instruction boundaries, a
//     different instruction count, a different kind of final branch, a start
//     that is not where the previous branch (or exception entry / return) must
//     have gone, or a function return that does not land after a call.
//   * A loss also dirties the `commit_lag` segments before it, since the bad
//     bytes may have started there. Dirty segments are counted, not merged.
//
// So the result is "executed in clean trace"; not hit means not observed.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_COVERAGE_HPP
#define CORTRACE_COVERAGE_HPP

#include "cortrace/codemap.hpp"
#include "cortrace/element.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cortrace {

// Finds ETMv4 A-sync packets (eleven 0x00 then 0x80) in a stream fed in
// chunks, a packet split across two chunks included.
class AsyncScanner {
public:
    // Appends the stream offset of the first byte of each A-sync in
    // `data[0..len)`, where data[0] is at stream offset `base`.
    void scan(const uint8_t* data, std::size_t len, uint64_t base, std::vector<uint64_t>& out);

private:
    uint64_t zeros_ = 0;
};

struct CoverageHealth {
    uint64_t segments = 0; // segments seen (including empty ones)
    uint64_t committed = 0; // clean segments merged
    uint64_t quarantined = 0; // dirty segments dropped
    uint64_t quarantined_bytes = 0; // ETM bytes in the dropped segments
    uint64_t ranges = 0; // instruction ranges seen
    uint64_t ranges_committed = 0;
    // losses reported by the decoder
    uint64_t lost_sync = 0, addr_nacc = 0, trace_on = 0, overflow = 0;
    // cross-checks against the static code
    uint64_t outside_code = 0; // range not on instruction boundaries of the code map
    uint64_t count_mismatch = 0; // decoder's instruction count != static count
    uint64_t kind_mismatch = 0; // final branch kind != static kind
    uint64_t cond_mismatch = 0; // final instruction conditional flag != static flag
    uint64_t flow_mismatch = 0; // range did not start where the previous one led
    uint64_t flow_checked = 0; // ranges whose start could be checked
    uint64_t return_mismatch = 0; // a function return not to its caller (or not after a call)
    uint64_t return_checked = 0;
    // Exception returns that went somewhere else than where the exception was
    // taken: what an OS thread switch looks like. Counted, not a fault.
    uint64_t exception_return_elsewhere = 0;
    uint64_t cond_outcomes = 0; // conditional waypoints seen (taken or not)
    // Streaming capture (cortrace-cov), counted by the pipeline, not the sink:
    uint64_t udp_gaps = 0; // places where UDP frames were lost
    uint64_t udp_lost_frames = 0; // frames missing at those places
    uint64_t dropped_chunks = 0; // chunks given up because the decoders fell behind
    uint64_t dropped_bytes = 0; // ETM bytes in the dropped chunks / before the first A-sync
    uint64_t decode_fatal = 0; // chunks on which the decoder stopped with a fatal error

    uint64_t losses() const
    {
        return lost_sync + addr_nacc + trace_on + overflow + udp_gaps + dropped_chunks
            + decode_fatal;
    }
    uint64_t mismatches() const
    {
        return outside_code + count_mismatch + kind_mismatch + cond_mismatch + flow_mismatch
            + return_mismatch;
    }

    // Every counter by name, for JSON output and for adding two healths up.
    struct Field {
        const char* name;
        uint64_t CoverageHealth::*ptr;
    };
    static const std::vector<Field>& fields();
    void add(const CoverageHealth& o);
};

class CoverageSink {
public:
    explicit CoverageSink(const CodeMap& map, int commit_lag = 1);

    // Declare that a segment starts at ETM byte `offset` (an A-sync). Call in
    // increasing order, before the elements decoded from that segment.
    void add_segment_start(uint64_t offset);

    void on_element(const Element& e);

    // Chunked decoding (cortrace-cov): the stream of this sink is a chunk
    // [0, commit_limit) that is followed by an overlap of a few more segments.
    // The overlap is decoded so that a loss shortly after the chunk can still
    // drop its last segments, but it belongs to the next chunk: its segments
    // are neither merged nor counted, and the events in it (losses, failed
    // checks) are left to the chunk that owns it. Default: no limit.
    void set_commit_limit(uint64_t limit) { commit_limit_ = limit; }

    // The stream does not continue as it was (frames lost after this point, or
    // the decoder gave up): nothing decoded so far can be trusted to its end.
    void mark_discontinuity();

    // End of the stream: close and decide the open segments. `total_bytes` is
    // the stream length (used for the size of the last segment).
    void finish(uint64_t total_bytes);

    const CodeMap& map() const { return map_; }
    // With a commit limit: the events up to the limit, the commits of the
    // segments below it.
    CoverageHealth health() const;
    // Per instruction of map().insns():
    const std::vector<uint32_t>& hits() const { return hits_; } // executions (saturating)
    const std::vector<uint16_t>& segments_seen() const { return segs_; } // distinct segments
    const std::vector<uint32_t>& taken() const { return taken_; } // conditional waypoints
    const std::vector<uint32_t>& not_taken() const { return not_taken_; }
    const std::vector<uint16_t>& taken_segments() const { return taken_segs_; } // distinct
    const std::vector<uint16_t>& not_taken_segments() const { return not_taken_segs_; }
    // Indirect branch / call site -> targets seen.
    const std::map<uint32_t, std::set<uint32_t>>& indirect_targets() const { return indirect_; }

    // Object coverage as JSON (format "cortrace-objcov" version 1), the input
    // of `cortrace cover`. The file is written under a temporary name and
    // renamed, so a reader never sees half of it. Returns false on failure.
    bool write_json(const std::string& path, const std::string& elf_path) const;

private:
    struct Segment {
        uint64_t id = 0;
        uint64_t start = 0;
        uint64_t end = 0;
        bool dirty = false;
        uint64_t ranges = 0;
        std::vector<std::pair<uint32_t, uint32_t>> spans; // instruction index [first, last]
        std::vector<std::pair<uint32_t, bool>> outcomes; // (instruction index, taken)
        std::vector<std::pair<uint32_t, uint32_t>> indirect; // (site addr, target addr)
    };

    void advance_to(uint64_t byte_index);
    void close_open(uint64_t end);
    void commit(Segment& s);
    void lose();
    void taint();
    void dirty_current();
    void on_range(const Element& e);
    void push_call(uint32_t ret);
    void on_exception(const Element& e);
    void on_exception_return();
    void reset_flow();

    static constexpr std::size_t kMaxExceptionDepth = 64;
    static constexpr std::size_t kMaxCallDepth = 4096;
    static constexpr uint32_t kExceptionMark = 0xFFFFFFFFu; // never a Thumb return address

    const CodeMap& map_;
    int lag_;
    CoverageHealth health_;
    uint64_t commit_limit_ = UINT64_MAX;
    bool frozen_ = false; // the open segment is past commit_limit_
    CoverageHealth frozen_health_; // health_ at the moment it got there
    std::vector<uint32_t> hits_, taken_, not_taken_;
    std::vector<uint16_t> segs_, taken_segs_, not_taken_segs_;
    // id of the last segment that counted, per instruction / outcome
    std::vector<uint64_t> last_seg_, last_taken_seg_, last_not_taken_seg_;
    std::map<uint32_t, std::set<uint32_t>> indirect_;

    std::deque<uint64_t> starts_; // announced segment starts not reached yet
    Segment open_;
    std::deque<Segment> closed_; // waiting for `lag_` successors
    uint64_t next_id_ = 1;
    bool seen_range_ = false;

    // What the start of the next range is checked against.
    enum class Expect {
        None, // nothing known (indirect branch, after a loss, ...)
        Exact, // must be expect_ (direct branch, fall-through, exception vector)
        Return, // must be expect_, the return address of the matching call
        AfterCall, // a return with no known caller: right after some call
        ExceptionReturn, // should be expect_; elsewhere means a thread switch
    };
    Expect expect_kind_ = Expect::None;
    uint32_t expect_ = 0;
    long indirect_site_ = -1; // instruction index of a taken indirect branch
    std::vector<uint32_t> calls_; // shadow call stack: return addresses, kExceptionMark
    // The frames below this depth were pushed before the last return from an
    // exception to thread level. An OS may have switched threads there (the
    // resumed PC can equal the interrupted one, so it cannot be told apart),
    // and then they belong to another thread: returns through them are only
    // checked to land after a call, not against the exact address.
    std::size_t soft_depth_ = 0;
    std::deque<uint32_t> exc_returns_; // return addresses of the pending exceptions
};

// The sum of many sinks, e.g. one per chunk of a long capture. 64-bit counts:
// a sink's own counters saturate at 32 / 16 bits, which an hours-long run
// would reach. Segment counts add up because the chunks' committed segments
// are disjoint. Not thread-safe: the caller serialises merge() and reads.
class CoverageAccumulator {
public:
    CoverageAccumulator(const CodeMap& map, int commit_lag);

    // Adds the counts and the health of `s` (a sink of the same CodeMap).
    void merge(const CoverageSink& s);
    // Adds events the sinks cannot see (lost UDP frames, dropped chunks ...).
    void add_health(const CoverageHealth& h) { health_.add(h); }

    const CodeMap& map() const { return map_; }
    const CoverageHealth& health() const { return health_; }
    const std::vector<uint64_t>& hits() const { return hits_; }
    uint64_t instructions_hit() const;

    // Same file as CoverageSink::write_json.
    bool write_json(const std::string& path, const std::string& elf_path) const;

private:
    const CodeMap& map_;
    int lag_;
    CoverageHealth health_;
    std::vector<uint64_t> hits_, taken_, not_taken_, segs_, taken_segs_, not_taken_segs_;
    std::map<uint32_t, std::set<uint32_t>> indirect_;
};

} // namespace cortrace

#endif // CORTRACE_COVERAGE_HPP
