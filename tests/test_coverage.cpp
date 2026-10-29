// Cortrace — CoverageSink / AsyncScanner unit tests.
//
// SPDX-License-Identifier: MIT
#include "cortrace/coverage.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace cortrace;

namespace {

Insn mk(uint32_t addr, BranchKind kind = BranchKind::None, bool cond = false, uint32_t target = 0,
    bool waypoint = false)
{
    Insn in;
    in.addr = addr;
    in.size = 2;
    in.kind = kind;
    in.cond = cond;
    in.target = target;
    in.waypoint = waypoint || kind != BranchKind::None;
    return in;
}

// 0x100 nop | 0x102 BEQ 0x110 | 0x104 nop | 0x106 BL 0x120 | 0x108 nop | 0x10a B 0x100
// 0x110 nop | 0x112 BX LR
// 0x120 nop | 0x122 BX LR | 0x124 BX R3 | 0x126 BLX R2 | 0x128 ISB
// 0x130 nop | 0x132 BX LR (handler of exception 15)
CodeMap test_map()
{
    CodeMap m({ mk(0x100), mk(0x102, BranchKind::Direct, true, 0x110), mk(0x104),
        mk(0x106, BranchKind::DirectCall, false, 0x120), mk(0x108),
        mk(0x10a, BranchKind::Direct, false, 0x100), mk(0x110), mk(0x112, BranchKind::Return),
        mk(0x120), mk(0x122, BranchKind::Return), mk(0x124, BranchKind::IndirectBranch),
        mk(0x126, BranchKind::IndirectCall), mk(0x128, BranchKind::None, false, 0, true), mk(0x130),
        mk(0x132, BranchKind::Return) });
    std::vector<uint32_t> vec(16, 0);
    vec[15] = 0x130;
    m.set_vectors(vec);
    return m;
}

Element range(uint32_t st, uint32_t en, BranchKind br, bool exec, uint64_t idx, bool cond = false)
{
    Element e = Element::instr_range(st, en, br, exec, idx);
    e.last_instr_sz = 2;
    e.last_instr_cond = cond;
    e.num_instr = (en - st) / 2;
    return e;
}

uint32_t hits_at(const CoverageSink& s, uint32_t addr)
{
    return s.hits()[static_cast<std::size_t>(s.map().index_at(addr))];
}

// Segments start at 100, 200, 300, ...: element index 150 is in segment 1.
void segments(CoverageSink& s, int n)
{
    for (int i = 1; i <= n; ++i)
        s.add_segment_start(static_cast<uint64_t>(i) * 100);
}

} // namespace

TEST(async_scanner_finds_packets_across_chunks)
{
    AsyncScanner sc;
    std::vector<uint64_t> out;
    std::vector<uint8_t> a(20, 0x00); // 20 zeros: the A-sync is the last 11 + 0x80
    sc.scan(a.data(), a.size(), 1000, out);
    const uint8_t b[] = { 0x80, 0x55, 0, 0, 0, 0x80 };
    sc.scan(b, sizeof b, 1020, out);
    CHECK_EQ(out.size(), std::size_t(1));
    CHECK_EQ(out[0], uint64_t(1009));
}

TEST(coverage_single_segment_commits_at_finish)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 1);
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 10, true));
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 11));
    s.finish(50);
    CHECK_EQ(hits_at(s, 0x100), 1u);
    CHECK_EQ(hits_at(s, 0x106), 1u);
    CHECK_EQ(hits_at(s, 0x108), 0u);
    CHECK_EQ(s.health().committed, uint64_t(1));
    CHECK_EQ(s.health().flow_checked, uint64_t(1));
    CHECK_EQ(s.health().cond_outcomes, uint64_t(1));
    const long b = m.index_at(0x102);
    CHECK_EQ(s.not_taken()[b], 1u);
    CHECK_EQ(s.taken()[b], 0u);
}

TEST(coverage_unannounced_element_index_stays_in_the_open_segment)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(Element::simple(ElementKind::Timestamp, 5));
    s.on_element(Element::simple(ElementKind::Exception, 6));
    s.finish(0);
    CHECK_EQ(s.health().segments, uint64_t(1));
    CHECK_EQ(s.health().committed, uint64_t(1));
}

TEST(coverage_last_of_several_segments_is_not_trusted)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    segments(s, 1);
    s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 50, true));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 150));
    s.finish(200);
    CHECK_EQ(hits_at(s, 0x100), 1u);
    CHECK_EQ(hits_at(s, 0x110), 0u); // in the final, unconfirmed segment
    CHECK_EQ(s.health().quarantined, uint64_t(1));
    CHECK_EQ(s.health().quarantined_bytes, uint64_t(100));
}

