// Cortrace — ElfMemory implementation (see elfmem.hpp).
//
// SPDX-License-Identifier: MIT
#include "cortrace/elfmem.hpp"

#include <cstdio>

namespace cortrace {

namespace {
    uint32_t rd32(const uint8_t* p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
            | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
} // namespace

std::shared_ptr<const ElfMemory> ElfMemory::load(const std::string& path, std::string& err)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[1 << 16];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        bytes.insert(bytes.end(), buf, buf + n);
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) {
        err = "cannot read " + path;
        return nullptr;
    }
    std::shared_ptr<const ElfMemory> mem = from_bytes(std::move(bytes), err);
    if (!mem)
        err = path + ": " + err;
    return mem;
}

std::shared_ptr<const ElfMemory> ElfMemory::from_bytes(std::vector<uint8_t> bytes, std::string& err)
{
    constexpr std::size_t kEhdr = 52; // ELF32 header
    constexpr std::size_t kPhdr = 32; // ELF32 program header
    if (bytes.size() < kEhdr || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L'
        || bytes[3] != 'F') {
        err = "not an ELF file";
        return nullptr;
    }
    if (bytes[4] != 1 /*ELFCLASS32*/ || bytes[5] != 1 /*ELFDATA2LSB*/) {
        err = "not a 32-bit little-endian ELF";
        return nullptr;
    }
    const uint32_t phoff = rd32(&bytes[28]);
    const uint16_t phentsize = rd16(&bytes[42]);
    const uint16_t phnum = rd16(&bytes[44]);
    if (phoff == 0 || phnum == 0 || phentsize < kPhdr) {
        err = "no program headers";
        return nullptr;
    }
    std::shared_ptr<ElfMemory> mem(new ElfMemory);
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint64_t at = static_cast<uint64_t>(phoff) + static_cast<uint64_t>(i) * phentsize;
        if (at + kPhdr > bytes.size()) {
            err = "program header table runs past the end of the file";
            return nullptr;
        }
        const uint8_t* ph = &bytes[static_cast<std::size_t>(at)];
        if (rd32(ph) != 1 /*PT_LOAD*/)
            continue;
        const uint32_t offset = rd32(ph + 4);
        const uint32_t paddr = rd32(ph + 12);
        const uint32_t filesz = rd32(ph + 16);
        if (filesz == 0)
            continue; // .bss only: nothing in the file
        if (static_cast<uint64_t>(offset) + filesz > bytes.size()) {
            err = "a loadable segment runs past the end of the file";
            return nullptr;
        }
        mem->regions_.push_back({ paddr, offset, filesz });
    }
    if (mem->regions_.empty()) {
        err = "no loadable segments";
        return nullptr;
    }
    mem->bytes_ = std::move(bytes);
    return mem;
}

} // namespace cortrace
