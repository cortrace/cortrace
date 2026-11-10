// Cortrace — Perfetto writer unit tests.
//
// Encode a synthetic slice stream and parse the protobuf back to assert the
// multi-track wire format: one TrackDescriptor per track, and each TrackEvent
// routed to its track's uuid. Zero external deps (hand-rolled varint reader).
//
// SPDX-License-Identifier: MIT
#include "cortrace/callstack.hpp"
#include "cortrace/perfetto_writer.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace cortrace;

namespace {

struct Reader {
    const unsigned char* p;
    std::size_t n, i = 0;
    uint64_t varint()
    {
        uint64_t v = 0;
        int s = 0;
        while (i < n) {
            unsigned char b = p[i++];
            v |= (uint64_t)(b & 0x7f) << s;
            if (!(b & 0x80))
                break;
            s += 7;
        }
        return v;
    }
};

// Parsed result: track descriptors (uuid->name) and per-track-uuid event count.
struct Parsed {
    std::map<uint64_t, std::string> tracks;
    std::map<uint64_t, int> events; // track_uuid -> #TrackEvents
};

Parsed parse(const std::string& s)
{
    Parsed out;
    Reader r { reinterpret_cast<const unsigned char*>(s.data()), s.size() };
    while (r.i < r.n) {
        uint64_t key = r.varint();
        if ((key >> 3) != 1) // expect TracePacket (field 1)
            break;
        uint64_t len = r.varint();
        std::size_t end = r.i + len;
        // parse the packet
        while (r.i < end) {
            uint64_t k = r.varint();
            int f = k >> 3, w = k & 7;
            if (w == 2) {
                uint64_t l = r.varint();
                std::size_t sub_end = r.i + l;
                if (f == 60) { // track_descriptor
                    uint64_t uuid = 0;
                    std::string name;
                    while (r.i < sub_end) {
                        uint64_t k2 = r.varint();
                        int f2 = k2 >> 3, w2 = k2 & 7;
                        if (w2 == 0) {
                            uint64_t v = r.varint();
                            if (f2 == 1)
                                uuid = v;
                        } else if (w2 == 2) {
                            uint64_t l2 = r.varint();
                            if (f2 == 2)
                                name.assign(reinterpret_cast<const char*>(r.p + r.i), l2);
                            r.i += l2;
                        }
                    }
                    out.tracks[uuid] = name;
                } else if (f == 11) { // track_event
                    uint64_t te_uuid = 0;
                    while (r.i < sub_end) {
                        uint64_t k2 = r.varint();
                        int f2 = k2 >> 3, w2 = k2 & 7;
                        if (w2 == 0) {
                            uint64_t v = r.varint();
                            if (f2 == 11)
                                te_uuid = v;
                        } else if (w2 == 2) {
                            uint64_t l2 = r.varint();
                            r.i += l2;
                        }
                    }
                    out.events[te_uuid]++;
                } else {
                    r.i = sub_end;
                }
                r.i = sub_end;
            } else if (w == 0) {
                r.varint();
            } else {
                break;
            }
        }
        r.i = end;
    }
    return out;
}

} // namespace

TEST(perfetto_multi_track_descriptors_and_routing)
{
    // main thread (track 0) + one ISR track (track 1).
    std::vector<SliceEvent> slices = {
        { 0, 0, true, "main", 0 },
        { 1, 0, true, "IRQ:SysTick", 1 },
        { 2, 0, false, "", 1 },
        { 3, 0, false, "", 0 },
    };
    std::map<int, std::string> tracks = { { 0, "main thread" }, { 1, "IRQ:SysTick" } };

    Parsed p = parse(encode_perfetto_trace_multi(slices, tracks));

    // two distinct track descriptors, distinct uuids
    CHECK_EQ((long)p.tracks.size(), 2L);
    // find uuids by name
    uint64_t main_uuid = 0, isr_uuid = 0;
    for (auto& kv : p.tracks) {
        if (kv.second == "main thread")
            main_uuid = kv.first;
        if (kv.second == "IRQ:SysTick")
            isr_uuid = kv.first;
    }
    CHECK(main_uuid != 0);
    CHECK(isr_uuid != 0);
    CHECK(main_uuid != isr_uuid);

    // 2 events on each track (1 begin + 1 end)
    CHECK_EQ(p.events[main_uuid], 2);
    CHECK_EQ(p.events[isr_uuid], 2);
}

TEST(perfetto_single_track_backcompat)
{
    std::vector<SliceEvent> slices = {
        { 0, 0, true, "f", 0 },
        { 1, 0, false, "", 0 },
    };
    // single-track encoder still produces one descriptor + two events.
    Parsed p = parse(encode_perfetto_trace(slices, "ETM callstack", 0x1001));
    CHECK_EQ((long)p.tracks.size(), 1L);
    CHECK_EQ(p.events[0x1001], 2);
}

namespace {
std::string read_file(const std::string& path)
{
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}
} // namespace

TEST(perfetto_slice_names_and_timestamps_are_in_the_encoding)
{
    std::vector<SliceEvent> slices = {
        { 123456789, 0, true, "name_that_must_appear", 0 },
        { 123456999, 0, false, "end_has_no_name", 0 },
    };
    const std::string bytes = encode_perfetto_trace(slices);
    CHECK(bytes.find("name_that_must_appear") != std::string::npos);
    CHECK(bytes.find("end_has_no_name") == std::string::npos); // only begins carry a name
    CHECK(bytes.find("ETM callstack") != std::string::npos); // default track name
}

TEST(perfetto_empty_slice_list_still_describes_the_track)
{
    Parsed p = parse(encode_perfetto_trace({}, "only a track", 0x77));
    CHECK_EQ((long)p.tracks.size(), 1L);
    CHECK(p.events.empty());
}

TEST(perfetto_write_single_track_file_matches_the_encoding)
{
    std::vector<SliceEvent> slices = { { 5, 0, true, "f", 0 }, { 9, 0, false, "", 0 } };
    const std::string path = cortrace_test::temp_path("single.perfetto");
    CHECK(write_perfetto_trace(path, slices, "trk", 0x42));
    CHECK_EQ(read_file(path), encode_perfetto_trace(slices, "trk", 0x42));
    std::remove(path.c_str());
}

TEST(perfetto_write_multi_track_file_matches_the_encoding)
{
    std::vector<SliceEvent> slices = { { 5, 0, true, "f", 0 }, { 9, 0, false, "", 0 } };
    std::map<int, std::string> tracks = { { 0, "main" } };
    const std::string path = cortrace_test::temp_path("multi.perfetto");
    CHECK(write_perfetto_trace_multi(path, slices, tracks));
    CHECK_EQ(read_file(path), encode_perfetto_trace_multi(slices, tracks));
    std::remove(path.c_str());
}

TEST(perfetto_write_reports_an_unwritable_path)
{
    std::vector<SliceEvent> slices = { { 1, 0, true, "f", 0 } };
    CHECK(!write_perfetto_trace("/nonexistent/dir/x.perfetto", slices));
    CHECK(!write_perfetto_trace_multi("/nonexistent/dir/x.perfetto", slices, { { 0, "m" } }));
}

TEST(perfetto_multi_track_without_tracks_or_slices_is_empty)
{
    CHECK(encode_perfetto_trace_multi({}, {}).empty());
}