TEST(coverage_flow_mismatch_drops_the_waiting_segments)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 1);
    segments(s, 3);
    s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 50, true)); // taken -> 0x110
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 150)); // not 0x110
    s.on_element(range(0x120, 0x124, BranchKind::Return, true, 250)); // returns to 0x108
    s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 260));
    s.on_element(Element::simple(ElementKind::Benign, 350)); // closes segment 3
    s.finish(400);
    CHECK_EQ(s.health().flow_mismatch, uint64_t(1));
    CHECK_EQ(hits_at(s, 0x100), 0u);
    CHECK_EQ(hits_at(s, 0x104), 0u);
    CHECK_EQ(hits_at(s, 0x120), 1u); // segment 2 is clean; segment 3 (empty) is the tail
    CHECK_EQ(s.health().return_checked, uint64_t(1));
    CHECK_EQ(s.health().return_mismatch, uint64_t(0)); // the call stack was reset by the taint
}

TEST(coverage_loss_drops_lag_segments_and_startup_sync_is_fine)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 2);
    segments(s, 5);
    s.on_element(Element::simple(ElementKind::NoSync, 1)); // stream start: normal
    s.on_element(Element::simple(ElementKind::TraceOn, 2));
    s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 50, true));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 150));
    s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 250));
    s.on_element(Element::simple(ElementKind::NoSync, 350)); // drops the 2 waiting + this one
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 450, true));
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 460));
    s.on_element(Element::simple(ElementKind::Benign, 550));
    s.finish(600);
    CHECK_EQ(s.health().lost_sync, uint64_t(1));
    CHECK_EQ(s.health().trace_on, uint64_t(0));
    CHECK_EQ(hits_at(s, 0x110), 0u);
    CHECK_EQ(hits_at(s, 0x104), 1u); // segment 5 (clean) committed, 6 is the tail
    CHECK_EQ(s.health().committed + s.health().quarantined, s.health().segments);
}

TEST(coverage_other_losses)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 1, true));
    s.on_element(Element::simple(ElementKind::TraceOn, 2));
    s.on_element(Element::simple(ElementKind::AddrNacc, 3));
    s.on_element(Element::simple(ElementKind::Overflow, 4));
    s.on_element(Element::simple(ElementKind::Benign, 5));
    s.finish(10);
    const CoverageHealth& h = s.health();
    CHECK_EQ(h.trace_on, uint64_t(1));
    CHECK_EQ(h.addr_nacc, uint64_t(1));
    CHECK_EQ(h.overflow, uint64_t(1));
    CHECK_EQ(h.losses(), uint64_t(3));
    CHECK_EQ(hits_at(s, 0x100), 0u);
}

TEST(coverage_static_cross_checks)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(range(0x200, 0x204, BranchKind::None, false, 1)); // not code
    s.on_element(range(0x100, 0x103, BranchKind::None, false, 2)); // not a boundary
    Element e = range(0x100, 0x104, BranchKind::Direct, false, 3, true);
    e.num_instr = 3;
    s.on_element(e);
    s.on_element(range(0x104, 0x108, BranchKind::Direct, true, 4)); // static: BL
    s.on_element(range(0x120, 0x124, BranchKind::Return, true, 5, true)); // static: not cond
    Element nosz = Element::instr_range(0x100, 0x104, BranchKind::Direct, false, 6);
    s.on_element(nosz); // derive the last instruction without the size
    Element wide = Element::instr_range(0x100, 0x10e, BranchKind::None, false, 7);
    s.on_element(wide); // ends past 0x10c: no instruction ends there
    s.finish(10);
    const CoverageHealth& h = s.health();
    CHECK_EQ(h.outside_code, uint64_t(3));
    CHECK_EQ(h.count_mismatch, uint64_t(1));
    CHECK_EQ(h.kind_mismatch, uint64_t(1));
    CHECK_EQ(h.cond_mismatch, uint64_t(1));
    CHECK(h.mismatches() >= 6);
    CHECK_EQ(hits_at(s, 0x100), 0u);
}

TEST(coverage_returns_follow_the_shadow_call_stack)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 1)); // push 0x108
    s.on_element(range(0x120, 0x124, BranchKind::Return, true, 2));
    s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 3)); // back at the caller
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 4, true));
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 5));
    s.on_element(range(0x120, 0x124, BranchKind::Return, true, 6));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 7)); // not 0x108
    s.finish(10);
    CHECK_EQ(s.health().return_checked, uint64_t(2));
    CHECK_EQ(s.health().return_mismatch, uint64_t(1));
}

