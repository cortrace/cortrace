// Cortrace — call-stack machine.
//
// Consumes normalised trace Elements in order and reconstructs the function
// call stack, emitting begin/end slice events. This is the deterministic core
// of cortrace and is fully unit-tested with synthetic element streams.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_CALLSTACK_HPP
#define CORTRACE_CALLSTACK_HPP

#include "cortrace/element.hpp"
#include "cortrace/symbols.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cortrace {

// Which thread a slice belongs to (SliceEvent::thread).
//   >= 0            the RTOS thread whose call stack the slice came from
//   kThreadUntagged not attributed by the machine (ISR slices; callers that do
//                   not announce thread switches): attribute by byte index
//   kThreadBoot     before any thread switch was announced (boot context)
constexpr int kThreadUntagged = -1;
constexpr int kThreadBoot = -2;

// One emitted slice boundary (begin or end of a function on the timeline).
struct SliceEvent {
    uint64_t tick = 0; // monotonic order (real ns applied by the sink via byte_index)
    uint64_t byte_index = 0; // source ETM byte offset, for time-base lookup
    bool begin = false; // true = slice begin, false = slice end
    std::string name; // function / ISR name
    int track = 0; // track id: 0 = main thread, >=1 = a per-exception ISR track
    uint64_t etm_ts = 0; // most-recent ETM global timestamp seen before this event
    uint64_t cycle_clock = 0; // accumulated CPU cycles (cycle counting) at this event
    int thread = kThreadUntagged; // owning thread, see kThreadUntagged / kThreadBoot
    // Trace losses (NoSync, Overflow, TraceOn, AddrNacc) seen before this event.
    // A begin and an end with different values have lost trace between them.
    uint64_t losses = 0;
    // An end the machine made up because the trace stopped (finish()) or the
    // boot context was left (first thread switch), not one the CPU executed.
    bool truncated = false;
    // Instructions the CPU had executed (decoder ranges) when this event happened,
    // and the CPU cycles at this event estimated from them: the cycles of a cycle
    // count are spread over the instructions it covers, so a function shorter than
    // the count interval still gets its share (cycle_clock alone only moves when a
    // count arrives, and gives such a function zero). Both are valid once the
    // slice is final: see CallStackMachine::slices().
    uint64_t instr = 0;
    uint64_t cycle_est = 0;
};

// The CPU switched from thread `from` to thread `to`. It sits between
// slices()[slice_index - 1] and slices()[slice_index].
struct ThreadSwitch {
    std::size_t slice_index = 0;
    uint64_t byte_index = 0;
    uint64_t cycle_clock = 0;
    uint64_t losses = 0;
    int from = kThreadBoot;
    int to = kThreadBoot;
    uint64_t instr = 0;
    uint64_t cycle_est = 0; // as SliceEvent::cycle_est
};

// An ETM global-timestamp anchor: the TSGEN value that was in force at a given
// source byte offset. Between consecutive anchors the host interpolates by
// byte_index so slices get a monotonically increasing time instead of all
// sharing the last anchor's value.
struct TsAnchor {
    uint64_t byte_index;
    uint64_t ts;
};

// Aggregate quality/΄shape metrics produced during reconstruction.
struct StackMetrics {
    long begins = 0;
    long ends = 0;
    int max_depth = 0;
    long mismatched_returns = 0; // return with an (almost) empty stack
    long dropped_calls = 0; // callee lost to a blind spot (not fabricated)
    long recovered_missed_returns = 0; // stale frame popped before re-entry
    long exceptions = 0; // ISR frames rendered
    bool balanced() const { return begins == ends; }
};

class CallStackMachine {
public:
    explicit CallStackMachine(const SymbolTable& syms);

    // Feed one element. Safe to call in any order; ignores kinds it does not
    // model.
    void process(const Element& e);

    // The CPU now runs thread `tid` (a write of the RTOS current-task pointer).
    // Every thread has its own call stack: the stack of the thread being left
    // is parked and the one of `tid` (empty the first time) takes its place.
    // Without this a single stack is shared by all threads, and whatever a
    // task left open when it was switched out (a call into the scheduler) is
    // popped, or left standing, by the next task's returns.
    //
    // ISR contexts have stacks of their own, so a switch announced while an
    // exception runs (the scheduler runs in PendSV) takes effect at once and
    // the exception return resumes the new thread's stack.
    void switch_thread(int tid);

    // Call once at end-of-trace: closes any still-open frames so the emitted
    // slice stream is balanced.
    void finish();

