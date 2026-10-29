// Cortrace — CodeMap unit tests (static instruction map from an ELF).
//
// SPDX-License-Identifier: MIT
#include "cortrace/codemap.hpp"
#include "cortrace/elfmem.hpp"
#include "elf_fixture.hpp"
#include "test_framework.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace cortrace;
using elf_fixture::elf_with_segments;

namespace {

// A made-up 16-bit encoding, so the map logic is tested without OpenCSD:
//   bit 15        : 32-bit instruction (the next halfword is its second half)
//   bits 14..12   : 0 other, 1 B, 2 BL, 3 BX reg, 4 BLX reg, 5 return,
//                   6 ISB-like waypoint, 7 IT (bits 3..0 = instructions covered)
//   bit 11        : conditional
//   bits 7..0     : direct target = 0x1000 + 2 * imm8
//   0xFFFF        : not an instruction
struct FakeDecoder : IInsnDecoder {
    bool decode(uint32_t addr, uint32_t opcode, InsnDecodeState& st, Insn& out) override
    {
        const uint16_t op = static_cast<uint16_t>(opcode & 0xFFFF);
        if (op == 0xFFFF)
            return false;
        out.addr = addr;
        out.size = (op & 0x8000) ? 4 : 2;
        const unsigned cls = (op >> 12) & 7;
        static const BranchKind kinds[] = { BranchKind::None, BranchKind::Direct,
            BranchKind::DirectCall, BranchKind::IndirectBranch, BranchKind::IndirectCall,
            BranchKind::Return, BranchKind::None, BranchKind::None };
        out.kind = kinds[cls];
        out.waypoint = cls >= 1 && cls <= 6;
        out.cond = (op & 0x0800) != 0;
        out.target = (cls == 1 || cls == 2) ? 0x1000 + 2u * (op & 0xFF) : 0;
        if (st.it_remaining) {
            out.cond = true;
            --st.it_remaining;
        } else if (cls == 7) {
            st.it_remaining = op & 0xF;
        }
        return true;
    }
};

void put16(std::vector<uint8_t>& b, uint16_t v)
{
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& b, uint32_t v)
{
    put16(b, static_cast<uint16_t>(v));
    put16(b, static_cast<uint16_t>(v >> 16));
}

void set32(std::vector<uint8_t>& b, std::size_t off, uint32_t v)
{
    for (int k = 0; k < 4; ++k)
        b[off + k] = static_cast<uint8_t>(v >> (8 * k));
}

struct TSym {
    std::string name;
    uint32_t value, size;
    uint8_t type; // STT_*
    uint16_t shndx;
};

// ELF32 LE with sections: 1 .text (AX) @0x1000, 2 `data_name` (A) @0x2000,
// 3 .symtab, 4 .strtab, 5 .shstrtab.
std::string write_elf(const std::string& tag, const std::vector<uint8_t>& text,
    const std::vector<TSym>& syms, const std::string& data_name = ".rodata",
    const std::vector<uint32_t>& data_words = {})
{
    std::vector<uint8_t> f(52, 0);
    auto align4 = [&] {
        while (f.size() % 4)
            f.push_back(0);
    };
    const uint32_t text_off = static_cast<uint32_t>(f.size());
    f.insert(f.end(), text.begin(), text.end());
    align4();
    const uint32_t data_off = static_cast<uint32_t>(f.size());
    for (uint32_t w : data_words)
        put32(f, w);
    std::string strtab(1, '\0');
    std::vector<uint8_t> symtab(16, 0);
    for (const auto& s : syms) {
        put32(symtab, static_cast<uint32_t>(strtab.size()));
        strtab += s.name + '\0';
        put32(symtab, s.value);
        put32(symtab, s.size);
        symtab.push_back(s.type);
        symtab.push_back(0);
        put16(symtab, s.shndx);
    }
    const uint32_t sym_off = static_cast<uint32_t>(f.size());
    f.insert(f.end(), symtab.begin(), symtab.end());
    const uint32_t str_off = static_cast<uint32_t>(f.size());
    f.insert(f.end(), strtab.begin(), strtab.end());
    const std::string shstr
        = std::string("\0.text\0", 7) + data_name + '\0' + ".symtab\0.strtab\0.shstrtab" + '\0';
    const uint32_t shstr_off = static_cast<uint32_t>(f.size());
    f.insert(f.end(), shstr.begin(), shstr.end());
    align4();
    const uint32_t sh_off = static_cast<uint32_t>(f.size());
    auto sh = [&](uint32_t name, uint32_t type, uint32_t flags, uint32_t addr, uint32_t off,
                  uint32_t size, uint32_t link) {
        put32(f, name);
        put32(f, type);
        put32(f, flags);
        put32(f, addr);
        put32(f, off);
        put32(f, size);
        put32(f, link);
        put32(f, 0);
        put32(f, 4);
        put32(f, type == 2 ? 16 : 0);
    };
    const uint32_t n_data = 7, n_sym = n_data + static_cast<uint32_t>(data_name.size()) + 1;
    sh(0, 0, 0, 0, 0, 0, 0);
    sh(1, 1, 0x6, 0x1000, text_off, static_cast<uint32_t>(text.size()), 0);
    sh(n_data, 1, 0x2, 0x2000, data_off, static_cast<uint32_t>(data_words.size() * 4), 0);
    sh(n_sym, 2, 0, 0, sym_off, static_cast<uint32_t>(symtab.size()), 4);
    sh(n_sym + 8, 3, 0, 0, str_off, static_cast<uint32_t>(strtab.size()), 0);
    sh(n_sym + 16, 3, 0, 0, shstr_off, static_cast<uint32_t>(shstr.size()), 0);
    std::memcpy(f.data(),
        "\x7f"
        "ELF",
        4);
    f[4] = 1; // ELFCLASS32
    f[5] = 1; // little endian
    f[6] = 1;
    set32(f, 0x20, sh_off);
    f[0x2E] = 40; // shentsize
    f[0x30] = 6; // shnum
    f[0x32] = 5; // shstrndx
    const std::string path = cortrace_test::temp_path(tag + ".elf");
    FILE* out = std::fopen(path.c_str(), "wb");
    std::fwrite(f.data(), 1, f.size(), out);
    std::fclose(out);
    return path;
}

std::vector<uint8_t> halfwords(std::initializer_list<uint16_t> hw)
{
    std::vector<uint8_t> b;
    for (uint16_t h : hw)
        put16(b, h);
    return b;
}

} // namespace

