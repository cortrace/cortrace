// Cortrace — ETMv3 (Cortex-M3/M4) decoder configuration test.
//
// SPDX-License-Identifier: MIT
#include "cortrace/decoder.hpp"
#include "test_framework.hpp"
#include <cstdint>
#include <fstream>
#include <vector>
#if defined(CORTRACE_HAVE_OPENCSD)
using namespace cortrace;
// Thumb code at 0x08000000: `movs r0, #0` then `b .`. The stream is an A-sync, an
// I-sync at 0x08000000 (Thumb) and E atoms: an ETMv3 decoder built from
// etm_version = 3 must follow the program from the I-sync address.
TEST(etmv3_decoder_follows_a_branch_from_an_isync)
{
    const std::string image = cortrace_test::temp_path("etmv3_image.bin");
    {
        std::ofstream f(image, std::ios::binary);
        const uint8_t code[] = { 0x00, 0x20, 0xfe, 0xe7 };
        f.write(reinterpret_cast<const char*>(code), sizeof code);
    }
    EtmV4Config cfg;
    cfg.etm_version = 3;
    auto dec = make_opencsd_decoder(cfg);
    CHECK(dec != nullptr);
    CHECK(dec->add_memory_image(0x08000000, image));
    std::vector<Element> got;
    dec->set_sink([&](const Element& e) { got.push_back(e); });
    const uint8_t stream[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x80, // A-sync
        0x08, 0x00, 0x01, 0x00, 0x00, 0x08, // I-sync, info 0, address 0x08000001 (Thumb)
        0x88, // P-header: two E atoms (ETMv3 has one atom per executed instruction)
        0x84, // another packet, which releases the elements held back for a branch address
    };
    CHECK(dec->process(stream, sizeof stream));
    dec->flush();
    std::vector<Element> ranges;
    for (const auto& e : got) {
        if (e.kind == ElementKind::InstrRange)
            ranges.push_back(e);
    }
    CHECK(!ranges.empty());
    if (!ranges.empty()) {
        CHECK_EQ(ranges[0].start_addr, 0x08000000u); // the I-sync address, Thumb bit removed
        CHECK(ranges[0].num_instr >= 1);
        CHECK(ranges[0].last_executed);
    }
}
#endif