TEST(coverage_frames_below_an_exception_return_are_checked_loosely)
{
    // An OS may switch threads in an exception handler and resume at the PC the
    // other thread was interrupted at, so nothing tells the two apart: the
    // frames on the shadow stack at that point are only checked to land after
    // a call. Frames pushed afterwards are still exact.
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 1)); // push 0x108
    s.on_element(range(0x120, 0x122, BranchKind::None, false, 2)); // cut by the exception
    Element exc = Element::exception(15, 3);
    exc.has_return_addr = true;
    exc.return_addr = 0x122;
    s.on_element(exc);
    s.on_element(range(0x130, 0x134, BranchKind::Return, true, 4)); // EXC_RETURN
    s.on_element(Element::simple(ElementKind::ExceptionRet, 5));
    s.on_element(range(0x122, 0x124, BranchKind::Return, true, 6)); // resumes at the same PC
    s.on_element(range(0x128, 0x12a, BranchKind::None, true, 7)); // after BLX R2, not 0x108
    s.on_element(Element::simple(ElementKind::ExceptionRet, 8)); // nothing pending: reset
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 9));
    s.on_element(range(0x120, 0x124, BranchKind::Return, true, 10));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 11)); // not 0x108: exact
    s.finish(12);
    const CoverageHealth& h = s.health();
    CHECK_EQ(h.exception_return_elsewhere, uint64_t(0));
    CHECK_EQ(h.flow_mismatch, uint64_t(0));
    CHECK_EQ(h.return_checked, uint64_t(2));
    CHECK_EQ(h.return_mismatch, uint64_t(1));
}
TEST(coverage_return_without_caller_must_land_after_a_call)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 1)); // nothing pushed
    s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 2)); // after BL @0x106: ok
    s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 3, true));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 4));
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 5)); // after BEQ: bad
    s.finish(10);
    CHECK_EQ(s.health().return_checked, uint64_t(2));
    CHECK_EQ(s.health().return_mismatch, uint64_t(1));
}

TEST(coverage_exceptions_vector_and_return)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 1, true));
    Element exc = Element::exception(15, 2);
    exc.has_return_addr = true;
    exc.return_addr = 0x104;
    s.on_element(exc);
    s.on_element(range(0x130, 0x134, BranchKind::Return, true, 3)); // vector ok, EXC_RETURN
    s.on_element(Element::simple(ElementKind::ExceptionRet, 4));
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 5)); // back where taken
    s.on_element(exc);
    s.on_element(range(0x120, 0x124, BranchKind::Return, true, 6)); // not the vector
    s.on_element(exc);
    s.on_element(range(0x130, 0x132, BranchKind::None, false, 7)); // cut short
    s.on_element(Element::simple(ElementKind::ExceptionRet, 8));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 9)); // a thread switch
    s.finish(10);
    const CoverageHealth& h = s.health();
    CHECK_EQ(h.flow_mismatch, uint64_t(1));
    CHECK_EQ(h.exception_return_elsewhere, uint64_t(1));
    CHECK_EQ(h.return_mismatch, uint64_t(0));
    CoverageSink t(m, 0); // an exception number past the table is not checked
    t.on_element(Element::exception(40, 1));
    t.on_element(range(0x120, 0x124, BranchKind::Return, true, 2));
    t.on_element(Element::simple(ElementKind::ExceptionRet, 3)); // nothing pending
    t.on_element(range(0x110, 0x114, BranchKind::Return, true, 4));
    t.finish(10);
    CHECK_EQ(t.health().flow_mismatch, uint64_t(0));
}

TEST(coverage_indirect_targets_and_waypoints)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(range(0x120, 0x126, BranchKind::IndirectBranch, true, 1));
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 2, true));
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 3));
    s.on_element(range(0x120, 0x128, BranchKind::IndirectCall, true, 4));
    s.on_element(range(0x128, 0x12a, BranchKind::None, true, 5)); // ISB: falls through
    s.finish(10);
    const auto& t = s.indirect_targets();
    CHECK_EQ(t.size(), std::size_t(2));
    CHECK(t.at(0x124).count(0x100) == 1);
    CHECK(t.at(0x126).count(0x128) == 1);
}

