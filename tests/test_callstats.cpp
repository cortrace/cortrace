// Cortrace — call statistics unit tests.
//
// Synthetic slice streams (no decoder): nesting, ISR preemption, thread
// switches, lost trace, truncated frames, percentiles and the output formats.
//
// SPDX-License-Identifier: MIT
#include "cortrace/callstack.hpp"
#include "cortrace/callstats.hpp"
#include "cortrace/element.hpp"
#include "cortrace/symbols.hpp"
#include "test_framework.hpp"

using namespace cortrace;

namespace {

SliceEvent ev(bool begin, const char* name, uint64_t cc, int track = 0, int thread = kThreadBoot,
    uint64_t losses = 0, bool truncated = false)
{
    SliceEvent s;
    s.begin = begin;
    s.name = name;
    s.cycle_clock = cc;
    s.cycle_est = cc;
    s.track = track;
    s.thread = thread;
    s.losses = losses;
    s.truncated = truncated;
    return s;
}

const CallFunctionStat* fn(const CallStats& r, const std::string& name)
{
    for (const auto& f : r.functions)
        if (f.name == name)
            return &f;
    return nullptr;
}

const CallThreadStat* thr(const CallStats& r, int id)
{
    for (const auto& t : r.threads)
        if (t.id == id)
            return &t;
    return nullptr;
}

const CallEdgeStat* edge(const CallStats& r, const std::string& from, const std::string& to)
{
    for (const auto& e : r.edges)
        if (e.caller == from && e.callee == to)
            return &e;
    return nullptr;
}

} // namespace

TEST(callstats_nested_calls_split_inclusive_and_self)
{
    std::vector<SliceEvent> s
        = { ev(true, "A", 0), ev(true, "B", 10), ev(false, "B", 30), ev(false, "A", 50) };
    CallStats r = compute_callstats(s, {}, {});
    const CallFunctionStat* a = fn(r, "A");
    const CallFunctionStat* b = fn(r, "B");
    CHECK(a && b);
    CHECK_EQ(a->calls, 1u);
    CHECK_EQ(a->incl, 50u);
    CHECK_EQ(a->self, 30u);
    CHECK_EQ(b->incl, 20u);
    CHECK_EQ(b->self, 20u);
    CHECK_EQ(r.credited, 50u);
    CHECK_EQ(r.dropped, 0u);
    const CallEdgeStat* ab = edge(r, "A", "B");
    CHECK(ab);
    CHECK_EQ(ab->calls, 1u);
    CHECK_EQ(ab->incl, 20u);
    CHECK(edge(r, "<root>", "A") != nullptr);
    // with no thread information the one context is called "main"
    CHECK_EQ(r.folded.at("main;A"), 30u);
    CHECK_EQ(r.folded.at("main;A;B"), 20u);
}

TEST(callstats_an_isr_is_taken_out_of_the_function_it_preempted)
{
    // A runs 0..60; SysTick runs 10..25 on its own track.
    std::vector<SliceEvent> s = { ev(true, "A", 0), ev(true, "IRQ:SysTick", 10, 1, kThreadUntagged),
        ev(false, "IRQ:SysTick", 25, 1, kThreadUntagged), ev(false, "A", 60) };
    CallStats r = compute_callstats(s, {}, {});
    CHECK_EQ(fn(r, "A")->incl, 45u);
    CHECK_EQ(fn(r, "IRQ:SysTick")->incl, 15u);
    const CallThreadStat* t = thr(r, kThreadBoot);
    CHECK(t);
    CHECK_EQ(t->run, 45u);
    CHECK_EQ(t->isr, 15u);
    CHECK_EQ(t->isr_entries, 1u);
    CHECK_EQ(r.folded.at("IRQ:SysTick"), 15u);
}

TEST(callstats_nested_isrs_do_not_double_count)
{
    // IRQ1 runs 10..50 and is preempted by IRQ2 from 20 to 30.
    std::vector<SliceEvent> s = { ev(true, "A", 0), ev(true, "IRQ:ext0", 10, 1, kThreadUntagged),
        ev(true, "IRQ:ext1", 20, 2, kThreadUntagged), ev(false, "IRQ:ext1", 30, 2, kThreadUntagged),
        ev(false, "IRQ:ext0", 50, 1, kThreadUntagged), ev(false, "A", 60) };
    CallStats r = compute_callstats(s, {}, {});
    CHECK_EQ(fn(r, "IRQ:ext1")->incl, 10u);
    CHECK_EQ(fn(r, "IRQ:ext0")->incl, 30u);
    CHECK_EQ(fn(r, "A")->incl, 20u);
    CHECK_EQ(thr(r, kThreadBoot)->run + thr(r, kThreadBoot)->isr, 60u);
}

