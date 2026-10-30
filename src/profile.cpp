// Cortrace — Profiler implementation (see profile.hpp).
//
// SPDX-License-Identifier: MIT
#include "cortrace/profile.hpp"

#include "cortrace/callstack.hpp" // exception_name

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace cortrace {

namespace {

    uint64_t block_key(uint32_t start, uint32_t end)
    {
        return (static_cast<uint64_t>(start) << 32) | end;
    }

    void append(std::string& out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void append(std::string& out, const char* fmt, ...)
    {
        char buf[256];
        va_list ap, again;
        va_start(ap, fmt);
        va_copy(again, ap);
        const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        if (n
            >= static_cast<int>(sizeof buf)) { // a long symbol name: format again, into the string
            const std::size_t at = out.size();
            out.resize(at + static_cast<std::size_t>(n) + 1);
            std::vsnprintf(&out[at], static_cast<std::size_t>(n) + 1, fmt, again);
            out.resize(at + static_cast<std::size_t>(n));
        } else {
            out.append(buf, static_cast<std::size_t>(n));
        }
        va_end(again);
    }

    std::string quote(const std::string& s)
    {
        std::string out = "\"";
        for (const char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                append(out, "\\u%04x", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
        return out + "\"";
    }

    template <std::size_t N> std::string hist_json(const CycleHist<N>& h)
    {
        std::string out;
        append(out,
            "{\"count\": %llu, \"min\": %llu, \"p50\": %llu, \"p90\": %llu, \"p99\": %llu, ",
            static_cast<unsigned long long>(h.count()), static_cast<unsigned long long>(h.min()),
            static_cast<unsigned long long>(h.percentile(50)),
            static_cast<unsigned long long>(h.percentile(90)),
            static_cast<unsigned long long>(h.percentile(99)));
        append(out, "\"max\": %llu, \"mean\": %.2f}", static_cast<unsigned long long>(h.max()),
            h.mean());
        return out;
    }

} // namespace

Profiler::Profiler(const SymbolTable& syms, const ProfileConfig& cfg)
    : syms_(syms)
    , cfg_(cfg)
{
}

int Profiler::fn_of(uint32_t addr)
{
    const auto hit = fn_by_addr_.find(addr);
    if (hit != fn_by_addr_.end())
        return hit->second;
    const std::string& name = syms_.function_at(addr);
    auto it = fn_by_name_.find(name);
    if (it == fn_by_name_.end()) {
        it = fn_by_name_.emplace(name, static_cast<int>(fn_stats_.size())).first;
        fn_stats_.emplace_back();
        fn_stats_.back().name = name;
    }
    fn_by_addr_[addr] = it->second;
    return it->second;
}

ExceptionStat& Profiler::exc_stat(uint32_t number)
{
    ExceptionStat& st = exc_[number];
    st.number = number;
    return st;
}

void Profiler::place_events(uint64_t before)
{
    while (!events_.empty() && events_.front().pos <= before) {
        const uint8_t mask = events_.front().mask;
        events_.pop_front();
        totals_.event_packets++;
        const uint8_t bits[kEventKinds] = { kDwtCpi, kDwtExc, kDwtSleep, kDwtLsu, kDwtFold };
        for (std::size_t k = 0; k < kEventKinds; k++) {
            if (!(mask & bits[k]))
                continue;
            totals_.event_wraps[k]++;
            if (last_fn_ >= 0)
                fn_stats_[last_fn_].events[k]++;
            else
                totals_.events_unplaced++;
        }
    }
}

void Profiler::finish() { place_events(UINT64_MAX); }

void Profiler::process(const Element& e)
{
    place_events(e.byte_index);
    switch (e.kind) {
    case ElementKind::InstrRange:
        on_range(e);
        break;
    case ElementKind::CycleCount:
        if (e.has_cc)
            on_cycle_count(e.cycle_count);
        break;
    case ElementKind::Timestamp:
        on_timestamp(e.timestamp);
        break;
    case ElementKind::Exception:
        on_exception(e.exception_number);
        break;
    case ElementKind::ExceptionRet:
        on_exception_return();
        break;
    case ElementKind::NoSync:
    case ElementKind::Overflow:
    case ElementKind::TraceOn:
        on_discontinuity(e.kind);
        break;
    default:
        break;
    }
}

void Profiler::on_range(const Element& e)
{
    const int fn = fn_of(e.start_addr);
    FunctionStat& f = fn_stats_[fn];
    f.ranges++;
    f.instructions += e.num_instr;
    if (syms_.is_function_entry(e.start_addr))
        f.entries++;
    totals_.ranges++;
    totals_.instructions += e.num_instr;
    seen_range_ = true;
    window_.push_back({ fn, std::max<uint32_t>(1, e.num_instr), e.start_addr, e.end_addr });
    last_end_ = e.end_addr;
    last_fn_ = fn;
    if (e.has_cc) // a decoder that puts the count on the range itself
        on_cycle_count(e.cycle_count);
}

void Profiler::on_cycle_count(uint32_t cycles)
{
    totals_.cc_packets++;
    totals_.cycles += cycles;

    // The cycles belong to the ranges seen since the last count.
    if (window_.empty()) {
        totals_.no_instr_cycles += cycles;
    } else {
        uint64_t weight = 0;
        for (const Range& r : window_)
            weight += r.weight;
        for (const Range& r : window_)
            fn_stats_[r.fn].cycles += static_cast<double>(cycles) * r.weight / weight;
        if (window_.size() == 1) {
            Block& b = blocks_[block_key(window_[0].start, window_[0].end)];
            b.start = window_[0].start;
            b.end = window_[0].end;
            b.fn = window_[0].fn;
            b.hist.add(cycles);
        }
        window_.clear();
    }

    // The first count after an exception is what getting in took.
    for (Isr& isr : stack_) {
        if (isr.entered_known)
            continue;
        exc_stat(isr.number).entry.add(cycles);
        isr.entered = totals_.cycles;
        isr.entered_known = true;
        if (isr.tick >= 0) {
            ticks_[isr.tick].at = totals_.cycles;
            ticks_[isr.tick].resolved = true;
        }
    }
    // The one after a return covers the last instructions of the handler.
    for (Isr& isr : returned_) {
        ExceptionStat& st = exc_stat(isr.number);
        if (!isr.entered_known) {
            st.entry.add(cycles); // returned before a count: only the entry is known
            continue;
        }
        st.duration.add(totals_.cycles - isr.entered);
    }
    returned_.clear();
}

void Profiler::on_timestamp(uint64_t ts)
{
    if (totals_.timestamps == 0) {
        totals_.ts_first = ts;
        totals_.clock_at_first_ts = totals_.cycles;
        totals_.sleep_wraps_at_first_ts = totals_.event_wraps[kSleepKind];
    }
    totals_.timestamps++;
    totals_.ts_last = ts;
    totals_.clock_at_last_ts = totals_.cycles;
    totals_.sleep_wraps_at_last_ts = totals_.event_wraps[kSleepKind];
}

void Profiler::on_exception(uint32_t number)
{
    Isr isr;
    isr.number = number;
    if (number == cfg_.period_exception) {
        Tick t;
        t.from_addr = last_end_;
        t.from_fn = last_fn_;
        t.segment = segment_;
        ticks_.push_back(t);
        isr.tick = static_cast<long>(ticks_.size()) - 1;
    }
    exc_stat(number).count++;
    stack_.push_back(isr);
}

void Profiler::on_exception_return()
{
    if (stack_.empty())
        return; // its entry was before the trace began
    returned_.push_back(stack_.back());
    stack_.pop_back();
}

void Profiler::on_discontinuity(ElementKind why)
{
    if (why == ElementKind::TraceOn)
        totals_.trace_on++;
    else if (seen_range_) { // the decoder starts with a NoSync: that is no loss
        if (why == ElementKind::NoSync)
            totals_.nosync++;
        else
            totals_.overflow++;
    }
    // What was in flight cannot be matched with what follows.
    window_.clear();
    stack_.clear();
    returned_.clear();
    segment_++;
}

std::optional<double> Profiler::clock_drift_ppm() const
{
    if (cfg_.sysclk_hz <= 0 || cfg_.tsgen_hz <= 0 || totals_.timestamps < 2
        || totals_.ts_last <= totals_.ts_first)
        return std::nullopt;
    const double expected = static_cast<double>(totals_.ts_last - totals_.ts_first)
        * (cfg_.sysclk_hz / cfg_.tsgen_hz);
    // The ETM's cycle counts stop while the core sleeps (WFI), the timestamps do
    // not: the cycles asleep between the two timestamps, counted by the DWT,
    // make up the difference.
    const double got = static_cast<double>(totals_.clock_at_last_ts)
        - static_cast<double>(totals_.clock_at_first_ts)
        + static_cast<double>(
            (totals_.sleep_wraps_at_last_ts - totals_.sleep_wraps_at_first_ts) * kEventsPerWrap);
    return (got - expected) / expected * 1e6;
}

std::vector<FunctionStat> Profiler::functions() const
{
    std::vector<FunctionStat> out;
    for (const FunctionStat& f : fn_stats_)
        if (f.ranges > 0)
            out.push_back(f);
    std::sort(out.begin(), out.end(), [](const FunctionStat& a, const FunctionStat& b) {
        return a.cycles != b.cycles ? a.cycles > b.cycles : a.name < b.name;
    });
    return out;
}

std::vector<const ExceptionStat*> Profiler::exceptions() const
{
    std::vector<const ExceptionStat*> out;
    for (const auto& kv : exc_)
        out.push_back(&kv.second);
    return out;
}

PeriodicStat Profiler::periodic() const
{
    PeriodicStat st;
    st.exception = cfg_.period_exception;

    std::vector<const Tick*> ticks;
    for (const Tick& t : ticks_)
        if (t.resolved)
            ticks.push_back(&t);
    st.occurrences = ticks.size();
    if (totals_.event_wraps[kSleepKind] > 0) {
        st.asleep = true;
        return st;
    }

    // The period: given, or the median interval inside the unbroken runs.
    std::vector<uint64_t> intervals;
    for (std::size_t i = 1; i < ticks.size(); i++)
        if (ticks[i]->segment == ticks[i - 1]->segment && ticks[i]->at > ticks[i - 1]->at)
            intervals.push_back(ticks[i]->at - ticks[i - 1]->at);
    if (cfg_.period_cycles > 0) {
        st.period_cycles = cfg_.period_cycles;
        st.period_given = true;
    } else if (!intervals.empty()) {
        std::nth_element(
            intervals.begin(), intervals.begin() + intervals.size() / 2, intervals.end());
        st.period_cycles = intervals[intervals.size() / 2];
    }
    if (st.period_cycles == 0)
        return st;

    // Lateness per run: against the earliest occurrence on the grid.
    std::unordered_map<uint32_t, Blocker> by_addr;
    const double period = static_cast<double>(st.period_cycles);
    for (std::size_t lo = 0; lo < ticks.size();) {
        std::size_t hi = lo;
        while (hi < ticks.size() && ticks[hi]->segment == ticks[lo]->segment)
            hi++;
        if (hi - lo >= 2) {
            std::vector<double> off(hi - lo);
            double base = 0;
            for (std::size_t i = lo; i < hi; i++) {
                const double k = std::round(
                    (static_cast<double>(ticks[i]->at) - static_cast<double>(ticks[lo]->at))
                    / period);
                off[i - lo] = static_cast<double>(ticks[i]->at) - k * period;
                base = (i == lo) ? off[0] : std::min(base, off[i - lo]);
            }
            for (std::size_t i = lo; i < hi; i++) {
                const uint64_t late = static_cast<uint64_t>(std::llround(off[i - lo] - base));
                st.lateness.add(late);
                Blocker& b = by_addr[ticks[i]->from_addr];
                b.address = ticks[i]->from_addr;
                b.function = ticks[i]->from_fn >= 0 ? fn_stats_[ticks[i]->from_fn].name : "?";
                b.min = b.count == 0 ? late : std::min(b.min, late);
                b.max = std::max(b.max, late);
                b.total += static_cast<double>(late);
                b.count++;
            }
        }
        lo = hi;
    }
    for (auto& kv : by_addr)
        st.blockers.push_back(kv.second);
    std::sort(st.blockers.begin(), st.blockers.end(), [](const Blocker& a, const Blocker& b) {
        return a.total != b.total ? a.total > b.total : a.address < b.address;
    });
    if (st.blockers.size() > cfg_.top_blockers)
        st.blockers.resize(cfg_.top_blockers);
    return st;
}

std::vector<BlockStat> Profiler::blocks() const
{
    std::vector<BlockStat> out;
    for (const auto& kv : blocks_) {
        const Block& b = kv.second;
        if (b.hist.count() < cfg_.block_min_count)
            continue;
        BlockStat s;
        s.function = fn_stats_[b.fn].name;
        s.start = b.start;
        s.end = b.end;
        s.count = b.hist.count();
        s.total = b.hist.sum();
        s.min = b.hist.min();
        s.p50 = b.hist.percentile(50);
        s.p90 = b.hist.percentile(90);
        s.p99 = b.hist.percentile(99);
        s.max = b.hist.max();
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const BlockStat& a, const BlockStat& b) {
        return a.total != b.total ? a.total > b.total : a.start < b.start;
    });
    if (out.size() > cfg_.top_blocks)
        out.resize(cfg_.top_blocks);
    return out;
}

std::string Profiler::to_json(const std::string& elf_path) const
{
    std::string j = "{\n \"format\": \"cortrace-prof\",\n \"version\": 1,\n";
    append(j, " \"elf\": %s,\n", quote(elf_path).c_str());

    const ProfileTotals& t = totals_;
    append(j, " \"clock\": {\"sysclk_hz\": %.0f, \"tsgen_hz\": %.0f, \"cycles\": %llu, ",
        cfg_.sysclk_hz, cfg_.tsgen_hz, static_cast<unsigned long long>(t.cycles));
    append(j, "\"instructions\": %llu, \"ranges\": %llu, \"cc_packets\": %llu, ",
        static_cast<unsigned long long>(t.instructions), static_cast<unsigned long long>(t.ranges),
        static_cast<unsigned long long>(t.cc_packets));
    append(j, "\"timestamps\": %llu, \"no_instr_cycles\": %.0f, \"drift_ppm\": ",
        static_cast<unsigned long long>(t.timestamps), t.no_instr_cycles);
    const auto drift = clock_drift_ppm();
    if (drift)
        append(j, "%.2f}", *drift);
    else
        j += "null}";
    append(j, ",\n \"health\": {\"nosync\": %llu, \"overflow\": %llu, \"trace_on\": %llu},\n",
        static_cast<unsigned long long>(t.nosync), static_cast<unsigned long long>(t.overflow),
        static_cast<unsigned long long>(t.trace_on));

    append(j,
        " \"events\": {\"packets\": %llu, \"unit\": %llu, \"unplaced\": %llu, "
        "\"sleep_cycles\": %llu, \"wraps\": {",
        static_cast<unsigned long long>(t.event_packets),
        static_cast<unsigned long long>(kEventsPerWrap),
        static_cast<unsigned long long>(t.events_unplaced),
        static_cast<unsigned long long>(t.event_wraps[kSleepKind] * kEventsPerWrap));
    for (std::size_t k = 0; k < kEventKinds; k++)
        append(j, "%s\"%s\": %llu", k ? ", " : "", event_name(k),
            static_cast<unsigned long long>(t.event_wraps[k]));
    j += "}},\n";

    j += " \"functions\": [";
    bool first = true;
    for (const FunctionStat& f : functions()) {
        append(j,
            "%s\n  {\"name\": %s, \"entries\": %llu, \"ranges\": %llu, \"instructions\": %llu, ",
            first ? "" : ",", quote(f.name).c_str(), static_cast<unsigned long long>(f.entries),
            static_cast<unsigned long long>(f.ranges),
            static_cast<unsigned long long>(f.instructions));
        append(j, "\"cycles\": %.1f, \"events\": {", f.cycles);
        for (std::size_t k = 0; k < kEventKinds; k++)
            append(j, "%s\"%s\": %llu", k ? ", " : "", event_name(k),
                static_cast<unsigned long long>(f.events[k]));
        j += "}}";
        first = false;
    }
    j += "\n ],\n \"exceptions\": [";
    first = true;
    for (const ExceptionStat* e : exceptions()) {
        append(j, "%s\n  {\"number\": %u, \"name\": %s, \"count\": %llu, \"entry\": %s, ",
            first ? "" : ",", e->number, quote(exception_name(e->number)).c_str(),
            static_cast<unsigned long long>(e->count), hist_json(e->entry).c_str());
        append(j, "\"duration\": %s}", hist_json(e->duration).c_str());
        first = false;
    }

    const PeriodicStat p = periodic();
    append(j, "\n ],\n \"periodic\": {\"exception\": %u, \"name\": %s, \"occurrences\": %llu, ",
        p.exception, quote(exception_name(p.exception)).c_str(),
        static_cast<unsigned long long>(p.occurrences));
    append(j,
        "\"period_cycles\": %llu, \"period_given\": %s, \"asleep\": %s, \"lateness\": %s, "
        "\"blockers\": [",
        static_cast<unsigned long long>(p.period_cycles), p.period_given ? "true" : "false",
        p.asleep ? "true" : "false", hist_json(p.lateness).c_str());
    first = true;
    for (const Blocker& b : p.blockers) {
        append(j, "%s\n   {\"function\": %s, \"address\": %u, \"count\": %llu, \"min\": %llu, ",
            first ? "" : ",", quote(b.function).c_str(), b.address,
            static_cast<unsigned long long>(b.count), static_cast<unsigned long long>(b.min));
        append(j, "\"mean\": %.1f, \"max\": %llu}", b.total / static_cast<double>(b.count),
            static_cast<unsigned long long>(b.max));
        first = false;
    }
    j += "\n  ]},\n \"blocks\": [";
    first = true;
    for (const BlockStat& b : blocks()) {
        append(j, "%s\n  {\"function\": %s, \"start\": %u, \"end\": %u, \"count\": %llu, ",
            first ? "" : ",", quote(b.function).c_str(), b.start, b.end,
            static_cast<unsigned long long>(b.count));
        append(j, "\"total\": %.0f, \"min\": %llu, \"p50\": %llu, \"p90\": %llu, \"p99\": %llu, ",
            b.total, static_cast<unsigned long long>(b.min), static_cast<unsigned long long>(b.p50),
            static_cast<unsigned long long>(b.p90), static_cast<unsigned long long>(b.p99));
        append(j, "\"max\": %llu}", static_cast<unsigned long long>(b.max));
        first = false;
    }
    j += "\n ]\n}\n";
    return j;
}

} // namespace cortrace
