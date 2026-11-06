// Cortrace — call-graph, per-function and per-thread statistics.
//
// Consumes the slice stream of the CallStackMachine (begin/end events with the
// estimated CPU-cycle clock, SliceEvent::cycle_est) and the thread switches, and computes:
//   - per function: calls, inclusive and self cycles, p50/p90/p99 of the
//     inclusive time;
//   - per caller -> callee edge: calls and inclusive cycles;
//   - folded stacks (the input of flamegraph.pl, speedscope, inferno);
//   - per thread: cycles it ran, cycles ISRs took from it, scheduling
//     intervals, the longest interval.
//
// For a recursive function a call adds to the inclusive total of the function
// (and of its edge) only the time that is not already in an intact call of the
// same function nested in it, so the total is the time the function was on the
// stack, counted once; the edges of a function add up to that total. The
// min/max/percentiles describe every call.
//
// Time is the CPU-cycle clock, so the trace needs cycle counts (ETM profile
// `timing`). A function's time is the time its CONTEXT ran while the call was
// open: the time of other threads and of ISRs that preempted it is not in it.
//
// A call is only counted when the trace is intact between its begin and its
// end. Calls that span lost trace (NoSync, TraceOn, AddrNacc, Overflow), and
// calls that the machine had to close because the trace ended, are counted
// apart and add no time. Cycles between two events with lost trace in between
// are dropped, not credited to anyone.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_CALLSTATS_HPP
#define CORTRACE_CALLSTATS_HPP

#include "cortrace/callstack.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cortrace {

struct CallFunctionStat {
    std::string name;
    uint64_t calls = 0; // completed calls with intact trace
    uint64_t crossed = 0; // calls that spanned lost trace
    uint64_t truncated = 0; // calls still open when the trace (or boot) ended
    uint64_t incl = 0; // inclusive cycles of the calls (recursion counted once)
    uint64_t self = 0; // cycles not spent in callees
    uint64_t incl_min = 0;
    uint64_t incl_max = 0;
    uint64_t p50 = 0;
    uint64_t p90 = 0;
    uint64_t p99 = 0;
};

struct CallEdgeStat {
    std::string caller; // "<root>" for the outermost call of a context
    std::string callee;
    uint64_t calls = 0;
    uint64_t incl = 0;
};

struct CallThreadStat {
    int id = kThreadBoot;
    std::string name;
    uint64_t run = 0; // cycles the thread ran
    uint64_t isr = 0; // cycles ISRs took while it was the current thread
    uint64_t isr_entries = 0; // exceptions that preempted it
    uint64_t intervals = 0; // times it was switched in
    uint64_t longest = 0; // longest interval (run + isr) with intact trace
    uint64_t lossy_intervals = 0; // intervals with lost trace, or cut by the trace end
};

struct CallStats {
    std::vector<CallFunctionStat> functions; // by inclusive cycles, descending
    std::vector<CallEdgeStat> edges; // by inclusive cycles, descending
    std::vector<CallThreadStat> threads; // by run cycles, descending
    std::map<std::string, uint64_t> folded; // "thread;f;g" -> self cycles
    uint64_t credited = 0; // cycles credited to a running context
    uint64_t dropped = 0; // cycles skipped because trace was lost in between
    uint64_t losses = 0; // lost-trace events
    uint64_t crossed_frames = 0;
    uint64_t truncated_frames = 0;

    // JSON, format "cortrace-callstats". `sysclk_hz` is only recorded.
    std::string to_json(const std::string& elf_path, double sysclk_hz) const;
    // One line per stack, "a;b;c <self cycles>", the folded-stack format.
    std::string folded_text() const;
};

// `switches` are the thread switches of the machine, `names` maps a thread id
// to its name (missing ids get a generic name).
CallStats compute_callstats(const std::vector<SliceEvent>& slices,
    const std::vector<ThreadSwitch>& switches, const std::map<int, std::string>& names);

} // namespace cortrace

#endif // CORTRACE_CALLSTATS_HPP