TEST(callstats_a_call_does_not_include_the_time_of_other_threads)
{
    // F (thread 5) is switched out at 10 and back in at 40. G runs on thread 6.
    std::vector<SliceEvent> s = { ev(true, "F", 0, 0, 5), ev(true, "G", 10, 0, 6),
        ev(false, "G", 40, 0, 6), ev(false, "F", 50, 0, 5) };
    std::vector<ThreadSwitch> sw(3);
    sw[0].slice_index = 0;
    sw[0].cycle_est = 0;
    sw[0].from = kThreadBoot;
    sw[0].to = 5;
    sw[1].slice_index = 1;
    sw[1].cycle_est = 10;
    sw[1].from = 5;
    sw[1].to = 6;
    sw[2].slice_index = 3;
    sw[2].cycle_est = 40;
    sw[2].from = 6;
    sw[2].to = 5;
    CallStats r = compute_callstats(s, sw, { { 5, "tA" }, { 6, "tB" } });
    CHECK_EQ(fn(r, "F")->incl, 20u);
    CHECK_EQ(fn(r, "G")->incl, 30u);
    const CallThreadStat* a = thr(r, 5);
    const CallThreadStat* b = thr(r, 6);
    CHECK(a && b);
    CHECK_EQ(a->name, std::string("tA"));
    CHECK_EQ(a->run, 20u);
    CHECK_EQ(b->run, 30u);
    CHECK_EQ(a->longest, 10u); // 0..10; the second interval is cut by the end of the trace
    CHECK_EQ(a->intervals, 2u);
    CHECK_EQ(a->lossy_intervals, 1u);
    CHECK_EQ(b->longest, 30u);
    CHECK_EQ(r.folded.at("tA;F"), 20u);
    CHECK_EQ(r.folded.at("tB;G"), 30u);
}

TEST(callstats_calls_that_span_lost_trace_add_no_time)
{
    std::vector<SliceEvent> s = { ev(true, "A", 0), ev(true, "B", 10), ev(false, "B", 20),
        ev(false, "A", 100, 0, kThreadBoot, 1), ev(true, "C", 100, 0, kThreadBoot, 1),
        ev(false, "C", 110, 0, kThreadBoot, 1) };
    CallStats r = compute_callstats(s, {}, {});
    CHECK_EQ(fn(r, "A")->calls, 0u);
    CHECK_EQ(fn(r, "A")->crossed, 1u);
    CHECK_EQ(fn(r, "B")->calls, 1u);
    CHECK_EQ(fn(r, "B")->incl, 10u);
    CHECK_EQ(fn(r, "C")->incl, 10u);
    CHECK_EQ(r.dropped, 80u); // 20 -> 100 cannot be placed
    CHECK_EQ(r.credited, 30u);
    CHECK_EQ(r.crossed_frames, 1u);
    CHECK_EQ(r.losses, 1u);
}

TEST(callstats_frames_closed_because_the_trace_ended_are_counted_apart)
{
    std::vector<SliceEvent> s = { ev(true, "A", 0), ev(false, "A", 10, 0, kThreadBoot, 0, true) };
    CallStats r = compute_callstats(s, {}, {});
    CHECK_EQ(fn(r, "A")->calls, 0u);
    CHECK_EQ(fn(r, "A")->truncated, 1u);
    CHECK_EQ(r.truncated_frames, 1u);
    CHECK_EQ(fn(r, "A")->incl, 0u);
}

TEST(callstats_recursion_counts_the_inclusive_time_once)
{
    std::vector<SliceEvent> s
        = { ev(true, "F", 0), ev(true, "F", 10), ev(false, "F", 20), ev(false, "F", 30) };
    CallStats r = compute_callstats(s, {}, {});
    const CallFunctionStat* f = fn(r, "F");
    CHECK_EQ(f->calls, 2u);
    CHECK_EQ(f->incl, 30u); // not 40
    CHECK_EQ(f->self, 30u);
    CHECK_EQ(f->incl_min, 10u);
    CHECK_EQ(f->incl_max, 30u);
    const CallEdgeStat* e = edge(r, "F", "F");
    CHECK(e);
    CHECK_EQ(e->calls, 1u);
    CHECK_EQ(e->incl, 10u); // the edges of F add up to its total
    CHECK_EQ(edge(r, "<root>", "F")->incl, 20u);
}

TEST(callstats_recursion_with_a_lost_outer_call_keeps_the_intact_inner_calls)
{
    // The outer F spans lost trace, the inner one does not.
    std::vector<SliceEvent> s = { ev(true, "F", 0), ev(true, "F", 10, 0, kThreadBoot, 0),
        ev(false, "F", 20, 0, kThreadBoot, 0), ev(false, "F", 90, 0, kThreadBoot, 1) };
    CallStats r = compute_callstats(s, {}, {});
    const CallFunctionStat* f = fn(r, "F");
    CHECK_EQ(f->calls, 1u);
    CHECK_EQ(f->crossed, 1u);
    CHECK_EQ(f->incl, 10u);
    CHECK(f->self <= f->incl);
}

