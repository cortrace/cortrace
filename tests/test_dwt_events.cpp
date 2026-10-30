// Cortrace — DWT event packets: the aux stream of the deframer and the parser.
//
// SPDX-License-Identifier: MIT
#include "cortrace/deframe.hpp"
#include "cortrace/dwt_events.hpp"
#include "test_framework.hpp"
#include "tpiu_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace cortrace;

namespace {

std::vector<AuxByte> numbered(const std::vector<uint8_t>& bytes)
{
    std::vector<AuxByte> out;
    for (std::size_t i = 0; i < bytes.size(); i++)
        out.push_back({ bytes[i], 100 + i });
    return out;
}

std::vector<DwtEvent> parse(DwtEventParser& p, const std::vector<uint8_t>& bytes)
{
    std::vector<DwtEvent> ev;
    const auto in = numbered(bytes);
    p.feed(in.data(), in.size(), ev);
    return ev;
}

} // namespace

// ---- the deframer's second stream ------------------------------------------------
TEST(deframer_aux_stream_has_the_position_among_the_main_bytes)
{
    using namespace tpiu_fixture;
    Rng rng { 7 };
    const auto a = tpiu_stream(rng, 60, 9);
    const MultiDeframeResult ref = tpiu_deframe_multi(a, DeframePhase {});
    const auto& etm = ref.streams.at(2);
    const auto& itm = ref.streams.at(1);
    const auto& etm_src = ref.src_index.at(2);
    const auto& itm_src = ref.src_index.at(1);
    CHECK(!itm.empty());

    const DeframePhase ph { 1, 0, 4 };
    const auto raw = raw_for(rng, a, ph);
    for (const std::size_t piece : { std::size_t(1 << 30), std::size_t(1), std::size_t(37) }) {
        StreamDeframer d(ph, 2);
        d.set_aux_stream(1);
        std::vector<uint8_t> main;
        std::vector<AuxByte> aux;
        for (std::size_t at = 0; at < raw.size(); at += piece)
            d.feed(raw.data() + at, std::min(piece, raw.size() - at), main, &aux);
        CHECK(main.size() >= etm.size());
        CHECK(std::equal(etm.begin(), etm.end(), main.begin()));
        CHECK_EQ(d.main_bytes(), static_cast<uint64_t>(main.size()));
        CHECK(aux.size() >= itm.size());
        for (std::size_t i = 0; i < itm.size(); i++) {
            CHECK_EQ(aux[i].value, itm[i]);
            // the ETM bytes that came before this byte in the assembled stream
            const uint64_t before
                = std::lower_bound(etm_src.begin(), etm_src.end(), itm_src[i]) - etm_src.begin();
            CHECK_EQ(aux[i].pos, before);
        }
    }

    // without an aux stream or a place to put it, nothing is kept
    StreamDeframer plain(ph, 2);
    plain.set_aux_stream(1);
    std::vector<uint8_t> main;
    plain.feed(raw.data(), raw.size(), main); // aux pointer omitted
    CHECK(std::equal(etm.begin(), etm.end(), main.begin()));
}

// ---- the parser --------------------------------------------------------------------
TEST(dwt_parser_gives_the_event_packets_with_their_place)
{
    DwtEventParser p;
    const auto ev = parse(p, { 0x05, 0x08, 0x05, 0x11 });
    CHECK_EQ(ev.size(), 2u);
    CHECK_EQ(ev[0].mask, static_cast<uint8_t>(kDwtLsu));
    CHECK_EQ(ev[0].pos, 101u); // the payload byte
    CHECK_EQ(ev[1].mask, static_cast<uint8_t>(kDwtCpi | kDwtFold));
    CHECK_EQ(ev[1].pos, 103u);
    CHECK_EQ(p.events(), 2u);
}

TEST(dwt_parser_walks_the_other_packets_to_stay_aligned)
{
    DwtEventParser p;
    const auto ev = parse(p,
        { 0x01, 0x05, // stimulus port 0, one byte that looks like an event header
            0xC0, 0x81, 0x05, // local timestamp 1, continued
            0x30, // local timestamp 2
            0x94, 0x80, 0x01, // global timestamp 1
            0xB4, 0x02, // global timestamp 2
            0x88, 0x81, 0x01, // extension, continued
            0x08, // extension
            0x70, // overflow
            0x17, 1, 2, 3, 4, // DWT PC sample: hardware source, 4 bytes
            0x16, 9, 9, // hardware source, discriminator 2, 2 bytes
            0x0D, 0x01, // hardware source, discriminator 1, 1 byte (not an event counter)
            0x04, // a header nobody uses
            0x05, 0x04 }); // the event
    CHECK_EQ(ev.size(), 1u);
    CHECK_EQ(ev[0].mask, static_cast<uint8_t>(kDwtSleep));
    CHECK_EQ(p.overflows(), 1u);
    CHECK_EQ(p.other_packets(), 1u);
    CHECK_EQ(p.rejected(), 0u);
}

TEST(dwt_parser_rejects_packets_that_are_not_ours)
{
    DwtEventParser p(kDwtLsu); // only the LSU counter is on
    const auto ev = parse(p, { 0x05, 0x01, 0x05, 0x00, 0x05, 0xC8, 0x05, 0x08 });
    CHECK_EQ(ev.size(), 1u); // the last one
    CHECK_EQ(p.rejected(), 3u); // another counter, no bit, reserved bits
    CHECK_EQ(p.events(), 1u);
}

TEST(dwt_parser_syncs_and_keeps_its_state_between_feeds)
{
    DwtEventParser p;
    CHECK(parse(p, { 0, 0, 0, 0, 0, 0x80 }).empty());
    CHECK_EQ(p.syncs(), 1u);
    CHECK(parse(p, { 0, 0, 0x80 }).empty()); // too few zeros: not a sync
    CHECK_EQ(p.syncs(), 1u);
    CHECK_EQ(p.other_packets(), 1u);

    // a packet split over feeds
    std::vector<DwtEvent> ev;
    const AuxByte h { 0x05, 1 }, v { 0x02, 2 };
    p.feed(&h, 1, ev);
    CHECK(ev.empty());
    p.feed(&v, 1, ev);
    CHECK_EQ(ev.size(), 1u);
    CHECK_EQ(ev[0].mask, static_cast<uint8_t>(kDwtExc));

    // a lost stream: the packet in progress is forgotten
    p.feed(&h, 1, ev);
    p.reset();
    const auto after = parse(p, { 0x05, 0x04 });
    CHECK_EQ(after.size(), 1u);
}
