// Cortrace — nxtrace DWT data-value parser unit tests.
//
// SPDX-License-Identifier: MIT
#include "cortrace/nxtrace.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace cortrace;

namespace {

// DWT data-value WRITE packet header: bits[7:6]=10, bit3=1 (write), bit2=1
// (hardware), bits[5:4]=comparator, bits[1:0]=SS. For comparator 0, 4-byte
// write: 0x80 | 0x08 | 0x04 | 0x03 = 0x8F.
uint8_t dwt_write_hdr(int comp, int ss)
{
    return static_cast<uint8_t>(0x80 | 0x08 | 0x04 | ((comp & 3) << 4) | (ss & 3));
}

void push_le(std::vector<uint8_t>& v, uint32_t val, int n)
{
    for (int i = 0; i < n; ++i)
        v.push_back(static_cast<uint8_t>((val >> (8 * i)) & 0xFF));
}

} // namespace

TEST(nxtrace_parses_single_write_value)
{
    std::vector<uint8_t> s;
    s.push_back(dwt_write_hdr(0, 3)); // comp 0, 4 bytes
    push_le(s, 0x24000400u, 4);

    auto ev = parse_dwt_data_values(s, {});
    CHECK_EQ((long)ev.size(), 1L);
    CHECK_EQ((int)ev[0].comparator, 0);
    CHECK_EQ((int)ev[0].size, 4);
    CHECK_EQ((long)(unsigned long)ev[0].value, (long)0x24000400L);
}

TEST(nxtrace_recovers_incrementing_sequence)
{
    // Mirror the on-board §11.8 result: a run of comparator-0 4-byte writes with
    // a monotonically +1 payload, interleaved with local-timestamp packets.
    std::vector<uint8_t> s;
    for (uint32_t v = 0x496e8b; v < 0x496e8b + 8; ++v) {
        s.push_back(dwt_write_hdr(0, 3));
        push_le(s, v, 4);
        // valid LTS1: header (bits[7:6]=11, low nibble 0) + continuation bytes;
        // 0x83 has bit7=1 (more follow), 0x02 has bit7=0 (final).
        s.push_back(0xC0); // LTS1 header
        s.push_back(0x83); // continuation (bit7=1 -> more)
        s.push_back(0x02); // final payload byte (bit7=0)
    }
    auto ev = parse_dwt_data_values(s, {});
    CHECK_EQ((long)ev.size(), 8L);
    for (std::size_t i = 0; i + 1 < ev.size(); ++i)
        CHECK_EQ((long)(ev[i + 1].value - ev[i].value), 1L);
}

TEST(nxtrace_ignores_read_and_sw_packets)
{
    std::vector<uint8_t> s;
    // A software-source packet (bit2=0): should be skipped, not parsed as DWT.
    s.push_back(static_cast<uint8_t>(0x80 | 0x01)); // data-value-ish but SW source, 1B
    s.push_back(0xAA);
    // A DWT READ (bit3=0): not a write, skip.
    s.push_back(static_cast<uint8_t>(0x80 | 0x04 | 0x03)); // hardware, read, 4B
    push_le(s, 0xDEADBEEF, 4);
    // A real DWT write.
    s.push_back(dwt_write_hdr(1, 3));
    push_le(s, 0x20001000u, 4);

    auto ev = parse_dwt_data_values(s, {});
    CHECK_EQ((long)ev.size(), 1L);
    CHECK_EQ((int)ev[0].comparator, 1);
    CHECK_EQ((long)(unsigned long)ev[0].value, (long)0x20001000L);
}

TEST(nxtrace_maps_source_index)
{
    std::vector<uint8_t> s;
    s.push_back(dwt_write_hdr(0, 3));
    push_le(s, 0x11223344u, 4);
    // src_index parallel to s: pretend the header byte came from assembled
    // offset 100.
    std::vector<std::size_t> src = { 100, 101, 102, 103, 104 };

    auto ev = parse_dwt_data_values(s, src);
    CHECK_EQ((long)ev.size(), 1L);
    CHECK_EQ((long)ev[0].src_index, 100L);
}