TEST(callstats_slices_without_thread_information_belong_to_the_current_thread)
{
    SliceEvent b = ev(true, "A", 0);
    SliceEvent e = ev(false, "A", 12);
    b.thread = e.thread = kThreadUntagged;
    CallStats r = compute_callstats({ b, e }, {}, {});
    CHECK_EQ(fn(r, "A")->incl, 12u);
}

TEST(callstats_an_end_is_matched_by_position_not_by_name)
{
    // A tail call relabels the top frame: the end carries the new name.
    std::vector<SliceEvent> s = { ev(true, "A", 0), ev(false, "B", 10) };
    CallStats r = compute_callstats(s, {}, {});
    CHECK_EQ(fn(r, "A")->calls, 1u);
    CHECK_EQ(fn(r, "A")->incl, 10u);
    CHECK(fn(r, "B") == nullptr);
}

TEST(callstats_an_end_without_a_begin_is_ignored)
{
    std::vector<SliceEvent> s = { ev(false, "A", 5), ev(true, "B", 6), ev(false, "B", 9) };
    CallStats r = compute_callstats(s, {}, {});
    CHECK_EQ(fn(r, "B")->incl, 3u);
    CHECK(fn(r, "A") == nullptr);
}

TEST(callstats_percentiles_and_extremes)
{
    std::vector<SliceEvent> s;
    uint64_t cc = 0;
    for (int d = 1; d <= 10; ++d) {
        s.push_back(ev(true, "F", cc));
        cc += static_cast<uint64_t>(d);
        s.push_back(ev(false, "F", cc));
    }
    CallStats r = compute_callstats(s, {}, {});
    const CallFunctionStat* f = fn(r, "F");
    CHECK_EQ(f->calls, 10u);
    CHECK_EQ(f->incl, 55u);
    CHECK_EQ(f->incl_min, 1u);
    CHECK_EQ(f->incl_max, 10u);
    CHECK_EQ(f->p50, 5u);
    CHECK_EQ(f->p90, 9u);
    CHECK_EQ(f->p99, 9u);
}

TEST(callstats_output_formats)
{
    std::vector<SliceEvent> s = { ev(true, "a;b", 0), ev(false, "a;b", 10) };
    CallStats r = compute_callstats(s, {}, { { kThreadBoot, "my \"t\"" } });
    const std::string folded = r.folded_text();
    // ';' inside a name would split the frame: it becomes ','
    CHECK(folded.find("a,b 10\n") != std::string::npos);
    CHECK(folded.find(";a,b") != std::string::npos);
    const std::string json = r.to_json("fw.elf", 150e6);
    CHECK(json.find("\"format\": \"cortrace-callstats\"") != std::string::npos);
    CHECK(json.find("\"sysclk_hz\": 150000000") != std::string::npos);
    CHECK(json.find("\"name\": \"a;b\"") != std::string::npos);
    CHECK(json.find("my \\\"t\\\"") != std::string::npos);
    CHECK(r.to_json("fw.elf", 0).find("\"sysclk_hz\": null") != std::string::npos);
}

// ---- the call-stack machine's side: losses, truncated ends, switches ------------------------

static SymbolTable make_syms()
{
    SymbolTable t;
    t.add(0x1000, "main");
    t.add(0x2000, "leaf");
    t.finalize();
    return t;
}

TEST(callstack_slices_carry_the_losses_seen_so_far)
{
    SymbolTable syms = make_syms();
    CallStackMachine m(syms);
    m.process(Element::instr_range(0x1000, 0x1008, BranchKind::None, false));
    m.process(Element::instr_range(0x1004, 0x1008, BranchKind::DirectCall, true));
    m.process(Element::instr_range(0x2000, 0x2010, BranchKind::None, false));
    m.process(Element::simple(ElementKind::NoSync, 100)); // NoSync alone is a loss too
    m.process(Element::instr_range(0x2010, 0x2014, BranchKind::Return, true));
    m.finish();
    const auto& s = m.slices();
    CHECK(s.size() >= 2);
    CHECK_EQ(s.front().losses, 0u); // begin of leaf
    bool saw_end_of_leaf = false;
    for (const auto& e : s)
        if (!e.begin && e.name == "leaf") {
            CHECK_EQ(e.losses, 1u);
            saw_end_of_leaf = true;
        }
    CHECK(saw_end_of_leaf);
}