TEST(codemap_scan_sizes_kinds_targets)
{
    FakeDecoder dec;
    std::vector<Insn> v;
    const auto b = halfwords({ 0x0000, 0x8000, 0x1234, 0x1805, 0x5000, 0x6000 });
    CodeMap::scan_region(0x100, b.data(), b.size(), dec, v);
    CHECK_EQ(v.size(), std::size_t(5));
    CHECK_EQ(v[0].addr, 0x100u);
    CHECK_EQ(v[1].size, uint8_t(4));
    CHECK_EQ(v[2].addr, 0x106u);
    CHECK(v[2].kind == BranchKind::Direct);
    CHECK(v[2].cond);
    CHECK_EQ(v[2].target, 0x100Au);
    CHECK(v[3].kind == BranchKind::Return && v[3].waypoint);
    CHECK(v[4].kind == BranchKind::None && v[4].waypoint); // ISB-like
}

TEST(codemap_scan_tracks_it_blocks)
{
    FakeDecoder dec;
    std::vector<Insn> v;
    const auto b = halfwords({ 0x7002, 0x0000, 0x5000, 0x0000 });
    CodeMap::scan_region(0x100, b.data(), b.size(), dec, v);
    CHECK_EQ(v.size(), std::size_t(4));
    CHECK(!v[0].cond);
    CHECK(v[1].cond);
    CHECK(v[2].cond); // a return inside the IT block is conditional
    CHECK(!v[3].cond);
}