TEST(nxtrace_build_thread_runs_intervals)
{
    // three switches at src 10, 20, 35 to TCBs A, B, A; stream ends at 50.
    std::vector<DwtEvent> ev;
    auto mk = [](std::size_t src, uint32_t v) {
        DwtEvent e;
        e.src_index = src;
        e.comparator = 0;
        e.size = 4;
        e.value = v;
        return e;
    };
    ev.push_back(mk(10, 0x20000100));
    ev.push_back(mk(20, 0x20000200));
    ev.push_back(mk(35, 0x20000100));

    auto runs = build_thread_runs(ev, nullptr, 0, /*stream_end_src=*/50);
    CHECK_EQ((long)runs.size(), 3L);
    // run 0: [10,20) TCB A ; run 1: [20,35) TCB B ; run 2: [35,50) TCB A
    CHECK_EQ((long)runs[0].begin_src, 10L);
    CHECK_EQ((long)runs[0].end_src, 20L);
    CHECK_EQ((long)runs[1].begin_src, 20L);
    CHECK_EQ((long)runs[1].end_src, 35L);
    CHECK_EQ((long)runs[2].begin_src, 35L);
    CHECK_EQ((long)runs[2].end_src, 50L);
    // same TCB -> same tid/name
    CHECK_EQ(runs[0].id.tid, runs[2].id.tid);
    CHECK(runs[0].id.tid != runs[1].id.tid);
}

TEST(nxtrace_build_thread_runs_folds_repeated_value)
{
    // An RTOS may rewrite the current-task pointer with the value it already
    // holds (FreeRTOS does on every scheduler pass). That is not a switch.
    std::vector<DwtEvent> ev;
    auto mk = [](std::size_t src, uint32_t v) {
        DwtEvent e;
        e.src_index = src;
        e.comparator = 0;
        e.size = 4;
        e.value = v;
        return e;
    };
    ev.push_back(mk(10, 0x20000100));
    ev.push_back(mk(15, 0x20000100)); // same task again
    ev.push_back(mk(20, 0x20000200));
    ev.push_back(mk(25, 0x20000200)); // same task again
    ev.push_back(mk(30, 0x20000100));

    auto runs = build_thread_runs(ev, nullptr, 0, /*stream_end_src=*/40);
    CHECK_EQ((long)runs.size(), 3L);
    CHECK_EQ((long)runs[0].begin_src, 10L);
    CHECK_EQ((long)runs[0].end_src, 20L);
    CHECK_EQ((long)runs[1].begin_src, 20L);
    CHECK_EQ((long)runs[1].end_src, 30L);
    CHECK_EQ((long)runs[2].begin_src, 30L);
    CHECK_EQ((long)runs[2].end_src, 40L);
}

TEST(nxtrace_map_resolver_uses_map_then_falls_back_to_pointer)
{
    std::map<uint32_t, ThreadId> m;
    ThreadId t;
    t.tid = 7;
    t.name = "worker";
    m[0x20000100] = t;
    auto r = make_map_resolver(m);

    CHECK_EQ(r(0x20000100).tid, 7);
    CHECK(r(0x20000100).name == "worker");
    // not in the map: raw pointer, stable tid
    CHECK(r(0x20000200).name == "tcb@0x20000200");
    CHECK_EQ(r(0x20000200).tid, r(0x20000200).tid);
    CHECK(r(0x20000200).tid != 7);
}

TEST(nxtrace_nuttx_resolver_names_thread)
{
    // Fake ELF image: a static TCB at 0x24000400 with pid=5 at +0x30 and
    // entry=0x08001234 at +0x3C. Symbol table maps 0x08001234 -> hello_main.
    SymbolTable syms;
    syms.add(0x08001200, "other_fn");
    syms.add(0x08001234, "hello_main");
    syms.finalize();

    auto reader = [](uint32_t addr, uint32_t& out) -> bool {
        switch (addr) {
        case 0x24000400 + 0x30:
            out = 5;
            return true; // pid
        case 0x24000400 + 0x3C:
            out = 0x08001240;
            return true; // entry (inside hello_main)
        default:
            return false; // not backed by the image
        }
    };
    NuttxResolver resolve(reader, syms);

    ThreadId id = resolve(0x24000400);
    CHECK_EQ(id.tid, 5);
    // name contains the resolved function and pid
    CHECK(id.name.find("hello_main") != std::string::npos);
    CHECK(id.name.find("pid 5") != std::string::npos);

    // A dynamically-allocated TCB not in the image -> pointer-formatted.
    ThreadId dyn = resolve(0x30001000);
    CHECK(dyn.name.find("tcb@0x30001000") != std::string::npos);
}

