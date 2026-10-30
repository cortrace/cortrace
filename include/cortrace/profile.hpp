// Cortrace — cycle-accurate profile from the ETM cycle counts.
//
// With cycle counting on (TRCCONFIGR.CCI) the ETM reports, every few cycles,
// how many CPU cycles went by since the last report. That is a clock with the
// resolution of the core, and it comes together with the instruction flow, so
// the trace gives, without any instrumentation:
//
//   - the cycles each function spends (self time) and its cycles per instruction,
//   - the time every exception takes to get in and to run, and for a periodic
//     one (SysTick) how late each occurrence was, and what it was waiting for,
//   - for the basic blocks that are reported alone, how the cycles of one and
//     the same code vary: the stalls of caches and flash show up there.
//
// How the cycles are attributed. A cycle count reports the time since the
// previous one, and the instruction ranges that ran in that time came before
// it in the stream. So the cycles of a count belong to the ranges seen since
// the previous count (a "window"); they are shared between them in proportion
// to their instruction counts. A window with one range is exact for that
// range. The cycle counts that a timestamp packet may carry are NOT added: the
// counts alone already agree with the global timestamp to a few ppm.
//
// The Profiler consumes Elements, never OpenCSD types, so it is unit-tested
// with synthetic streams.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_PROFILE_HPP
#define CORTRACE_PROFILE_HPP

#include "cortrace/element.hpp"
#include "cortrace/symbols.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cortrace {

// An exact histogram of integer cycle counts. Values from N-1 up share the
// last bin; min and max stay exact, and a percentile that lands in the last
// bin is reported as the max. The bins are allocated on the first value.
template <std::size_t N> class CycleHist {
public:
    void add(uint64_t v)
    {
        if (bins_.empty())
            bins_.assign(N, 0);
        bins_[std::min<uint64_t>(v, N - 1)]++;
        if (count_ == 0 || v < min_)
            min_ = v;
        if (v > max_)
            max_ = v;
        sum_ += static_cast<double>(v);
        count_++;
    }

    uint64_t count() const { return count_; }
    uint64_t min() const { return min_; }
    uint64_t max() const { return max_; }
    double sum() const { return sum_; }
    double mean() const { return count_ ? sum_ / static_cast<double>(count_) : 0.0; }

    // The smallest value v such that at least p percent of the values are <= v.
    uint64_t percentile(double p) const
    {
        if (count_ == 0)
            return 0;
        const double want = std::min(100.0, std::max(0.0, p)) / 100.0 * static_cast<double>(count_);
        uint64_t rank = static_cast<uint64_t>(want);
        if (static_cast<double>(rank) < want)
            rank++;
        rank = std::max<uint64_t>(1, rank);
        uint64_t seen = 0;
        for (std::size_t i = 0; i < N; i++) {
            seen += bins_[i];
            if (seen >= rank)
                return i == N - 1 ? max_ : i;
        }
        return max_; // not reached: the bins add up to count_
    }

private:
    std::vector<uint32_t> bins_;
    uint64_t count_ = 0;
    uint64_t min_ = 0;
    uint64_t max_ = 0;
    double sum_ = 0;
};

struct ProfileConfig {
    double sysclk_hz = 0; // CPU clock, to compare with the timestamp clock (0: unknown)
    double tsgen_hz = 0; // ETM timestamp clock (0: unknown)
    uint32_t period_exception = 15; // the periodic exception to analyse (15 = SysTick)
    uint64_t period_cycles = 0; // its period in cycles (0: the median interval)
    std::size_t top_blocks = 20; // blocks listed
    uint64_t block_min_count = 1000; // ... that ran at least this often alone
    std::size_t top_blockers = 10; // places the periodic exception waited at, listed
};

// What the stream was, in total.
struct ProfileTotals {
    uint64_t cycles = 0; // the cycle clock: all cycle counts added up
    uint64_t instructions = 0;
    uint64_t ranges = 0;
    uint64_t cc_packets = 0; // cycle counts
    uint64_t timestamps = 0;
    uint64_t ts_first = 0, ts_last = 0; // first and last global timestamp
    uint64_t clock_at_first_ts = 0, clock_at_last_ts = 0; // the cycle clock at those two
    double no_instr_cycles = 0; // cycles of windows without an instruction (exception entry...)
    uint64_t nosync = 0; // trace lost: decoder lost sync (after the first range)
    uint64_t overflow = 0; // trace lost: ETM overflow
    uint64_t trace_on = 0; // trace restarted
    uint64_t discontinuities() const { return nosync + overflow; }
};