TEST(callstack_finish_marks_the_ends_it_made_up)
{
    SymbolTable syms = make_syms();
    CallStackMachine m(syms);
    m.process(Element::instr_range(0x1000, 0x1008, BranchKind::None, false));
    m.process(Element::instr_range(0x1004, 0x1008, BranchKind::DirectCall, true));
    m.process(Element::instr_range(0x2000, 0x2010, BranchKind::None, false));
    m.process(Element::instr_range(0x2010, 0x2014, BranchKind::Return, true));
    const size_t before = m.slices().size();
    m.finish();
    for (size_t i = 0; i < before; ++i)
        CHECK(!m.slices()[i].truncated); // the return the CPU executed is not truncated
}

TEST(callstack_finish_marks_open_frames_truncated)
{
    SymbolTable syms = make_syms();
    CallStackMachine m(syms);
    m.process(Element::instr_range(0x1000, 0x1008, BranchKind::None, false));
    m.process(Element::instr_range(0x1004, 0x1008, BranchKind::DirectCall, true));
    m.process(Element::instr_range(0x2000, 0x2010, BranchKind::None, false));
    m.finish();
    CHECK(m.slices().back().truncated);
    CHECK_EQ(m.slices().back().name, std::string("leaf"));
}

TEST(callstack_thread_switches_are_recorded_with_their_position)
{
    SymbolTable syms = make_syms();
    CallStackMachine m(syms);
    m.switch_thread(1);
    m.process(Element::instr_range(0x1000, 0x1008, BranchKind::None, false));
    m.process(Element::instr_range(0x1004, 0x1008, BranchKind::DirectCall, true));
    m.process(Element::instr_range(0x2000, 0x2010, BranchKind::None, false));
    const size_t n = m.slices().size();
    m.switch_thread(2);
    m.switch_thread(2); // not a switch
    const auto& sw = m.thread_switches();
    CHECK_EQ(sw.size(), 2u);
    CHECK_EQ(sw[0].from, kThreadBoot);
    CHECK_EQ(sw[0].to, 1);
    CHECK_EQ(sw[1].from, 1);
    CHECK_EQ(sw[1].to, 2);
    CHECK_EQ(sw[1].slice_index, n);
}

TEST(callstack_leaving_the_boot_context_marks_its_frames_truncated)
{
    SymbolTable syms = make_syms();
    CallStackMachine m(syms);
    m.process(Element::instr_range(0x1000, 0x1008, BranchKind::None, false));
    m.process(Element::instr_range(0x1004, 0x1008, BranchKind::DirectCall, true));
    m.process(Element::instr_range(0x2000, 0x2010, BranchKind::None, false));
    m.switch_thread(1);
    CHECK(m.slices().back().truncated);
    CHECK(!m.slices().back().begin);
}

TEST(callstack_cycles_of_a_count_are_spread_over_the_instructions_it_covers)
{
    SymbolTable syms = make_syms();
    CallStackMachine m(syms);
    // main calls leaf; leaf runs 8 instructions and returns in a range that
    // carries the cycle count of the whole window (2 + 8 + 10 instructions).
    Element r1 = Element::instr_range(0x1000, 0x1008, BranchKind::DirectCall, true);
    r1.num_instr = 2;
    Element r2 = Element::instr_range(0x2000, 0x2010, BranchKind::None, false);
    r2.num_instr = 8;
    Element r3 = Element::instr_range(0x2010, 0x2014, BranchKind::Return, true, 0, true, 100);
    r3.num_instr = 10;
    m.process(r1);
    m.process(r2);
    m.process(r3);
    m.finish();
    const auto& s = m.slices();
    CHECK_EQ(s.size(), 2u);
    CHECK(s[0].begin);
    CHECK_EQ(s[0].cycle_clock, 0u); // the clock alone has not moved yet
    CHECK_EQ(s[0].cycle_est, 10u); // 100 cycles * 2 of 20 instructions
    CHECK(!s[1].begin);
    CHECK_EQ(s[1].cycle_est, 100u); // the return is the last instruction
}

TEST(callstack_events_after_the_last_cycle_count_stay_at_the_last_clock)
{
    SymbolTable syms = make_syms();
    CallStackMachine m(syms);
    Element r1 = Element::instr_range(0x1000, 0x1008, BranchKind::DirectCall, true, 0, true, 50);
    r1.num_instr = 4;
    Element r2 = Element::instr_range(0x2000, 0x2010, BranchKind::None, false);
    r2.num_instr = 4;
    m.process(r1);
    m.process(r2);
    m.finish();
    CHECK_EQ(m.slices().size(), 2u);
    CHECK_EQ(m.slices()[0].cycle_est, 50u);
    CHECK_EQ(m.slices()[1].cycle_est, 50u);
    CHECK_EQ(m.slices()[1].truncated, true);
}