TEST(nxtrace_resolver_prefers_tcb_map)
{
    SymbolTable syms;
    syms.finalize();
    // resolver with no ELF reader; the map supplies identities for heap TCBs.
    NuttxResolver resolve(nullptr, syms);
    std::map<uint32_t, ThreadId> m;
    m[0x38000a10] = ThreadId { 2, "nxtrace_churn (pid 2)" };
    resolve.set_tcb_map(m);

    ThreadId hit = resolve(0x38000a10);
    CHECK_EQ(hit.tid, 2);
    CHECK(hit.name.find("nxtrace_churn") != std::string::npos);

    // a TCB not in the map falls back to pointer formatting
    ThreadId miss = resolve(0x38009999);
    CHECK(miss.name.find("tcb@0x38009999") != std::string::npos);
}

TEST(nxtrace_reattributes_callstack_per_thread)
{
    // Two thread runs: TCB A [10,20), TCB B [20,30). ETM call-stack slices at
    // byte 12 (in A) and byte 25 (in B) must land on different per-thread
    // tracks; a slice at byte 5 (before any switch) stays on track 0.
    std::vector<DwtEvent> ev;
    auto mk = [](std::size_t src, uint32_t v) {
        DwtEvent e;
        e.src_index = src;
        e.comparator = 0;
        e.size = 4;
        e.value = v;
        return e;
    };
    ev.push_back(mk(10, 0x20000100));
    ev.push_back(mk(20, 0x20000200));
    auto runs = build_thread_runs(ev, nullptr, 0, 30);

    std::vector<SliceEvent> etm;
    auto sl = [](uint64_t bidx, bool begin, int track) {
        SliceEvent s;
        s.byte_index = bidx;
        s.begin = begin;
        s.track = track;
        s.name = "fn";
        return s;
    };
    etm.push_back(sl(5, true, 0)); // before first switch -> stays track 0
    etm.push_back(sl(12, true, 0)); // in run A
    etm.push_back(sl(25, true, 0)); // in run B
    etm.push_back(sl(13, true, 3)); // an ISR slice (track 3) -> untouched

    std::map<int, std::string> tracks;
    auto out = reattribute_slices_to_threads(etm, runs, 2000, tracks);
    CHECK_EQ((long)out.size(), 4L);
    CHECK_EQ(out[0].track, 0); // before switch
    CHECK(out[1].track >= 2000); // run A thread track
    CHECK(out[2].track >= 2000); // run B thread track
    CHECK(out[1].track != out[2].track); // A and B on different tracks
    CHECK_EQ(out[3].track, 3); // ISR track preserved
}

TEST(nxtrace_runs_to_slices_balanced)
{
    std::vector<DwtEvent> ev;
    auto mk = [](std::size_t src, uint32_t v) {
        DwtEvent e;
        e.src_index = src;
        e.comparator = 0;
        e.size = 4;
        e.value = v;
        return e;
    };
    ev.push_back(mk(10, 0x20000100));
    ev.push_back(mk(20, 0x20000200));
    auto runs = build_thread_runs(ev, nullptr, 0, 30);

    std::map<int, std::string> tracks;
    auto sl = thread_runs_to_slices(runs, 7, tracks);
    CHECK_EQ((long)sl.size(), 4L); // 2 runs * (begin+end)
    CHECK(tracks.count(7) == 1);
    // balanced begins/ends, all on track 7, byte_index matches run bounds
    long begins = 0, ends = 0;
    for (const auto& s : sl) {
        CHECK_EQ(s.track, 7);
        if (s.begin)
            ++begins;
        else
            ++ends;
    }
    CHECK_EQ(begins, 2L);
    CHECK_EQ(ends, 2L);
    CHECK_EQ((long)sl[0].byte_index, 10L);
    CHECK_EQ((long)sl[1].byte_index, 20L);
}