TEST(codemap_scan_stops_at_bad_or_truncated)
{
    FakeDecoder dec;
    std::vector<Insn> v;
    const auto bad = halfwords({ 0x0000, 0xFFFF, 0x0000 });
    CodeMap::scan_region(0x100, bad.data(), bad.size(), dec, v);
    CHECK_EQ(v.size(), std::size_t(1));
    v.clear();
    const auto cut = halfwords({ 0x0000, 0x8000 }); // 32-bit insn with half of it missing
    CodeMap::scan_region(0x100, cut.data(), cut.size(), dec, v);
    CHECK_EQ(v.size(), std::size_t(1));
    v.clear();
    const uint8_t odd[] = { 0x00, 0x00, 0x00 }; // a trailing single byte is ignored
    CodeMap::scan_region(0x100, odd, sizeof odd, dec, v);
    CHECK_EQ(v.size(), std::size_t(1));
}

TEST(codemap_regions_and_index)
{
    std::vector<Insn> v(4);
    v[0].addr = 0x106;
    v[0].size = 2;
    v[1].addr = 0x100;
    v[1].size = 2;
    v[2].addr = 0x102;
    v[2].size = 4;
    v[3].addr = 0x100; // duplicate
    v[3].size = 2;
    CodeMap m(v);
    CHECK_EQ(m.size(), std::size_t(3));
    CHECK_EQ(m.index_at(0x100), 0L);
    CHECK_EQ(m.index_at(0x102), 1L);
    CHECK_EQ(m.index_at(0x104), -1L); // inside the 4-byte instruction
    CHECK_EQ(m.index_at(0x106), 2L);
    CHECK_EQ(m.index_at(0x101), -1L);
    CHECK_EQ(m.index_at(0x0FE), -1L);
    CHECK_EQ(m.index_at(0x200), -1L);
    CHECK_EQ(m.insns()[0].region, m.insns()[2].region); // 0x100,0x102(4),0x106 contiguous
    std::vector<Insn> w(2);
    w[0].addr = 0x100;
    w[0].size = 2;
    w[1].addr = 0x104;
    w[1].size = 2;
    CodeMap g(w);
    CHECK(g.insns()[0].region != g.insns()[1].region);
    CHECK_EQ(CodeMap().index_at(0x100), -1L);
}

TEST(codemap_sparse_lookup_for_huge_spans)
{
    std::vector<Insn> v(3);
    v[0].addr = 0x0000;
    v[0].size = 2;
    v[1].addr = 0x10000000;
    v[1].size = 2;
    v[2].addr = 0x10000002;
    v[2].size = 2;
    CodeMap m(v);
    CHECK_EQ(m.index_at(0x0000), 0L);
    CHECK_EQ(m.index_at(0x10000002), 2L);
    CHECK_EQ(m.index_at(0x10000004), -1L);
    CHECK_EQ(m.index_at(0x08000000), -1L);
}

TEST(codemap_from_elf_uses_mapping_symbols)
{
    FakeDecoder dec;
    // 0x1000..0x1008 code, 0x1008..0x100c a literal pool, 0x100c..0x1010 code
    const auto text
        = halfwords({ 0x0000, 0x0000, 0x0000, 0x5000, 0xFFFF, 0xFFFF, 0x0000, 0x5000, 0x0000 });
    const std::string p = write_elf("map", text,
        { { "$t", 0x1000, 0, 0, 1 }, { "$d", 0x1008, 0, 0, 1 }, { "$t.1", 0x100C, 0, 0, 1 },
            { "$d.x", 0x1010, 0, 0, 1 }, { "$a", 0x1012, 0, 0, 1 }, { "$tx", 0x1000, 0, 0, 1 },
            { "f", 0x1001, 16, 2, 1 } });
    CodeMap m;
    std::string err;
    CHECK(CodeMap::from_elf(p, dec, m, err));
    CHECK_EQ(m.size(), std::size_t(6));
    CHECK_EQ(m.index_at(0x1008), -1L);
    CHECK(m.index_at(0x100C) >= 0);
    CHECK_EQ(m.index_at(0x1010), -1L); // $d again
    CHECK(m.vectors().empty());
    std::remove(p.c_str());
}

