// Cortrace — static instruction map implementation (see codemap.hpp).
//
// SPDX-License-Identifier: MIT
#include "cortrace/codemap.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace cortrace {

namespace {
    // A dense halfword table up to this many entries (64 MB of code); a
    // larger span falls back to a binary search.
    constexpr std::size_t kDenseMax = std::size_t(32) << 20;

    constexpr uint32_t SHT_SYMTAB = 2;
    constexpr uint32_t SHF_EXECINSTR = 0x4;
    constexpr uint8_t STT_FUNC = 2;

    uint32_t rd32(const std::vector<uint8_t>& b, std::size_t off)
    {
        return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8)
            | (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
    }

    uint16_t rd16(const std::vector<uint8_t>& b, std::size_t off)
    {
        return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
    }

    struct Section {
        uint32_t name, type, flags, addr, offset, size, link;
    };

    struct Sym {
        uint32_t value, size;
        uint16_t shndx;
        uint8_t type;
        char map; // 't', 'a', 'd' for a mapping symbol, 0 otherwise
        bool vector_name; // one of the usual names of the vector table
    };

    constexpr const char* kVectorSections[] = { ".isr_vector", ".vectors", ".vector_table" };
    constexpr const char* kVectorSymbols[]
        = { "g_pfnVectors", "_vectors", "__isr_vector", "__Vectors", "_vector_table" };
    constexpr std::size_t kMaxVectors = 512;

    template <std::size_t N> bool one_of(const char* s, const char* const (&names)[N])
    {
        for (const char* n : names)
            if (std::strcmp(s, n) == 0)
                return true;
        return false;
    }

    // "$t", "$a", "$d", optionally followed by ".anything".
    char mapping_kind(const char* name)
    {
        if (name[0] != '$' || (name[1] != 't' && name[1] != 'a' && name[1] != 'd'))
            return 0;
        return (name[2] == '\0' || name[2] == '.') ? name[1] : 0;
    }
} // namespace

CodeMap::CodeMap(std::vector<Insn> insns)
    : insns_(std::move(insns))
{
    std::sort(
        insns_.begin(), insns_.end(), [](const Insn& a, const Insn& b) { return a.addr < b.addr; });
    insns_.erase(std::unique(insns_.begin(), insns_.end(),
                     [](const Insn& a, const Insn& b) { return a.addr == b.addr; }),
        insns_.end());
    uint32_t region = 0;
    for (std::size_t i = 0; i < insns_.size(); ++i) {
        if (i > 0 && insns_[i - 1].addr + insns_[i - 1].size != insns_[i].addr)
            ++region;
        insns_[i].region = region;
    }
    if (insns_.empty())
        return;
    lo_ = insns_.front().addr;
    const uint64_t span = static_cast<uint64_t>(insns_.back().addr) + insns_.back().size - lo_;
    if (span / 2 > kDenseMax)
        return;
    dense_.assign(static_cast<std::size_t>(span / 2) + 1, -1);
    for (std::size_t i = 0; i < insns_.size(); ++i)
        dense_[(insns_[i].addr - lo_) / 2] = static_cast<int32_t>(i);
}

long CodeMap::index_at(uint32_t addr) const
{
    if (insns_.empty() || addr < lo_ || (addr & 1u))
        return -1;
    if (!dense_.empty()) {
        const std::size_t h = (addr - lo_) / 2;
        return h < dense_.size() ? dense_[h] : -1;
    }
    auto it = std::lower_bound(
        insns_.begin(), insns_.end(), addr, [](const Insn& in, uint32_t a) { return in.addr < a; });
    if (it == insns_.end() || it->addr != addr)
        return -1;
    return static_cast<long>(it - insns_.begin());
}

void CodeMap::scan_region(
    uint32_t base, const uint8_t* bytes, std::size_t len, IInsnDecoder& dec, std::vector<Insn>& out)
{
    InsnDecodeState st;
    std::size_t off = 0;
    while (off + 2 <= len) {
        uint32_t op = 0;
        for (std::size_t k = 0; k < 4 && off + k < len; ++k)
            op |= static_cast<uint32_t>(bytes[off + k]) << (8 * k);
        Insn in;
        in.addr = base + static_cast<uint32_t>(off);
        if (!dec.decode(in.addr, op, st, in) || (in.size != 2 && in.size != 4)
            || off + in.size > len)
            return;
        out.push_back(in);
        off += in.size;
    }
}