TEST(nxtrace_load_tcb_map_parses_file)
{
    // Write a tcbmap file with a comment, a blank line, a malformed line, and
    // two good entries (one with a name, one name-less).
    const std::string path_str = cortrace_test::temp_path("tcbmap.txt");
    const char* path = path_str.c_str();
    {
        std::ofstream f(path);
        f << "# tcb\tpid\tname\n";
        f << "\n";
        f << "garbage-no-hex\n";
        f << "0x38000a90\t2\tworker_compute\n";
        f << "0x24000440\t0\t\n"; // name-less
    }
    auto m = load_tcb_map(path);
    std::remove(path);

    CHECK_EQ((long)m.size(), 2L);
    // named entry -> "name (pid N)"
    CHECK(m.count(0x38000a90u) == 1);
    CHECK_EQ(m[0x38000a90u].tid, 2);
    CHECK(m[0x38000a90u].name.find("worker_compute") != std::string::npos);
    CHECK(m[0x38000a90u].name.find("pid 2") != std::string::npos);
    // name-less entry -> pointer-formatted
    CHECK(m.count(0x24000440u) == 1);
    CHECK(m[0x24000440u].name.find("tcb@0x24000440") != std::string::npos);
}

TEST(nxtrace_load_tcb_map_missing_file_is_empty)
{
    auto m = load_tcb_map("/nonexistent/dir/cortrace_xyz.txt");
    CHECK_EQ((long)m.size(), 0L);
}

TEST(nxtrace_nuttx_resolver_name_and_pid_branches)
{
    // reader returns pid+entry; entry resolves to a symbol -> "fn (pid N)".
    SymbolTable syms;
    syms.add(0x08002000, "worker_fileio");
    syms.finalize();
    auto reader = [](uint32_t addr, uint32_t& out) -> bool {
        switch (addr) {
        case 0x38001148 + 0x30:
            out = 7;
            return true; // pid
        case 0x38001148 + 0x3C:
            out = 0x08002010;
            return true; // entry in worker_fileio
        default:
            return false;
        }
    };
    NuttxResolver r(reader, syms);
    ThreadId id = r(0x38001148);
    CHECK_EQ(id.tid, 7);
    CHECK(id.name.find("worker_fileio") != std::string::npos);
    CHECK(id.name.find("pid 7") != std::string::npos);

    // pid readable but entry not resolvable (no symbol / read fails) -> "pid N (tcb@..)"
    SymbolTable empty;
    empty.finalize();
    auto reader_pid_only = [](uint32_t addr, uint32_t& out) -> bool {
        if (addr == 0x30) {
            out = 9;
            return true;
        } // pid at tcb 0
        return false; // entry read fails
    };
    NuttxResolver r2(reader_pid_only, empty);
    ThreadId id2 = r2(0x0);
    CHECK_EQ(id2.tid, 9);
    CHECK(id2.name.find("pid 9") != std::string::npos);
}

TEST(nxtrace_reattribute_gap_and_no_runs)
{
    // No runs: everything stays on its input track.
    std::vector<ThreadRun> none;
    std::vector<SliceEvent> etm(1);
    etm[0].byte_index = 42;
    etm[0].track = 0;
    etm[0].begin = true;
    std::map<int, std::string> tracks;
    auto out0 = reattribute_slices_to_threads(etm, none, 2000, tracks);
    CHECK_EQ(out0[0].track, 0);

    // A slice in the gap AFTER the last run's end -> clamped, stays track 0.
    std::vector<DwtEvent> ev;
    DwtEvent e;
    e.src_index = 10;
    e.comparator = 0;
    e.size = 4;
    e.value = 0x20000100;
    ev.push_back(e);
    auto runs = build_thread_runs(ev, nullptr, 0, 20); // one run [10,20)
    std::vector<SliceEvent> etm2(1);
    etm2[0].byte_index = 50; // past last run end
    etm2[0].track = 0;
    etm2[0].begin = true;
    std::map<int, std::string> tracks2;
    auto out1 = reattribute_slices_to_threads(etm2, runs, 2000, tracks2);
    CHECK_EQ(out1[0].track, 0);
}

// ---- ITM software stimulus extraction ------------------------------------
namespace {
// Instrumentation packet header: bits[7:3]=port, bit2=0, bits[1:0]=SS.
uint8_t sw_hdr(int port, int ss) { return static_cast<uint8_t>((port << 3) | (ss & 3)); }
} // namespace

