// Cortrace — the program memory of an ELF, held in RAM.
//
// A decoder follows the program by reading the instructions at the addresses
// the trace gives. OpenCSD's file-backed reader keeps one open file per path
// in a process-wide table, which several decoders on several threads cannot
// share. ElfMemory reads the file once and hands every decoder the same
// read-only bytes instead.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_ELFMEM_HPP
#define CORTRACE_ELFMEM_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cortrace {

class ElfMemory {
public:
    // One PT_LOAD segment that has bytes in the file: `size` bytes at
    // data() + offset are the memory at the load (physical) address `addr`.
    // The physical address is what puts .data where the flash holds it.
    struct Region {
        uint32_t addr = 0;
        std::size_t offset = 0;
        std::size_t size = 0;
    };

    // ELF32 little-endian only (the Cortex-M target). Returns nullptr and sets
    // `err` when the file cannot be read or has no loadable segment.
    static std::shared_ptr<const ElfMemory> load(const std::string& path, std::string& err);
    static std::shared_ptr<const ElfMemory> from_bytes(
        std::vector<uint8_t> bytes, std::string& err);

    const uint8_t* data() const { return bytes_.data(); }
    std::size_t size() const { return bytes_.size(); }
    const std::vector<Region>& regions() const { return regions_; }

private:
    ElfMemory() = default;
    std::vector<uint8_t> bytes_;
    std::vector<Region> regions_;
};

} // namespace cortrace

#endif // CORTRACE_ELFMEM_HPP