bool CodeMap::from_elf(const std::string& path, IInsnDecoder& dec, CodeMap& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    const std::vector<uint8_t> b(
        (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 52
        || std::memcmp(b.data(),
               "\x7f"
               "ELF",
               4)
            != 0) {
        err = path + ": not an ELF file";
        return false;
    }
    if (b[4] != 1 || b[5] != 1) {
        err = path + ": only 32-bit little-endian ELF is supported";
        return false;
    }
    const uint32_t shoff = rd32(b, 0x20);
    const uint16_t shentsize = rd16(b, 0x2E);
    const uint16_t shnum = rd16(b, 0x30);
    if (shentsize < 40 || static_cast<uint64_t>(shoff) + uint64_t(shnum) * shentsize > b.size()) {
        err = path + ": bad section header table";
        return false;
    }
    std::vector<Section> secs(shnum);
    for (uint16_t i = 0; i < shnum; ++i) {
        const std::size_t o = shoff + std::size_t(i) * shentsize;
        secs[i] = { rd32(b, o), rd32(b, o + 4), rd32(b, o + 8), rd32(b, o + 12), rd32(b, o + 16),
            rd32(b, o + 20), rd32(b, o + 24) };
    }
    auto in_file
        = [&](const Section& s) { return static_cast<uint64_t>(s.offset) + s.size <= b.size(); };
    // NUL-terminated string at `off` in string-table section `st`, or "".
    auto str_at = [&](const Section& st, uint32_t off) -> const char* {
        if (!in_file(st) || off >= st.size)
            return "";
        const char* p = reinterpret_cast<const char*>(b.data()) + st.offset + off;
        return std::memchr(p, '\0', st.size - off) ? p : "";
    };
    const uint16_t shstrndx = rd16(b, 0x32);
    auto sec_name = [&](const Section& s) {
        return shstrndx < secs.size() ? str_at(secs[shstrndx], s.name) : "";
    };

    std::vector<Sym> syms;
    for (const Section& s : secs) {
        if (s.type != SHT_SYMTAB || !in_file(s) || s.link >= secs.size() || !in_file(secs[s.link]))
            continue;
        const Section& str = secs[s.link];
        for (uint32_t o = s.offset; o + 16 <= s.offset + s.size; o += 16) {
            const char* name = str_at(str, rd32(b, o));
            syms.push_back({ rd32(b, o + 4), rd32(b, o + 8), rd16(b, o + 14),
                static_cast<uint8_t>(b[o + 12] & 0xF), mapping_kind(name),
                one_of(name, kVectorSymbols) });
        }
    }

    // The vector table: words from a known section, else from a known symbol.
    std::vector<uint32_t> vectors;
    auto read_words = [&](const Section& s, uint32_t addr, uint32_t bytes) {
        if (!in_file(s) || addr < s.addr || addr >= s.addr + s.size)
            return;
        bytes = std::min<uint32_t>(bytes, s.addr + s.size - addr);
        for (uint32_t k = 0; k + 4 <= bytes && vectors.size() < kMaxVectors; k += 4)
            vectors.push_back(rd32(b, s.offset + (addr - s.addr) + k) & ~1u);
    };
    for (const Section& s : secs)
        if (vectors.empty() && s.type != 8 /* NOBITS */ && one_of(sec_name(s), kVectorSections))
            read_words(s, s.addr, s.size);
    for (const Sym& y : syms)
        if (vectors.empty() && y.vector_name && y.shndx < secs.size())
            read_words(
                secs[y.shndx], y.value, y.size ? y.size : static_cast<uint32_t>(kMaxVectors * 4));

    std::vector<Insn> insns;
    for (std::size_t si = 0; si < secs.size(); ++si) {
        const Section& s = secs[si];
        if (!(s.flags & SHF_EXECINSTR) || s.type == 8 /* NOBITS */ || !in_file(s) || !s.size)
            continue;
        const uint32_t end = s.addr + s.size;
        auto scan = [&](uint32_t a, uint32_t e) {
            if (a < s.addr || e > end || a >= e)
                return;
            scan_region(a, b.data() + s.offset + (a - s.addr), e - a, dec, insns);
        };
        std::vector<std::pair<uint32_t, char>> marks;
        for (const Sym& y : syms)
            if (y.map && y.shndx == si && y.value >= s.addr && y.value < end)
                marks.emplace_back(y.value, y.map);
        std::sort(marks.begin(), marks.end());
        if (!marks.empty()) {
            for (std::size_t k = 0; k < marks.size(); ++k)
                if (marks[k].second == 't')
                    scan(marks[k].first, k + 1 < marks.size() ? marks[k + 1].first : end);
        } else {
            for (const Sym& y : syms)
                if (y.type == STT_FUNC && y.shndx == si && y.size && (y.value & 1u))
                    scan(y.value & ~1u, (y.value & ~1u) + y.size);
        }
    }
    out = CodeMap(std::move(insns));
    out.set_vectors(std::move(vectors));
    if (out.size() == 0) {
        err = path + ": no Thumb code found (no executable section with $t or function symbols)";
        return false;
    }
    return true;
}

} // namespace cortrace