TEST(itm_extract_reassembles_port_payload_in_order)
{
    std::vector<uint8_t> s;
    s.push_back(sw_hdr(1, 3)); // port 1, 4 bytes
    push_le(s, 0x04030201u, 4);
    s.push_back(sw_hdr(1, 2)); // port 1, 2 bytes
    push_le(s, 0x0605u, 2);
    s.push_back(sw_hdr(1, 1)); // port 1, 1 byte
    s.push_back(0x07);
    ItmStats st;
    auto out = extract_itm_stimulus(s, 1, &st);
    CHECK_EQ((long)out.size(), 7L);
    for (int i = 0; i < 7; ++i)
        CHECK_EQ((long)out[i], (long)(i + 1));
    CHECK_EQ((long)st.port_packets, 3L);
}

TEST(itm_extract_ignores_other_ports_and_hw_packets)
{
    std::vector<uint8_t> s;
    s.push_back(sw_hdr(2, 1)); // another port
    s.push_back(0xAA);
    s.push_back(dwt_write_hdr(0, 3)); // DWT hardware packet: must not be copied
    push_le(s, 0x24000400u, 4);
    s.push_back(sw_hdr(1, 1));
    s.push_back(0x55);
    ItmStats st;
    auto out = extract_itm_stimulus(s, 1, &st);
    CHECK_EQ((long)out.size(), 1L);
    CHECK_EQ((long)out[0], 0x55L);
    CHECK_EQ((long)st.sw_packets, 2L);
    CHECK_EQ((long)st.hw_packets, 1L);
}

TEST(itm_extract_skips_timestamps_between_packets)
{
    std::vector<uint8_t> s;
    s.push_back(sw_hdr(1, 3));
    push_le(s, 0x11223344u, 4);
    s.push_back(0xC0); // LTS1 header
    s.push_back(0xB3); // continuation
    s.push_back(0x21); // final payload byte
    s.push_back(0x30); // LTS2
    s.push_back(0x00); // zero / sync
    s.push_back(sw_hdr(1, 1));
    s.push_back(0x99);
    ItmStats st;
    auto out = extract_itm_stimulus(s, 1, &st);
    CHECK_EQ((long)out.size(), 5L);
    CHECK_EQ((long)out[4], 0x99L);
    CHECK_EQ((long)st.timestamps, 2L);
}

TEST(itm_extract_flags_truncated_tail)
{
    std::vector<uint8_t> s;
    s.push_back(sw_hdr(1, 3));
    s.push_back(0x01); // 4-byte packet cut short
    ItmStats st;
    auto out = extract_itm_stimulus(s, 1, &st);
    CHECK_EQ((long)out.size(), 0L);
    CHECK(st.truncated);
}

TEST(itm_extract_counts_overflow_packets)
{
    std::vector<uint8_t> s;
    s.push_back(sw_hdr(1, 1));
    s.push_back(0x11);
    s.push_back(0x70); // ITM overflow
    s.push_back(sw_hdr(1, 1));
    s.push_back(0x22);
    ItmStats st;
    auto out = extract_itm_stimulus(s, 1, &st);
    CHECK_EQ((long)out.size(), 2L);
    CHECK_EQ((long)st.overflows, 1L);
    CHECK_EQ((long)st.other, 0L);
}

// ---- note timestamp unwrap ------------------------------------------------
namespace {
// A 16-byte note header with a given 32-bit systime and no body.
void push_note(std::vector<uint8_t>& v, uint32_t systime, int type = 3)
{
    v.push_back(16); // nc_length
    v.push_back(static_cast<uint8_t>(type));
    v.push_back(100); // priority
    v.push_back(0); // cpu
    push_le(v, 2, 4); // pid
    push_le(v, systime, 4); // systime low
    push_le(v, 0, 4); // systime high (raw 32-bit counter)
}
uint64_t note_systime(const std::vector<uint8_t>& v, std::size_t note_idx)
{
    uint64_t t = 0;
    for (int b = 0; b < 8; ++b)
        t |= static_cast<uint64_t>(v[note_idx * 16 + 8 + b]) << (8 * b);
    return t;
}
} // namespace

