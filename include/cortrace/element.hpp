// Cortrace — decoder-agnostic trace element.
//
// The upper layers (call-stack machine, sinks) consume `Element`s and never
// touch OpenCSD types directly. This keeps the core logic unit-testable with
// synthetic element streams, with no live decoder dependency.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_ELEMENT_HPP
#define CORTRACE_ELEMENT_HPP

#include <cstdint>

namespace cortrace {

// Kind of a decoded trace element. Mirrors the subset of OpenCSD generic
// elements that the call-stack machine cares about.
enum class ElementKind {
    Unknown,
    InstrRange, // a run of executed instructions ending in a branch/waypoint
    Exception, // exception entry (SysTick, PendSV, external IRQ, ...)
    ExceptionRet, // exception return
    TraceOn, // (re)start of trace after a discontinuity
    AddrNacc, // address not accessible: a decode blind spot
    Timestamp, // wall-clock / cycle timestamp marker
    CycleCount, // standalone cycle-count (cycles since last counted point)
    NoSync, // decoder lost sync / resync point (overflow or bad-stream symptom)
    Overflow, // explicit trace overflow (ETM/ETF dropped data)
    Benign, // modeled-but-inert element (PE_CONTEXT, SYNC_MARKER, EO_TRACE, ...):
            // counted for transparency but NOT a loss symptom
    OtherUnknown, // any other OpenCSD element we don't model (counted, not silent)
};

// Branch classification of the last instruction in an InstrRange. Derived from
// OpenCSD's last_i_type / last_i_subtype.
enum class BranchKind {
    None, // range did not end in a branch waypoint
    Direct, // direct branch (B / conditional B)
    DirectCall, // BL / BLX immediate  (subtype BR_LINK)
    IndirectCall, // BLX reg            (indirect + BR_LINK)
    IndirectBranch, // BX reg / table branch (indirect, not a return)
    Return, // pop{pc} / bx lr / RET / ERET (implied or explicit return)
};

// A single normalised trace element.
struct Element {
    ElementKind kind = ElementKind::Unknown;

    // InstrRange fields
    uint32_t start_addr = 0; // first executed instruction in the range
    uint32_t end_addr = 0; // address after the last instruction (return addr)
    BranchKind branch = BranchKind::None;
    bool last_executed = false; // was the final branch taken?
    // Detail of the last instruction and the range length, for code coverage
    // (0 / false when the decoder does not provide them).
    uint8_t last_instr_sz = 0; // size of the last instruction in bytes (2 or 4)
    bool last_instr_cond = false; // the last instruction is conditional (IT block too)
    uint32_t num_instr = 0; // instructions in the range (Thumb: not (end-start)/2)

    // Exception fields
    uint32_t exception_number = 0; // ARM exception number (15 = SysTick)
    bool has_return_addr = false; // the exception carries its preferred return address
    uint32_t return_addr = 0; // where the exception returns to (has_return_addr)

    // Timestamp fields (kind == Timestamp): the 64-bit global-timestamp value
    // OpenCSD decoded from the ETM TIMESTAMP packet. This is the SoC TSGEN
    // count sampled at execution time -- the anchor for a real (not
    // ETF-egress) time base.
    uint64_t timestamp = 0;

    // Cycle-count fields. When cycle counting is enabled (TRCCONFIGR.CCI), an
    // InstrRange can carry the number of CPU cycles it took (has_cc), and a
    // Timestamp can carry the cycles since the last counted point. Accumulated
    // on the host, this gives a CPU-cycle time base (sysclk resolution) to
    // interpolate between the sparse global-timestamp anchors.
    uint32_t cycle_count = 0;
    bool has_cc = false;

    // Provenance / timing
    uint64_t byte_index = 0; // source ETM byte offset (OpenCSD idx_sop)

    static Element instr_range(uint32_t start, uint32_t end, BranchKind br, bool exec,
        uint64_t idx = 0, bool has_cc = false, uint32_t cc = 0)
    {
        Element e;
        e.kind = ElementKind::InstrRange;
        e.start_addr = start;
        e.end_addr = end;
        e.branch = br;
        e.last_executed = exec;
        e.byte_index = idx;
        e.has_cc = has_cc;
        e.cycle_count = cc;
        return e;
    }

    static Element exception(uint32_t number, uint64_t idx = 0)
    {
        Element e;
        e.kind = ElementKind::Exception;
        e.exception_number = number;
        e.byte_index = idx;
        return e;
    }

    static Element simple(ElementKind k, uint64_t idx = 0)
    {
        Element e;
        e.kind = k;
        e.byte_index = idx;
        return e;
    }

    static Element make_timestamp(uint64_t ts, uint64_t idx = 0)
    {
        Element e;
        e.kind = ElementKind::Timestamp;
        e.timestamp = ts;
        e.byte_index = idx;
        return e;
    }

    bool is_call() const
    {
        return kind == ElementKind::InstrRange && last_executed
            && (branch == BranchKind::DirectCall || branch == BranchKind::IndirectCall);
    }

    bool is_return() const
    {
        return kind == ElementKind::InstrRange && last_executed && branch == BranchKind::Return;
    }
};

} // namespace cortrace

#endif // CORTRACE_ELEMENT_HPP
