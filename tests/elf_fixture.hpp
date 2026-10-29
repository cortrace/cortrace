// Cortrace — test fixtures: a minimal ELF32 with chosen PT_LOAD segments.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_TESTS_ELF_FIXTURE_HPP
#define CORTRACE_TESTS_ELF_FIXTURE_HPP

#include <array>
#include <cstdint>
#include <vector>

namespace elf_fixture {

// ELF32 LE with the given PT_LOAD segments {paddr, filesz, memsz}; the file
// data of each segment is filled with its index + 1.
inline std::vector<uint8_t> elf_with_segments(
    const std::vector<std::array<uint32_t, 3>>& segs, bool truncate_data = false)
{
    std::vector<uint8_t> b(52, 0);
    b[0] = 0x7f;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = 1; // 32-bit
    b[5] = 1; // little endian
    auto set32 = [&](std::size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b[at + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    auto set16 = [&](std::size_t at, uint16_t v) {
        b[at] = static_cast<uint8_t>(v);
        b[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    set32(28, 52); // e_phoff
    set16(42, 32); // e_phentsize
    set16(44, static_cast<uint16_t>(segs.size()));
    const std::size_t data_at = 52 + 32 * segs.size();
    b.resize(data_at, 0);
    std::size_t off = data_at;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const std::size_t ph = 52 + 32 * i;
        set32(ph + 0, 1); // PT_LOAD
        set32(ph + 4, static_cast<uint32_t>(off));
        set32(ph + 12, segs[i][0]);
        set32(ph + 16, segs[i][1]);
        set32(ph + 20, segs[i][2]);
        off += segs[i][1];
    }
    b.resize(truncate_data ? off - 1 : off, 0);
    std::size_t at = data_at;
    for (std::size_t i = 0; i < segs.size(); ++i)
        for (uint32_t k = 0; k < segs[i][1] && at < b.size(); ++k)
            b[at++] = static_cast<uint8_t>(i + 1);
    return b;
}

} // namespace elf_fixture

#endif // CORTRACE_TESTS_ELF_FIXTURE_HPP