TEST(codemap_from_elf_falls_back_to_functions)
{
    FakeDecoder dec;
    const auto text = halfwords({ 0x0000, 0x5000, 0xFFFF, 0xFFFF, 0x0000, 0x5000 });
    const std::string p = write_elf("funcs", text,
        { { "f", 0x1001, 4, 2, 1 }, { "g", 0x1009, 4, 2, 1 }, { "arm", 0x1004, 4, 2, 1 },
            { "nosize", 0x1005, 0, 2, 1 }, { "obj", 0x1004, 4, 1, 1 } });
    CodeMap m;
    std::string err;
    CHECK(CodeMap::from_elf(p, dec, m, err));
    CHECK_EQ(m.size(), std::size_t(4));
    CHECK_EQ(m.index_at(0x1004), -1L);
    std::remove(p.c_str());
}

TEST(codemap_from_elf_reads_vector_table)
{
    FakeDecoder dec;
    const auto text = halfwords({ 0x0000, 0x5000 });
    const std::string a = write_elf("vec_sec", text, { { "$t", 0x1000, 0, 0, 1 } }, ".isr_vector",
        { 0x20001000, 0x1001, 0, 0x1003 });
    CodeMap m;
    std::string err;
    CHECK(CodeMap::from_elf(a, dec, m, err));
    CHECK_EQ(m.vectors().size(), std::size_t(4));
    CHECK_EQ(m.vectors()[1], 0x1000u);
    CHECK_EQ(m.vectors()[3], 0x1002u);
    std::remove(a.c_str());

    const std::string b = write_elf("vec_sym", text,
        { { "$t", 0x1000, 0, 0, 1 }, { "g_pfnVectors", 0x2004, 8, 1, 2 } }, ".rodata",
        { 0x11, 0x1001, 0x1003, 0x99 });
    CHECK(CodeMap::from_elf(b, dec, m, err));
    CHECK_EQ(m.vectors().size(), std::size_t(2));
    CHECK_EQ(m.vectors()[0], 0x1000u);
    std::remove(b.c_str());

    const std::string c = write_elf("vec_nosize", text,
        { { "$t", 0x1000, 0, 0, 1 }, { "_vectors", 0x2008, 0, 1, 2 },
            { "__Vectors", 0x3000, 0, 1, 2 } },
        ".rodata", { 0x11, 0x1001, 0x1003, 0x99 });
    CHECK(CodeMap::from_elf(c, dec, m, err));
    CHECK_EQ(m.vectors().size(), std::size_t(2)); // to the end of the section
    std::remove(c.c_str());
}

TEST(codemap_from_elf_errors)
{
    FakeDecoder dec;
    CodeMap m;
    std::string err;
    CHECK(!CodeMap::from_elf("/nonexistent/cortrace.elf", dec, m, err));
    CHECK(err.find("cannot open") != std::string::npos);

    const std::string junk = cortrace_test::temp_path("junk.elf");
    FILE* f = std::fopen(junk.c_str(), "wb");
    std::fputs("not an elf file at all, but long enough to be read as one.......", f);
    std::fclose(f);
    CHECK(!CodeMap::from_elf(junk, dec, m, err));
    CHECK(err.find("not an ELF") != std::string::npos);
    std::remove(junk.c_str());

    const auto text = halfwords({ 0x0000, 0x5000 });
    std::string p = write_elf("e64", text, { { "$t", 0x1000, 0, 0, 1 } });
    {
        FILE* g = std::fopen(p.c_str(), "r+b");
        std::fseek(g, 4, SEEK_SET);
        std::fputc(2, g); // ELFCLASS64
        std::fclose(g);
    }
    CHECK(!CodeMap::from_elf(p, dec, m, err));
    CHECK(err.find("32-bit") != std::string::npos);
    std::remove(p.c_str());

    p = write_elf("badsh", text, { { "$t", 0x1000, 0, 0, 1 } });
    {
        FILE* g = std::fopen(p.c_str(), "r+b");
        std::fseek(g, 0x30, SEEK_SET);
        std::fputc(200, g); // more section headers than the file holds
        std::fclose(g);
    }
    CHECK(!CodeMap::from_elf(p, dec, m, err));
    CHECK(err.find("section header") != std::string::npos);
    std::remove(p.c_str());

    p = write_elf("nocode", text, { { "$d", 0x1000, 0, 0, 1 } });
    CHECK(!CodeMap::from_elf(p, dec, m, err));
    CHECK(err.find("no Thumb code") != std::string::npos);
    std::remove(p.c_str());
}