    const std::vector<SliceEvent>& slices() const { return slices_; }
    const std::vector<ThreadSwitch>& thread_switches() const { return switches_; }
    const StackMetrics& metrics() const { return metrics_; }

    // caller->callee edge -> count. Used to cross-check against the ELF `bl`
    // graph (golden criterion: every edge is backed by a real bl).
    const std::map<std::string, long>& edges() const { return edges_; }

    // track id -> track name (0 = "main thread", one track per distinct ISR
    // keyed by exception number, e.g. "IRQ:SysTick"). The sink emits one
    // Perfetto TrackDescriptor per entry.
    const std::map<int, std::string>& tracks() const { return track_names_; }

private:
    struct Frame {
        std::string fn;
        uint32_t ret_addr; // expected return address (end of the call range)
        uint64_t gap = 0; // gaps_ when the frame was pushed
    };

    // An execution context = one call stack living on one Perfetto track. The
    // main thread is context 0 (track 0). Each exception preempts the current
    // context and runs on its OWN track (one track per exception number), so
    // ISRs appear as separate swim-lanes instead of nesting on the preempted
    // function. Exceptions can nest (higher-priority IRQ preempts an ISR), so
    // contexts form a stack.
    struct Context {
        int track; // Perfetto track id for this context
        std::vector<Frame> stack; // call frames on this track
        // A call whose callee had not started yet when an exception preempted
        // this context (the BL ran, the exception came before the callee's
        // first instruction). The exception return resumes at the callee's
        // entry, so the call is still to be confirmed then.
        bool pending_call = false;
        uint32_t pending_ret = 0;
        // The last instruction range this context ran (what was reported as
        // executed before an exception took over).
        uint32_t last_start = 0;
        uint32_t last_end = 0;
    };

    void do_call(const std::string& callee, uint32_t ret);
    void do_return();
    void relabel_top(const std::string& fn);
    Context& cur() { return ctx_.back(); } // active (innermost) context
    void emit(bool begin, const std::string& name); // to the active track
    void emit_to(bool begin, const std::string& name, int track, int thread, std::size_t depth,
        bool truncated = false);
    int track_for_exception(uint32_t number); // stable track id per exc number

    const SymbolTable& syms_;
    std::vector<Context> ctx_; // context stack (0 = the running thread)
    int cur_thread_ = kThreadBoot; // thread whose stack is ctx_[0].stack
    std::map<int, std::vector<Frame>> parked_; // stacks of the threads not running
    std::vector<SliceEvent> slices_;
    std::vector<ThreadSwitch> switches_;
    std::map<std::string, long> begins_by_fn_;
    std::map<std::string, long> ends_by_fn_;
    std::map<std::string, long> edges_;
    std::map<int, std::string> track_names_; // track id -> name
    std::map<uint32_t, int> exc_track_; // exception number -> track id
    int next_track_ = 1; // next ISR track id to allocate
    StackMetrics metrics_;

    uint64_t tick_ = 0;
    uint64_t last_byte_index_ = 0;
    uint64_t last_etm_ts_ = 0; // most-recent ETM global timestamp value
    uint64_t cycle_clock_ = 0; // accumulated CPU cycles (cycle counting)
    uint64_t gaps_ = 0; // trace discontinuities (lost sync, unreachable code) so far
    uint64_t losses_ = 0; // every NoSync / Overflow / TraceOn / AddrNacc so far
    // Cycle estimate (SliceEvent::cycle_est): events since the last cycle count
    // wait for the next one, which tells how many cycles its window took.
    uint64_t instr_clock_ = 0; // instructions executed so far
    uint64_t range_instr_ = 0; // instructions of the range being processed
    bool range_ending_ = false; // events now are at the END of that range
    uint64_t win_clock_ = 0; // cycle clock at the start of the open window
    uint64_t win_instr_ = 0; // instr_clock_ at the start of the open window
    std::size_t win_slice_ = 0; // first slice / switch of the open window
    std::size_t win_switch_ = 0;
    void close_window(uint64_t cycles);
    bool pending_call_ = false;
    uint32_t pending_ret_ = 0;
    bool after_blind_ = false;
    // Set by an exception return: the first range that follows may re-run the
    // branch that the exception interrupted (see process()).
    bool check_reexec_ = false;
    uint32_t reexec_start_ = 0;
    uint32_t reexec_end_ = 0;
    // The return that was just popped promised to resume at this address.
    bool have_expect_ = false;
    uint32_t expect_ret_ = 0;
    std::string expect_from_;
};

// Map an ARM Cortex-M exception number to a readable ISR name.
std::string exception_name(uint32_t number);

} // namespace cortrace

#endif // CORTRACE_CALLSTACK_HPP
