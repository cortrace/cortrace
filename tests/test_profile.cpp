// Cortrace — Profiler unit tests (synthetic element streams).
//
// SPDX-License-Identifier: MIT
#include "cortrace/profile.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace cortrace;

namespace {

constexpr uint32_t A = 0x100; // function "A"
constexpr uint32_t B = 0x200; // function "B"
constexpr uint32_t ISR = 0x300;

SymbolTable make_syms()
{
    SymbolTable s;
    s.add(A, "A");
    s.add(B, "B");
    s.add(ISR, "isr");
    s.finalize();
    return s;
}

Element range(uint32_t start, uint32_t end, uint32_t n)
{
    Element e = Element::instr_range(start, end, BranchKind::None, false);
    e.num_instr = n;
    return e;
}

Element cc(uint32_t cycles)
{
    Element e = Element::simple(ElementKind::CycleCount);
    e.has_cc = true;
    e.cycle_count = cycles;
    return e;
}

Element exc(uint32_t number) { return Element::exception(number); }
Element ret() { return Element::simple(ElementKind::ExceptionRet); }

// One occurrence of exception 15: the core runs `gap` cycles up to the
// exception (stopped at `end`), takes 10 to get in, the handler is one count.
// The interval between two such ticks is gap + 11.
void tick(Profiler& p, uint32_t gap, uint32_t end = A + 0x10)
{
    p.process(range(A, end, 4));
    p.process(cc(gap));
    p.process(exc(15));
    p.process(cc(10));
    p.process(ret());
    p.process(cc(1));
}

ProfileConfig cfg_blocks(uint64_t min_count, std::size_t top)
{
    ProfileConfig c;
    c.block_min_count = min_count;
    c.top_blocks = top;
    return c;
}

// The string outside of quotes has balanced {} and [] and every quote closes.
bool json_balanced(const std::string& j)
{
    int brace = 0, bracket = 0;
    bool in_str = false;
    for (std::size_t i = 0; i < j.size(); i++) {
        const char c = j[i];
        if (in_str) {
            if (c == '\\')
                i++;
            else if (c == '"')
                in_str = false;
            continue;
        }
        if (c == '"')
            in_str = true;
        else if (c == '{')
            brace++;
        else if (c == '}')
            brace--;
        else if (c == '[')
            bracket++;
        else if (c == ']')
            bracket--;
        if (brace < 0 || bracket < 0)
            return false;
    }
    return !in_str && brace == 0 && bracket == 0;
}

bool has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

} // namespace

// ---- CycleHist ----------------------------------------------------------------
TEST(cyclehist_percentiles_mean_and_the_overflow_bin)
{
    CycleHist<8> h;
    CHECK_EQ(h.count(), 0u);
    CHECK_EQ(h.percentile(50), 0u);
    CHECK_EQ(h.mean(), 0.0);
    h.add(3);
    h.add(1);
    h.add(100); // beyond the last bin
    CHECK_EQ(h.count(), 3u);
    CHECK_EQ(h.min(), 1u);
    CHECK_EQ(h.max(), 100u);
    CHECK_EQ(h.sum(), 104.0);
    CHECK_EQ(h.percentile(1), 1u);
    CHECK_EQ(h.percentile(50), 3u);
    CHECK_EQ(h.percentile(100), 100u); // the last bin reports the exact max
    CHECK_EQ(h.percentile(-5), 1u); // clamped to 0%
    CHECK_EQ(h.percentile(500), 100u); // clamped to 100%
    CHECK(h.mean() > 34.6 && h.mean() < 34.7);
}

// ---- cycles per function --------------------------------------------------------
TEST(profile_shares_a_window_by_instruction_count)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(range(A, A + 8, 3));
    p.process(range(B, B + 8, 1));
    p.process(cc(40)); // 30 for A, 10 for B
    p.process(cc(5)); // no instruction in the window
    CHECK_EQ(p.totals().cycles, 45u);
    CHECK_EQ(p.totals().cc_packets, 2u);
    CHECK_EQ(p.totals().instructions, 4u);
    CHECK_EQ(p.totals().no_instr_cycles, 5.0);

    const auto fns = p.functions();
    CHECK_EQ(fns.size(), 2u);
    CHECK_EQ(fns[0].name, std::string("A"));
    CHECK_EQ(fns[0].cycles, 30.0);
    CHECK_EQ(fns[0].entries, 1u);
    CHECK_EQ(fns[1].name, std::string("B"));
    CHECK_EQ(fns[1].cycles, 10.0);
    // the cycles are all accounted for
    CHECK_EQ(fns[0].cycles + fns[1].cycles + p.totals().no_instr_cycles, 45.0);
}