TEST(coverage_counts_distinct_segments)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    segments(s, 3);
    s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 50, true));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 60));
    s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 150)); // after the BL
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 160, true));
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 170));
    s.on_element(range(0x120, 0x124, BranchKind::Return, true, 180));
    s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 190));
    s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 195, true));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 250));
    s.on_element(Element::simple(ElementKind::Benign, 350));
    s.finish(400);
    CHECK_EQ(s.health().mismatches(), uint64_t(0));
    const long i = m.index_at(0x100);
    CHECK_EQ(s.hits()[i], 3u);
    CHECK_EQ(s.segments_seen()[i], uint16_t(2));
    const long b = m.index_at(0x102);
    CHECK_EQ(s.taken()[b], 2u);
    CHECK_EQ(s.taken_segments()[b], uint16_t(2));
    CHECK_EQ(s.not_taken()[b], 1u);
    CHECK_EQ(s.not_taken_segments()[b], uint16_t(1));
}

TEST(coverage_segment_starts_must_increase)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.add_segment_start(0); // the stream start: ignored
    s.add_segment_start(100);
    s.add_segment_start(100); // repeated: ignored
    s.add_segment_start(50); // going back: ignored
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 150, true));
    s.finish(200);
    CHECK_EQ(s.health().segments, uint64_t(2));
}

TEST(coverage_deep_nesting_is_bounded)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    Element exc = Element::exception(15, 1);
    exc.has_return_addr = true;
    exc.return_addr = 0x100;
    for (int i = 0; i < 70; ++i)
        s.on_element(exc);
    for (int i = 0; i < 5000; ++i) {
        s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 2));
        s.on_element(range(0x120, 0x122, BranchKind::None, false, 3)); // cut by an exception
        s.on_element(Element::exception(99, 4));
    }
    s.finish(10);
    CHECK(s.health().ranges > 0);
}

TEST(coverage_write_json)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 1, true));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 2));
    s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 3));
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 4, true));
    s.on_element(range(0x104, 0x108, BranchKind::DirectCall, true, 5));
    s.on_element(range(0x120, 0x126, BranchKind::IndirectBranch, true, 6));
    s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 7, true));
    s.finish(10);
    CHECK_EQ(s.health().mismatches(), uint64_t(0));
    const std::string p = cortrace_test::temp_path("objcov.json");
    CHECK(s.write_json(p, "fw \"x\"\\\n.elf"));
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string j = ss.str();
    CHECK(j.find("\"format\": \"cortrace-objcov\"") != std::string::npos);
    CHECK(j.find("\"elf\": \"fw \\\"x\\\"\\\\\\u000a.elf\"") != std::string::npos);
    CHECK(j.find("[256, 2, 0, 3, 1]") != std::string::npos); // 0x100 hit 3x in 1 segment
    CHECK(j.find("[258, 2, 25, 3, 1]") != std::string::npos); // BEQ: Direct|cond|waypoint
    CHECK(j.find("[258, 2, 1, 1, 1]") != std::string::npos); // taken 2, not taken 1
    CHECK(j.find("[292, [256]]") != std::string::npos); // BX R3 -> 0x100
    std::remove(p.c_str());
    CHECK(!s.write_json("/nonexistent-dir/x.json", "fw.elf"));
}

TEST(coverage_commit_limit_leaves_the_overlap_to_the_next_chunk)
{
    const CodeMap m = test_map();
    // The chunk is [0, 300); segments 3 and 4 are the overlap that follows it.
    auto feed = [&](CoverageSink& s) {
        segments(s, 4);
        s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 50)); // segment 0
        s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 150, true)); // segment 1
        s.on_element(range(0x110, 0x112, BranchKind::None, false, 250)); // segment 2
        s.on_element(Element::simple(ElementKind::NoSync, 350)); // segment 3: a loss
        s.on_element(range(0x120, 0x122, BranchKind::None, false, 450)); // segment 4
        s.finish(500);
    };
    CoverageSink whole(m, 1);
    feed(whole);
    CHECK_EQ(whole.health().lost_sync, uint64_t(1));
    CHECK_EQ(whole.health().segments, uint64_t(5));

    CoverageSink chunk(m, 1);
    chunk.set_commit_limit(300);
    feed(chunk);
    const CoverageHealth h = chunk.health();
    // The loss in the overlap still drops the segment before it ...
    CHECK_EQ(hits_at(chunk, 0x108), 1u);
    CHECK_EQ(hits_at(chunk, 0x100), 1u);
    CHECK_EQ(hits_at(chunk, 0x110), 0u);
    CHECK_EQ(h.committed, uint64_t(2));
    CHECK_EQ(h.quarantined, uint64_t(1));
    CHECK_EQ(h.quarantined_bytes, uint64_t(100));
    // ... but neither it nor the overlap's segments are this chunk's to count.
    CHECK_EQ(h.lost_sync, uint64_t(0));
    CHECK_EQ(h.segments, uint64_t(3));
    CHECK_EQ(hits_at(chunk, 0x120), 0u);
}