TEST(note_unwrap_extends_across_a_counter_wrap)
{
    std::vector<uint8_t> s;
    push_note(s, 0xFFFFFF00u);
    push_note(s, 0xFFFFFFF0u);
    push_note(s, 0x00000100u); // wrapped
    push_note(s, 0x00000200u);
    NoteUnwrapStats st;
    unwrap_note_systime(s, &st);
    CHECK_EQ((long)st.wraps, 1L);
    CHECK_EQ((long)st.notes, 4L);
    CHECK_EQ((long)note_systime(s, 0), 0xFFFFFF00L);
    CHECK_EQ((long long)note_systime(s, 2), (1LL << 32) + 0x100);
    CHECK_EQ((long long)note_systime(s, 3), (1LL << 32) + 0x200);
}

TEST(note_unwrap_keeps_a_late_note_in_the_old_epoch)
{
    std::vector<uint8_t> s;
    push_note(s, 0xFFFFFFF0u);
    push_note(s, 0x00000010u); // wrap
    push_note(s, 0xFFFFFFF8u); // slightly late, stamped before the wrap
    push_note(s, 0x00000020u);
    NoteUnwrapStats st;
    unwrap_note_systime(s, &st);
    CHECK_EQ((long)st.wraps, 1L);
    CHECK_EQ((long long)note_systime(s, 2), 0xFFFFFFF8LL);
    CHECK_EQ((long long)note_systime(s, 3), (1LL << 32) + 0x20);
}

TEST(note_unwrap_is_identity_without_a_wrap)
{
    std::vector<uint8_t> s;
    push_note(s, 100);
    push_note(s, 200);
    auto copy = s;
    NoteUnwrapStats st;
    unwrap_note_systime(s, &st);
    CHECK_EQ((long)st.wraps, 0L);
    CHECK(s == copy);
}

TEST(note_unwrap_skips_garbage_and_resyncs)
{
    std::vector<uint8_t> s = { 0xAA, 0x01, 0x02 }; // stream starts mid-note
    push_note(s, 500);
    NoteUnwrapStats st;
    unwrap_note_systime(s, &st);
    CHECK_EQ((long)st.notes, 1L);
    CHECK(st.skipped_bytes >= 3);
}

TEST(nxtrace_parse_skips_protocol_packets_and_stops_at_a_truncated_one)
{
    const std::vector<uint8_t> b = {
        0x00, // sync / idle zero
        0x10, // local timestamp, one byte
        0xC0, 0x81, 0x05, // local timestamp with continuation bytes
        0x08, // some other single-byte protocol packet
        0x8F, 0x78, 0x56, 0x34, 0x12, // data-value write, comparator 0, 4 bytes
        0x9F, 0x01, 0x02, // comparator 1 but only 2 of its 4 bytes: truncated
    };
    auto ev = parse_dwt_data_values(b, {});
    CHECK_EQ(ev.size(), static_cast<std::size_t>(1));
    CHECK_EQ(static_cast<long>(ev[0].src_index), 6L);
    CHECK_EQ(static_cast<int>(ev[0].comparator), 0);
    CHECK_EQ(ev[0].value, static_cast<uint32_t>(0x12345678));
}

TEST(nxtrace_itm_counts_other_protocol_bytes)
{
    // 0x08 is a protocol byte we only skip; 0x0B is a software packet on port 1.
    const std::vector<uint8_t> b = { 0x08, 0x0B, 1, 2, 3, 4 };
    ItmStats st;
    auto out = extract_itm_stimulus(b, 1, &st);
    CHECK_EQ(out.size(), static_cast<std::size_t>(4));
    CHECK_EQ(st.other, static_cast<std::size_t>(1));
    CHECK_EQ(st.port_packets, static_cast<std::size_t>(1));
}

TEST(nxtrace_thread_runs_without_a_resolver_name_the_pointer)
{
    std::vector<DwtEvent> ev(3);
    ev[0].src_index = 10;
    ev[0].comparator = 0;
    ev[0].value = 0x20001000;
    ev[1].src_index = 15;
    ev[1].comparator = 1; // another comparator: not the watched one
    ev[1].value = 0x20009000;
    ev[2].src_index = 20;
    ev[2].comparator = 0;
    ev[2].value = 0x20002000;
    auto runs = build_thread_runs(ev, {}, 0, 99);
    CHECK_EQ(runs.size(), static_cast<std::size_t>(2));
    CHECK_EQ(runs[0].id.name, std::string("tcb@0x20001000"));
    CHECK_EQ(static_cast<long>(runs[0].end_src), 20L); // closed by the next switch
    CHECK_EQ(static_cast<long>(runs[1].end_src), 99L); // closed at the end of the stream
}