TEST(profile_a_range_without_a_count_weighs_one_and_entries_need_the_function_start)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(range(A + 4, A + 8, 0)); // not the first address; num_instr unknown
    p.process(range(A + 8, A + 12, 0));
    p.process(cc(10));
    const auto fns = p.functions();
    CHECK_EQ(fns.size(), 1u);
    CHECK_EQ(fns[0].entries, 0u);
    CHECK_EQ(fns[0].ranges, 2u);
    CHECK_EQ(fns[0].cycles, 10.0);
}

TEST(profile_a_count_on_the_range_itself_covers_that_range)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(Element::instr_range(A, A + 4, BranchKind::None, false, 0, true, 7));
    CHECK_EQ(p.totals().cycles, 7u);
    CHECK_EQ(p.functions()[0].cycles, 7.0);
}

TEST(profile_ignores_other_elements_and_counts_without_cycles)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(Element::simple(ElementKind::Benign));
    p.process(Element::simple(ElementKind::AddrNacc));
    p.process(Element::simple(ElementKind::CycleCount)); // has_cc false
    CHECK_EQ(p.totals().cc_packets, 0u);
    CHECK_EQ(p.totals().cycles, 0u);
    CHECK(p.functions().empty());
}

TEST(profile_unknown_code_is_question_mark)
{
    const SymbolTable none; // no symbols at all
    Profiler p(none, {});
    p.process(range(0x500, 0x504, 1));
    p.process(cc(3));
    CHECK_EQ(p.functions()[0].name, std::string("?"));
}

// ---- basic blocks ----------------------------------------------------------------
TEST(profile_blocks_report_the_spread_of_ranges_that_ran_alone)
{
    const SymbolTable s = make_syms();
    Profiler p(s, cfg_blocks(3, 2));
    for (const uint32_t c : { 10u, 12u, 14u }) { // block X: 3 times alone
        p.process(range(A, A + 8, 2));
        p.process(cc(c));
    }
    for (int i = 0; i < 3; i++) { // block Y: 3 times alone, cheaper
        p.process(range(B, B + 8, 2));
        p.process(cc(5));
    }
    for (int i = 0; i < 2; i++) { // block Z: only twice, below the limit
        p.process(range(B + 8, B + 12, 1));
        p.process(cc(9));
    }
    p.process(range(A, A + 8, 2)); // together with another range: not alone
    p.process(range(B, B + 8, 2));
    p.process(cc(99));

    const auto blocks = p.blocks();
    CHECK_EQ(blocks.size(), 2u);
    CHECK_EQ(blocks[0].function, std::string("A"));
    CHECK_EQ(blocks[0].start, A);
    CHECK_EQ(blocks[0].end, A + 8);
    CHECK_EQ(blocks[0].count, 3u);
    CHECK_EQ(blocks[0].total, 36.0);
    CHECK_EQ(blocks[0].min, 10u);
    CHECK_EQ(blocks[0].p50, 12u);
    CHECK_EQ(blocks[0].p90, 14u);
    CHECK_EQ(blocks[0].p99, 14u);
    CHECK_EQ(blocks[0].max, 14u);
    CHECK_EQ(blocks[1].function, std::string("B"));
    CHECK_EQ(blocks[1].total, 15.0);

    Profiler top1(s, cfg_blocks(3, 1));
    for (int i = 0; i < 3; i++) {
        top1.process(range(A, A + 8, 2));
        top1.process(cc(10));
        top1.process(range(B, B + 8, 2));
        top1.process(cc(5));
    }
    CHECK_EQ(top1.blocks().size(), 1u);
}

// ---- exceptions --------------------------------------------------------------------
TEST(profile_exception_entry_and_duration)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(range(A, A + 8, 2));
    p.process(cc(20));
    p.process(exc(11));
    p.process(cc(47)); // getting in
    p.process(range(ISR, ISR + 8, 5));
    p.process(cc(100)); // the handler
    p.process(ret());
    p.process(cc(30)); // covers the last instructions of the handler
    const auto ex = p.exceptions();
    CHECK_EQ(ex.size(), 1u);
    CHECK_EQ(ex[0]->number, 11u);
    CHECK_EQ(ex[0]->count, 1u);
    CHECK_EQ(ex[0]->entry.count(), 1u);
    CHECK_EQ(ex[0]->entry.min(), 47u);
    CHECK_EQ(ex[0]->duration.count(), 1u);
    CHECK_EQ(ex[0]->duration.max(), 130u); // 100 + 30
}