TEST(coverage_discontinuity_drops_everything_decoded_so_far)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 2);
    segments(s, 1);
    s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 50));
    s.on_element(range(0x100, 0x104, BranchKind::Direct, false, 150, true));
    s.mark_discontinuity();
    s.finish(200);
    CHECK_EQ(hits_at(s, 0x108), 0u);
    CHECK_EQ(hits_at(s, 0x100), 0u);
    CHECK_EQ(s.health().committed, uint64_t(0));
    CHECK_EQ(s.health().quarantined, uint64_t(2));
}

TEST(coverage_accumulator_adds_sinks_and_pipeline_events)
{
    const CodeMap m = test_map();
    CoverageAccumulator acc(m, 0);
    for (int i = 0; i < 3; ++i) {
        CoverageSink s(m, 0);
        s.on_element(range(0x100, 0x104, BranchKind::Direct, i != 1, 1, true));
        s.on_element(range(i != 1 ? 0x110 : 0x104, i != 1 ? 0x112 : 0x108,
            i != 1 ? BranchKind::None : BranchKind::DirectCall, false, 2));
        s.on_element(Element::simple(ElementKind::Benign, 3));
        s.finish(10);
        acc.merge(s);
    }
    CHECK_EQ(acc.hits()[static_cast<std::size_t>(m.index_at(0x100))], uint64_t(3));
    CHECK_EQ(acc.hits()[static_cast<std::size_t>(m.index_at(0x110))], uint64_t(2));
    CHECK_EQ(acc.hits()[static_cast<std::size_t>(m.index_at(0x106))], uint64_t(1));
    CHECK_EQ(acc.instructions_hit(), uint64_t(5));
    CHECK_EQ(acc.health().committed, uint64_t(3));
    CHECK_EQ(acc.health().cond_outcomes, uint64_t(3));
    CoverageHealth ev;
    ev.udp_gaps = 2;
    ev.udp_lost_frames = 7;
    ev.dropped_chunks = 1;
    acc.add_health(ev);
    CHECK_EQ(acc.health().losses(), uint64_t(3));
    CHECK_EQ(acc.health().udp_lost_frames, uint64_t(7));

    const std::string p = cortrace_test::temp_path("acc.json");
    CHECK(acc.write_json(p, "fw.elf"));
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string j = ss.str();
    CHECK(j.find("\"udp_gaps\": 2") != std::string::npos);
    CHECK(j.find("\"dropped_chunks\": 1") != std::string::npos);
    CHECK(j.find("[256, 2, 0, 3, 3]") != std::string::npos); // 0x100: 3 hits in 3 segments
    CHECK(j.find("[258, 2, 1, 2, 1]") != std::string::npos); // BEQ: taken 2x, not taken 1x
    std::ifstream tmp(p + ".tmp"); // renamed, not left behind
    CHECK(!tmp.good());
    std::remove(p.c_str());
    CHECK(!acc.write_json("/nonexistent-dir/x.json", "fw.elf"));
}

TEST(coverage_accumulator_file_is_the_sink_file)
{
    const CodeMap m = test_map();
    CoverageSink s(m, 0);
    s.on_element(range(0x100, 0x104, BranchKind::Direct, true, 1, true));
    s.on_element(range(0x110, 0x114, BranchKind::Return, true, 2));
    s.on_element(range(0x108, 0x10c, BranchKind::Direct, true, 3));
    s.finish(10);
    CoverageAccumulator acc(m, 0);
    acc.merge(s);
    const std::string a = cortrace_test::temp_path("same_a.json");
    const std::string b = cortrace_test::temp_path("same_b.json");
    CHECK(s.write_json(a, "fw.elf"));
    CHECK(acc.write_json(b, "fw.elf"));
    std::ifstream fa(a), fb(b);
    std::stringstream sa, sb;
    sa << fa.rdbuf();
    sb << fb.rdbuf();
    CHECK(!sa.str().empty());
    CHECK(sa.str() == sb.str());
    std::remove(a.c_str());
    std::remove(b.c_str());
}