struct FunctionStat {
    std::string name;
    uint64_t entries = 0; // ranges that start at the function's first address
    uint64_t ranges = 0;
    uint64_t instructions = 0;
    double cycles = 0; // self cycles
};

// One exception number.
struct ExceptionStat {
    uint32_t number = 0;
    uint64_t count = 0;
    CycleHist<16384> entry; // cycles from the exception to the first cycle count after it
    CycleHist<16384>
        duration; // cycles from the first count after the entry to the one after the return
};

// A periodic exception: how late each occurrence was.
//
// Lateness is measured against the earliest occurrence of an unbroken run of
// the trace, on a grid of `period_cycles`: the part of the delay that never
// changes (stacking, the vector fetch) is not in it.
struct Blocker {
    std::string function; // where the core was when the exception was taken
    uint32_t address = 0; // end of the last range before the exception
    uint64_t count = 0;
    double total = 0; // lateness added up
    uint64_t min = 0, max = 0;
};

struct PeriodicStat {
    uint32_t exception = 0;
    uint64_t occurrences = 0;
    uint64_t period_cycles = 0; // 0: fewer than two occurrences in a run
    bool period_given = false;
    CycleHist<16384> lateness;
    std::vector<Blocker> blockers; // by total lateness, the biggest first
};

// A basic block that was reported alone between two cycle counts, so that its
// cycles are exact: how they vary is the stall of caches and flash.
struct BlockStat {
    std::string function;
    uint32_t start = 0;
    uint32_t end = 0;
    uint64_t count = 0;
    double total = 0;
    uint64_t min = 0, p50 = 0, p90 = 0, p99 = 0, max = 0;
};

class Profiler {
public:
    Profiler(const SymbolTable& syms, const ProfileConfig& cfg);

    // Feed one element, in stream order.
    void process(const Element& e);

    const ProfileTotals& totals() const { return totals_; }
    const ProfileConfig& config() const { return cfg_; }

    // Cycle clock against the global timestamp clock, in ppm; empty when the
    // clocks are not given or fewer than two timestamps were seen.
    std::optional<double> clock_drift_ppm() const;

    // Functions that ran, the one with the most cycles first.
    std::vector<FunctionStat> functions() const;
    // Exceptions seen, by number.
    std::vector<const ExceptionStat*> exceptions() const;
    PeriodicStat periodic() const;
    std::vector<BlockStat> blocks() const;

    // The profile as JSON (format "cortrace-prof", version 1).
    std::string to_json(const std::string& elf_path) const;

private:
    struct Range {
        int fn;
        uint32_t weight;
        uint32_t start, end;
    };
    // An exception that is running.
    struct Isr {
        uint32_t number = 0;
        uint64_t entered = 0; // the cycle clock at the first count after the entry
        bool entered_known = false;
        uint32_t from_addr = 0; // where the core was
        int from_fn = -1;
        long tick = -1; // index in ticks_ when it is the periodic exception
    };
    struct Tick {
        uint64_t at = 0; // the cycle clock at the first count after the entry
        uint32_t from_addr = 0;
        int from_fn = -1;
        std::size_t segment = 0; // unbroken run of the trace it happened in
        bool resolved = false;
    };
    struct Block {
        uint32_t start = 0, end = 0;
        int fn = 0;
        CycleHist<512> hist;
    };

    int fn_of(uint32_t addr);
    void on_range(const Element& e);
    void on_cycle_count(uint32_t cycles);
    void on_timestamp(uint64_t ts);
    void on_exception(uint32_t number);
    void on_exception_return();
    void on_discontinuity(ElementKind why);
    ExceptionStat& exc_stat(uint32_t number);

    const SymbolTable& syms_;
    ProfileConfig cfg_;
    ProfileTotals totals_;

    std::vector<std::string> fn_names_;
    std::vector<FunctionStat> fn_stats_;
    std::unordered_map<std::string, int> fn_by_name_;
    std::unordered_map<uint32_t, int> fn_by_addr_;

    std::vector<Range> window_; // ranges since the last cycle count
    std::unordered_map<uint64_t, Block> blocks_;
    std::vector<Isr> stack_; // running exceptions, innermost last
    std::vector<Isr> returned_; // exceptions returned since the last cycle count
    std::map<uint32_t, ExceptionStat> exc_; // by number
    std::vector<Tick> ticks_; // occurrences of the periodic exception
    std::size_t segment_ = 0;
    uint32_t last_end_ = 0; // end of the last range
    int last_fn_ = -1; // ... and its function
    bool seen_range_ = false;
};

} // namespace cortrace

#endif // CORTRACE_PROFILE_HPP