TEST(profile_nested_exceptions_and_returns_without_entry)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(ret()); // its entry is before the trace: ignored
    p.process(cc(5));
    CHECK(p.exceptions().empty());

    p.process(exc(11));
    p.process(exc(14)); // tail-chained: both wait for the same count
    p.process(cc(40));
    p.process(ret()); // the inner one
    p.process(cc(10));
    p.process(ret()); // the outer one
    p.process(cc(20));
    const auto ex = p.exceptions();
    CHECK_EQ(ex.size(), 2u);
    CHECK_EQ(ex[0]->number, 11u);
    CHECK_EQ(ex[0]->entry.min(), 40u);
    CHECK_EQ(ex[0]->duration.max(), 30u);
    CHECK_EQ(ex[1]->number, 14u);
    CHECK_EQ(ex[1]->duration.max(), 10u);

    // returned before any count after the entry: only the entry is known
    p.process(exc(5));
    p.process(ret());
    p.process(cc(60));
    const auto after = p.exceptions();
    CHECK_EQ(after[0]->number, 5u);
    CHECK_EQ(after[0]->entry.min(), 60u);
    CHECK_EQ(after[0]->duration.count(), 0u);
}

TEST(profile_a_loss_forgets_the_running_exceptions)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(range(A, A + 8, 2)); // the trace has begun
    p.process(exc(11));
    p.process(Element::simple(ElementKind::NoSync));
    p.process(cc(40)); // would have been the entry
    p.process(ret()); // nothing to return from
    p.process(cc(10));
    const auto ex = p.exceptions();
    CHECK_EQ(ex.size(), 1u);
    CHECK_EQ(ex[0]->entry.count(), 0u);
    CHECK_EQ(ex[0]->duration.count(), 0u);
    CHECK_EQ(p.totals().nosync, 1u);
}

// ---- trace losses ---------------------------------------------------------------------
TEST(profile_counts_losses_after_the_first_range_only)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(Element::simple(ElementKind::NoSync)); // the decoder's start-up
    p.process(Element::simple(ElementKind::Overflow));
    CHECK_EQ(p.totals().discontinuities(), 0u);
    p.process(range(A, A + 4, 1));
    p.process(Element::simple(ElementKind::NoSync));
    p.process(Element::simple(ElementKind::Overflow));
    p.process(Element::simple(ElementKind::TraceOn));
    CHECK_EQ(p.totals().nosync, 1u);
    CHECK_EQ(p.totals().overflow, 1u);
    CHECK_EQ(p.totals().trace_on, 1u);
    CHECK_EQ(p.totals().discontinuities(), 2u);
    // the range that was waiting for its count was dropped with the loss
    p.process(cc(8));
    CHECK_EQ(p.totals().no_instr_cycles, 8.0);
    CHECK_EQ(p.functions()[0].cycles, 0.0);
}

// ---- the clock ------------------------------------------------------------------------
TEST(profile_clock_drift_against_the_timestamps)
{
    const SymbolTable s = make_syms();
    ProfileConfig c;
    c.sysclk_hz = 150e6;
    c.tsgen_hz = 75e6;
    Profiler p(s, c);
    CHECK(!p.clock_drift_ppm());
    p.process(Element::make_timestamp(1000));
    CHECK(!p.clock_drift_ppm()); // one timestamp
    p.process(cc(2002));
    p.process(Element::make_timestamp(2000));
    CHECK(p.clock_drift_ppm());
    CHECK(*p.clock_drift_ppm() > 999.9 && *p.clock_drift_ppm() < 1000.1);
    CHECK_EQ(p.totals().timestamps, 2u);
    CHECK_EQ(p.config().sysclk_hz, 150e6);

    Profiler none(s, {}); // no clock rates given
    none.process(Element::make_timestamp(1));
    none.process(Element::make_timestamp(2));
    CHECK(!none.clock_drift_ppm());

    Profiler flat(s, c); // the timestamp did not advance
    flat.process(Element::make_timestamp(5));
    flat.process(Element::make_timestamp(5));
    CHECK(!flat.clock_drift_ppm());
}

// ---- periodic exception ------------------------------------------------------------------
TEST(periodic_lateness_from_the_median_interval)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    tick(p, 989); // tick 0
    tick(p, 989); // on the grid
    tick(p, 1039, A + 0x20); // 50 late, held up at A+0x20
    tick(p, 939); // back on the grid
    tick(p, 989);
    const PeriodicStat st = p.periodic();
    CHECK_EQ(st.exception, 15u);
    CHECK_EQ(st.occurrences, 5u);
    CHECK_EQ(st.period_cycles, 1000u);
    CHECK(!st.period_given);
    CHECK_EQ(st.lateness.count(), 5u);
    CHECK_EQ(st.lateness.max(), 50u);
    CHECK_EQ(st.lateness.percentile(50), 0u);
    CHECK_EQ(st.blockers.size(), 2u);
    CHECK_EQ(st.blockers[0].address, A + 0x20); // the biggest total lateness first
    CHECK_EQ(st.blockers[0].function, std::string("A"));
    CHECK_EQ(st.blockers[0].count, 1u);
    CHECK_EQ(st.blockers[0].max, 50u);
    CHECK_EQ(st.blockers[1].address, A + 0x10);
    CHECK_EQ(st.blockers[1].count, 4u);
    CHECK_EQ(st.blockers[1].min, 0u);

    ProfileConfig one;
    one.top_blockers = 1;
    Profiler q(s, one);
    tick(q, 989);
    tick(q, 1039, A + 0x20);
    tick(q, 939);
    CHECK_EQ(q.periodic().blockers.size(), 1u);
}