TEST(nxtrace_resolver_names_the_thread_after_its_entry_when_the_pid_is_unreadable)
{
    SymbolTable syms;
    syms.add(0x08000100, "worker");
    syms.finalize();
    const uint32_t tcb = 0x20000000;
    NuttxResolver r(
        [&](uint32_t addr, uint32_t& out) {
            if (addr == tcb + 0x3C) { // entry field only; the pid read fails
                out = 0x08000104;
                return true;
            }
            return false;
        },
        syms);
    ThreadId t = r(tcb);
    CHECK_EQ(t.name, std::string("worker")); // no "(pid N)" suffix
}

TEST(nxtrace_load_tcb_map_trims_trailing_whitespace_from_names)
{
    const std::string path = cortrace_test::temp_path("tcbmap_ws.txt");
    {
        std::ofstream f(path);
        f << "0x38000a90\t2\tworker \t\r\n"; // trailing blanks and a CR
    }
    auto m = load_tcb_map(path);
    std::remove(path.c_str());
    CHECK_EQ(m[0x38000a90u].name, std::string("worker (pid 2)"));
}

TEST(nxtrace_thread_slices_skip_runs_that_never_closed)
{
    std::vector<ThreadRun> runs(2);
    runs[0].begin_src = 5;
    runs[0].end_src = 5; // still open: no slice
    runs[0].id.name = "open";
    runs[1].begin_src = 5;
    runs[1].end_src = 9;
    runs[1].id.name = "closed";
    std::map<int, std::string> names;
    auto s = thread_runs_to_slices(runs, 7, names);
    CHECK_EQ(s.size(), static_cast<std::size_t>(2)); // begin + end of the closed run
    CHECK_EQ(s[0].name, std::string("closed"));
    CHECK_EQ(names[7], std::string("Threads"));
}

TEST(nxtrace_reattribute_believes_the_thread_a_slice_was_tagged_with)
{
    // A function of thread 1 that is switched out and ends in thread 1's next
    // run: both of its boundaries belong on thread 1's lane. Placing them by
    // byte index would put the begin in run 1 and the end in run 3 -- fine --
    // but a slice emitted while thread 2 is the run at that index (the end of a
    // parked thread at end-of-trace) would land on thread 2's lane.
    std::vector<DwtEvent> ev;
    auto mk = [](std::size_t src, uint32_t v) {
        DwtEvent e;
        e.src_index = src;
        e.comparator = 0;
        e.size = 4;
        e.value = v;
        return e;
    };
    ev.push_back(mk(10, 0x20000100)); // thread A
    ev.push_back(mk(20, 0x20000200)); // thread B
    auto runs = build_thread_runs(ev, nullptr, 0, 30);

    auto sl = [](uint64_t bidx, bool begin, int thread) {
        SliceEvent s;
        s.byte_index = bidx;
        s.begin = begin;
        s.track = 0;
        s.name = "fn";
        s.thread = thread;
        return s;
    };
    const int tid_a = runs[0].id.tid;
    const int tid_b = runs[1].id.tid;
    std::vector<SliceEvent> in;
    in.push_back(sl(12, true, tid_a)); // inside A's run
    in.push_back(sl(25, false, tid_a)); // emitted in B's run, but A's slice
    in.push_back(sl(25, true, tid_b));
    in.push_back(sl(15, true, kThreadBoot)); // boot frame closed late
    std::map<int, std::string> tracks;
    auto out = reattribute_slices_to_threads(in, runs, 2000, tracks);
    CHECK(out[0].track >= 2000);
    CHECK_EQ(out[0].track, out[1].track); // same lane as its begin
    CHECK(out[2].track != out[1].track); // thread B has its own lane
    CHECK_EQ(out[3].track, 0); // boot context stays on the boot lane
    CHECK((long)out[3].byte_index <= 10L); // clamped to the first switch
}