// ---- ElfMemory -------------------------------------------------------------

TEST(elfmem_regions_follow_the_load_segments)
{
    std::string err;
    // flash at 0x08000000 (16 B), .data's load image at 0x08000010 (8 B),
    // .bss-only segment (no file bytes) is left out
    auto mem = ElfMemory::from_bytes(elf_with_segments({ { 0x08000000, 16, 16 },
                                         { 0x08000010, 8, 64 }, { 0x24000000, 0, 128 } }),
        err);
    CHECK(mem != nullptr);
    CHECK_EQ(mem->regions().size(), std::size_t(2));
    CHECK_EQ(mem->regions()[0].addr, 0x08000000u);
    CHECK_EQ(mem->regions()[0].size, std::size_t(16));
    CHECK_EQ(mem->regions()[1].addr, 0x08000010u);
    CHECK_EQ(mem->regions()[1].size, std::size_t(8));
    CHECK_EQ(mem->data()[mem->regions()[0].offset], 1);
    CHECK_EQ(mem->data()[mem->regions()[1].offset], 2);
    CHECK_EQ(mem->data()[mem->regions()[1].offset + 7], 2);
}

TEST(elfmem_rejects_what_it_cannot_read)
{
    std::string err;
    CHECK(ElfMemory::from_bytes({}, err) == nullptr);
    CHECK(err.find("not an ELF") != std::string::npos);

    auto elf64 = elf_with_segments({ { 0x08000000, 4, 4 } });
    elf64[4] = 2; // ELFCLASS64
    CHECK(ElfMemory::from_bytes(elf64, err) == nullptr);
    CHECK(err.find("32-bit little-endian") != std::string::npos);

    auto none = elf_with_segments({ { 0x24000000, 0, 64 } });
    CHECK(ElfMemory::from_bytes(none, err) == nullptr);
    CHECK(err.find("no loadable") != std::string::npos);

    auto cut = elf_with_segments({ { 0x08000000, 16, 16 } }, true);
    CHECK(ElfMemory::from_bytes(cut, err) == nullptr);
    CHECK(err.find("past the end") != std::string::npos);

    auto noph = elf_with_segments({ { 0x08000000, 4, 4 } });
    noph[28] = noph[29] = 0; // e_phoff = 0
    CHECK(ElfMemory::from_bytes(noph, err) == nullptr);
    CHECK(err.find("no program headers") != std::string::npos);

    auto shorttab = elf_with_segments({ { 0x08000000, 4, 4 } });
    shorttab[44] = 9; // e_phnum: the table would run off the file
    CHECK(ElfMemory::from_bytes(shorttab, err) == nullptr);
    CHECK(err.find("program header table") != std::string::npos);
}

TEST(elfmem_load_reads_a_file)
{
    const std::string p = cortrace_test::temp_path("mem.elf");
    const auto bytes = elf_with_segments({ { 0x08000000, 8, 8 } });
    {
        std::FILE* f = std::fopen(p.c_str(), "wb");
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    std::string err;
    auto mem = ElfMemory::load(p, err);
    CHECK(mem != nullptr);
    CHECK_EQ(mem->size(), bytes.size());
    std::remove(p.c_str());

    CHECK(ElfMemory::load("/nonexistent-dir/x.elf", err) == nullptr);
    CHECK(err.find("cannot open") != std::string::npos);
    const std::string bad = cortrace_test::temp_path("bad.elf");
    {
        std::FILE* f = std::fopen(bad.c_str(), "wb");
        std::fputs("plain text", f);
        std::fclose(f);
    }
    CHECK(ElfMemory::load(bad, err) == nullptr);
    CHECK(err.find(bad) != std::string::npos); // the message names the file
    std::remove(bad.c_str());
}