TEST(periodic_given_period_and_a_missing_occurrence)
{
    const SymbolTable s = make_syms();
    ProfileConfig c;
    c.period_cycles = 1000;
    Profiler p(s, c);
    tick(p, 989);
    tick(p, 989);
    tick(p, 1989); // one occurrence never came: 2000 after the last
    const PeriodicStat st = p.periodic();
    CHECK(st.period_given);
    CHECK_EQ(st.period_cycles, 1000u);
    CHECK_EQ(st.lateness.count(), 3u);
    CHECK_EQ(st.lateness.max(), 0u);
}

TEST(periodic_runs_are_separate_and_short_ones_do_not_count)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(range(A, A + 4, 1)); // the trace has begun
    tick(p, 989);
    tick(p, 989);
    tick(p, 989);
    p.process(Element::simple(ElementKind::Overflow)); // a loss: a new run
    tick(p, 5); // a lone occurrence in its own run
    p.process(Element::simple(ElementKind::TraceOn));
    tick(p, 7000);
    tick(p, 7000);
    p.process(exc(15)); // never gets its count
    const PeriodicStat st = p.periodic();
    CHECK_EQ(st.occurrences, 6u);
    // intervals: 1000, 1000 | 7011 (the run of one has none) -> median 1000
    CHECK_EQ(st.period_cycles, 1000u);
    CHECK_EQ(st.lateness.count(), 5u); // the lone one is left out
}

TEST(periodic_nothing_to_measure)
{
    const SymbolTable s = make_syms();
    Profiler empty(s, {});
    CHECK_EQ(empty.periodic().occurrences, 0u);
    CHECK_EQ(empty.periodic().period_cycles, 0u);

    Profiler one(s, {});
    tick(one, 100);
    CHECK_EQ(one.periodic().occurrences, 1u);
    CHECK_EQ(one.periodic().period_cycles, 0u);
    CHECK_EQ(one.periodic().lateness.count(), 0u);

    ProfileConfig c;
    c.period_exception = 11; // another exception
    Profiler other(s, c);
    tick(other, 100); // exception 15 does not count
    CHECK_EQ(other.periodic().occurrences, 0u);
}

TEST(periodic_an_occurrence_at_the_start_has_no_known_place)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    p.process(exc(15)); // before any range
    p.process(cc(10));
    p.process(ret());
    p.process(cc(1));
    p.process(exc(15));
    p.process(cc(10));
    p.process(ret());
    p.process(cc(990));
    const PeriodicStat st = p.periodic();
    CHECK_EQ(st.occurrences, 2u);
    CHECK_EQ(st.blockers.size(), 1u);
    CHECK_EQ(st.blockers[0].function, std::string("?"));
}

// ---- JSON ----------------------------------------------------------------------------------
TEST(profile_json_of_an_empty_profile)
{
    const SymbolTable s = make_syms();
    Profiler p(s, {});
    const std::string j = p.to_json("fw.elf");
    CHECK(json_balanced(j));
    CHECK(has(j, "\"format\": \"cortrace-prof\""));
    CHECK(has(j, "\"elf\": \"fw.elf\""));
    CHECK(has(j, "\"drift_ppm\": null"));
    CHECK(has(j, "\"functions\": [\n ]"));
}

TEST(profile_json_content_and_string_escapes)
{
    SymbolTable s;
    const std::string nasty = std::string("we\"ird\\name\x01") + std::string(300, 'x'); // long
    s.add(A, nasty);
    s.add(ISR, "isr");
    s.finalize();
    ProfileConfig c;
    c.sysclk_hz = 150e6;
    c.tsgen_hz = 75e6;
    c.block_min_count = 1;
    Profiler p(s, c);
    p.process(Element::make_timestamp(10));
    for (int i = 0; i < 3; i++)
        tick(p, 989);
    p.process(Element::make_timestamp(2000));
    const std::string j = p.to_json("a \"b\".elf");
    CHECK(json_balanced(j));
    CHECK(has(j, "\"elf\": \"a \\\"b\\\".elf\""));
    CHECK(has(j, "we\\\"ird\\\\name\\u0001" + std::string(300, 'x')));
    CHECK(has(j, "\"name\": \"IRQ:SysTick\""));
    CHECK(has(j, "\"period_cycles\": 1000"));
    CHECK(has(j, "\"blockers\": [\n"));
    CHECK(has(j, "\"blocks\": [\n"));
    CHECK(!has(j, "\"drift_ppm\": null"));
}
