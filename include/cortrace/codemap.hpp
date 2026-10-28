// Cortrace — static instruction map of the program image, for code coverage.
//
// The coverage engine needs the denominator: every instruction the program
// could execute, with its size and whether it is a (conditional) branch. The
// map is built once from the ELF: the executable sections, split into code and
// data by the ARM mapping symbols ($t / $a / $d), decoded instruction by
// instruction. The instruction semantics come from an IInsnDecoder; the
// OpenCSD-backed one (decoder.hpp) is the same decoder the trace decoder uses
// to follow the program, so the static map and the trace agree on what a
// branch is.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_CODEMAP_HPP
#define CORTRACE_CODEMAP_HPP

#include "cortrace/element.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cortrace {

// One static instruction.
struct Insn {
    uint32_t addr = 0;
    uint8_t size = 0; // 2 or 4 bytes
    BranchKind kind = BranchKind::None; // as a range-ending branch (classified like the trace)
    bool cond = false; // conditional, including inside a Thumb IT block
    bool waypoint = false; // ends an ETM instruction range when executed (branches, ISB, ...)
    uint32_t target = 0; // direct branch target, 0 for anything else
    uint32_t region = 0; // id of the run of address-contiguous instructions it is in
};

// State carried from one instruction to the next while scanning (Thumb IT).
struct InsnDecodeState {
    uint8_t it_remaining = 0; // instructions still covered by the last IT
};

// Decodes one Thumb instruction.
class IInsnDecoder {
public:
    virtual ~IInsnDecoder() = default;

    // `opcode` is the 4 bytes at `addr`, little endian (the first halfword in
    // the low 16 bits). Fills `out.size`, `kind`, `cond`, `waypoint`, `target`.
    // Returns false when the bytes are not a decodable instruction.
    virtual bool decode(uint32_t addr, uint32_t opcode, InsnDecodeState& st, Insn& out) = 0;
};

class CodeMap {
public:
    // From an instruction list (any order). Regions are recomputed: a new
    // region starts wherever an instruction does not follow the previous one.
    explicit CodeMap(std::vector<Insn> insns = {});

    // Decode `len` bytes of Thumb code at `base`, appending the instructions.
    // Stops at the end, at an undecodable instruction or at one that would run
    // past the end.
    static void scan_region(uint32_t base, const uint8_t* bytes, std::size_t len, IInsnDecoder& dec,
        std::vector<Insn>& out);

    // Build the map from an ELF32 little-endian file: SHF_EXECINSTR sections,
    // the $t ranges of their mapping symbols (a section without mapping
    // symbols falls back to its STT_FUNC symbols). Returns false and sets
    // `err` on failure.
    static bool from_elf(
        const std::string& path, IInsnDecoder& dec, CodeMap& out, std::string& err);

    const std::vector<Insn>& insns() const { return insns_; }
    std::size_t size() const { return insns_.size(); }

    // Index of the instruction starting exactly at `addr`, or -1.
    long index_at(uint32_t addr) const;

    // The exception vector table (handler addresses, Thumb bit cleared), when
    // the ELF has one: a section .isr_vector / .vectors / .vector_table, or a
    // symbol g_pfnVectors / _vectors / __isr_vector / __Vectors / _vector_table.
    // Empty when unknown. Entry n is the handler of exception number n.
    const std::vector<uint32_t>& vectors() const { return vectors_; }
    void set_vectors(std::vector<uint32_t> v) { vectors_ = std::move(v); }

private:
    std::vector<Insn> insns_;
    std::vector<uint32_t> vectors_;
    uint32_t lo_ = 0; // first address covered by dense_
    std::vector<int32_t> dense_; // halfword offset from lo_ -> instruction index (-1: none)
};

} // namespace cortrace

#endif // CORTRACE_CODEMAP_HPP
