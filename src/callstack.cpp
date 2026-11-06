// Cortrace — CallStackMachine implementation.
//
// Reconstruction rules and blind-spot handling are ported from the validated
// proof of concept: call graph matched the ELF edge-for-edge (0 mismatch) with
// balanced nesting on an offline CoreMark slice.
//
// Multi-track model: the main thread runs on track 0; each exception (keyed by
// exception number) runs on its OWN Perfetto track, so ISRs appear as separate
// swim-lanes instead of nesting on the function they preempted. Contexts stack
// (a higher-priority IRQ can preempt an ISR), mirroring Cortex-M exception
// preemption.
//
// SPDX-License-Identifier: MIT
#include "cortrace/callstack.hpp"

#include "cortrace/log.hpp"

#include <string>

namespace cortrace {

std::string exception_name(uint32_t number)
{
    switch (number) {
    case 2:
        return "IRQ:NMI";
    case 3:
        return "IRQ:HardFault";
    case 11:
        return "IRQ:SVCall";
    case 14:
        return "IRQ:PendSV";
    case 15:
        return "IRQ:SysTick";
    default:
        return number >= 16 ? "IRQ:ext" + std::to_string(number - 16)
                            : "IRQ:" + std::to_string(number);
    }
}

CallStackMachine::CallStackMachine(const SymbolTable& syms)
    : syms_(syms)
{
    // Context 0 = main thread on track 0, seeded with a root frame so an early
    // return does not underflow.
    track_names_[0] = "main thread";
    ctx_.push_back({ 0, { { "<root>", 0 } } });
}

void CallStackMachine::emit(bool begin, const std::string& name)
{
    // Only the base context is a thread; ISR contexts keep their own track and
    // are not attributed to a thread.
    const int thread = ctx_.size() == 1 ? cur_thread_ : kThreadUntagged;
    emit_to(begin, name, cur().track, thread, cur().stack.size());
}

void CallStackMachine::emit_to(
    bool begin, const std::string& name, int track, int thread, std::size_t depth, bool truncated)
{
    slices_.push_back({ tick_++, last_byte_index_, begin, name, track, last_etm_ts_, cycle_clock_,
        thread, losses_, truncated, instr_clock_ + (range_ending_ ? range_instr_ : 0), 0 });
    if (begin) {
        begins_by_fn_[name]++;
        metrics_.begins++;
    } else {
        ends_by_fn_[name]++;
        metrics_.ends++;
    }
    // max_depth tracks the deepest single-track call stack.
    if (static_cast<int>(depth) > metrics_.max_depth)
        metrics_.max_depth = static_cast<int>(depth);
}

void CallStackMachine::close_window(uint64_t cycles)
{
    // The cycles of this count were spent on the instructions run since the last
    // one. An event at instruction i of n gets the share i/n.
    const uint64_t n = instr_clock_ - win_instr_;
    auto share = [&](uint64_t instr) {
        const uint64_t i = instr > win_instr_ ? instr - win_instr_ : 0;
        const uint64_t part = n ? (i < n ? i : n) : n;
        return win_clock_ + (n ? cycles * part / n : cycles);
    };
    for (std::size_t k = win_slice_; k < slices_.size(); ++k)
        slices_[k].cycle_est = share(slices_[k].instr);
    for (std::size_t k = win_switch_; k < switches_.size(); ++k)
        switches_[k].cycle_est = share(switches_[k].instr);
    win_clock_ += cycles;
    win_instr_ = instr_clock_;
    win_slice_ = slices_.size();
    win_switch_ = switches_.size();
}

void CallStackMachine::switch_thread(int tid)
{
    if (tid == cur_thread_)
        return;
    switches_.push_back({ slices_.size(), last_byte_index_, cycle_clock_, losses_, cur_thread_, tid,
        instr_clock_, 0 });
    auto& base = ctx_.front().stack;
    if (cur_thread_ == kThreadBoot) {
        // The context that ran before the first switch is not a thread we can
        // name or find again: its frames end where scheduling takes over.
        while (base.size() > 1) {
            emit_to(false, base.back().fn, ctx_.front().track, kThreadBoot, base.size(), true);
            base.pop_back();
        }
    } else {
        parked_[cur_thread_] = std::move(base);
    }
    auto it = parked_.find(tid);
    if (it != parked_.end()) {
        base = std::move(it->second);
        parked_.erase(it);
    } else {
        base = { { "<root>", 0 } };
    }
    cur_thread_ = tid;
    // A call announced by the last range of the thread that was left is not
    // the new thread's.
    pending_call_ = false;
}

int CallStackMachine::track_for_exception(uint32_t number)
{
    auto it = exc_track_.find(number);
    if (it != exc_track_.end())
        return it->second;
    int t = next_track_++;
    exc_track_[number] = t;
    track_names_[t] = exception_name(number);
    return t;
}

void CallStackMachine::do_call(const std::string& callee, uint32_t ret)
{
    auto& stack = cur().stack;
    // Blind-spot guard: about to push a frame whose function equals the current
    // top. If the trace lost sync since that frame was pushed, its return may
    // be in the gap: pop the stale frame first (net: re-entry, no runaway
    // depth). With no gap in between nothing was lost, so this is genuine
    // recursion and the frame stays.
    if (!stack.empty() && stack.back().fn == callee && stack.back().gap != gaps_) {
        CT_LOG_DEBUG("recovered missed return before re-entry into %s (byte %llu)", callee.c_str(),
            static_cast<unsigned long long>(last_byte_index_));
        emit(false, stack.back().fn);
        stack.pop_back();
        metrics_.recovered_missed_returns++;
    }
    const std::string& caller = stack.empty() ? std::string("<root>") : stack.back().fn;
    edges_[caller + " -> " + callee]++;
    stack.push_back({ callee, ret, gaps_ });
    emit(true, callee);
}

void CallStackMachine::do_return()
{
    auto& stack = cur().stack;
    if (stack.size() > 1) {
        have_expect_ = stack.back().ret_addr != 0;
        expect_ret_ = stack.back().ret_addr;
        expect_from_ = stack.back().fn;
        emit(false, stack.back().fn);
        stack.pop_back();
    } else if (ctx_.size() > 1) {
        // The ISR's own exit (BX LR with EXC_RETURN, POP {..., pc}) is a return
        // too; its frame is the context root and is closed by the exception
        // return element, so there is nothing to pop and nothing is wrong.
        have_expect_ = false;
    } else {
        // Per-occurrence diagnostic: on a lossy/desynced stream these fire by
        // the thousand, so keep them at DEBUG (silent by default). The total is
        // reported once at end-of-decode (metrics_.mismatched_returns); use
        // --log-level debug (redirect to a file) to see each byte offset.
        CT_LOG_DEBUG("mismatched return: stack near-empty at byte %llu (heuristic re-balanced)",
            static_cast<unsigned long long>(last_byte_index_));
        metrics_.mismatched_returns++;
    }
}

void CallStackMachine::relabel_top(const std::string& fn)
{
    auto& stack = cur().stack;
    if (!stack.empty())
        stack.back().fn = fn;
}

void CallStackMachine::process(const Element& e)
{
    last_byte_index_ = e.byte_index;

    // Accumulate the CPU-cycle clock from any element that carries a cycle
    // count (an InstrRange with has_cc, or a standalone CycleCount element).
    // This is the fine-grained (sysclk-resolution) time base used to
    // interpolate between the sparse global-timestamp anchors.
    if (e.has_cc)
        cycle_clock_ += e.cycle_count;

    // Counted for the statistics only; the stack logic below keeps its own
    // notion of a discontinuity (gaps_).
    if (e.kind == ElementKind::NoSync || e.kind == ElementKind::Overflow
        || e.kind == ElementKind::TraceOn || e.kind == ElementKind::AddrNacc)
        ++losses_;

    switch (e.kind) {
    case ElementKind::CycleCount:
        // Cycle clock already advanced above; no stack effect.
        break;

    case ElementKind::InstrRange: {
        range_instr_ = e.num_instr;
        const uint32_t start = e.start_addr;
        const uint32_t end = e.end_addr;
        auto& stack = cur().stack;

        if (have_expect_ && !after_blind_ && start != expect_ret_) {
            CT_LOG_DEBUG("return from %s should resume at 0x%x but resumed at 0x%x in %s "
                         "(byte %llu), top now %s",
                expect_from_.c_str(), expect_ret_, start, syms_.function_at(start).c_str(),
                static_cast<unsigned long long>(last_byte_index_), stack.back().fn.c_str());
        }
        have_expect_ = false;

        // An exception can take over on the branch that ends the last range it
        // saw (a POP {..., pc} or BX LR). That instruction is then run again
        // after the exception return, so the same branch shows up in two
        // ranges: the one before the exception and a one-instruction range
        // after it. The first one already acted on the stack; the second must
        // not pop (or push) a second time, or every frame below shifts by one.
        bool reexec = false;
        if (check_reexec_) {
            check_reexec_ = false;
            reexec = (e.is_call() || e.is_return()) && end == reexec_end_ && start >= reexec_start_
                && start < reexec_end_;
            if (reexec) {
                CT_LOG_DEBUG("branch at 0x%x run again after an exception return (byte %llu)",
                    start, static_cast<unsigned long long>(last_byte_index_));
                pending_call_ = false;
            }
        }

        // Resolve a pending CALL from the previous range. Confirm only if this
        // range starts exactly at a function entry; otherwise the callee body
        // was swallowed by a blind spot and OpenCSD resynced elsewhere — do not
        // fabricate a frame.
        if (pending_call_) {
            pending_call_ = false;
            if (!after_blind_ && syms_.is_function_entry(start))
                do_call(syms_.function_at(start), pending_ret_);
            else {
                CT_LOG_DEBUG("dropped call at byte %llu: range starts 0x%x, not a function "
                             "entry (callee lost to a blind spot)",
                    static_cast<unsigned long long>(last_byte_index_), start);
                metrics_.dropped_calls++;
            }
        }
        after_blind_ = false;

        // Reconcile the current track's stack with where we actually are
        // (unless we just confirmed a fresh call above, in which case top ==
        // callee already).
        if (!reexec && !stack.empty()) {
            const std::string& fn = syms_.function_at(start);
            if (stack.back().fn != fn) {
                // If `fn` matches a frame BELOW the top, the frames above it
                // returned without us seeing the return element (lost to a
                // blind spot). Unwind to that frame -- the general
                // missed-return recovery.
                int found = -1;
                for (int i = static_cast<int>(stack.size()) - 2; i >= 0; --i) {
                    if (stack[i].fn == fn) {
                        found = i;
                        break;
                    }
                }
                // A return lands in the middle of its caller, never on the
                // caller's first instruction. A range that starts exactly at a
                // function's entry without a call is a tail call (`b fn`),
                // also into a function that is already on the stack (a
                // callback that ends by jumping back into its ancestor):
                // that is not a sign that the frames above were lost.
                const bool at_entry = syms_.is_function_entry(start);
                if (found >= 0 && !at_entry) {
                    while (static_cast<int>(stack.size()) - 1 > found) {
                        CT_LOG_DEBUG("unwinding stale frame %s to reach %s (byte %llu)",
                            stack.back().fn.c_str(), fn.c_str(),
                            static_cast<unsigned long long>(last_byte_index_));
                        emit(false, stack.back().fn);
                        stack.pop_back();
                        metrics_.recovered_missed_returns++;
                    }
                } else if (found >= 0) {
                    // tail call into a function that is already on the stack
                    relabel_top(fn);
                } else if (!at_entry) {
                    // mid-function fallthrough into another symbol's tail region
                    relabel_top(fn);
                }
                // else: starts at a real function entry, not on the stack -> a
                // call path owns it; leave the frame as-is.
            }
        }

        if (e.is_call()) {
            pending_call_ = true;
            pending_ret_ = end;
        } else if (e.is_return() && !reexec) {
            range_ending_ = true; // the return is the last instruction of the range
            do_return();
        }
        range_ending_ = false;
        instr_clock_ += range_instr_;
        cur().last_start = start;
        cur().last_end = end;
        break;
    }

    case ElementKind::Exception: {
        // Preempt the current context: open a NEW context on this exception's
        // own track (one track per exception number). The ISR's functions land
        // on that track, not nested on the preempted function.
        // A call that has just been made but whose callee has not run yet is
        // not lost: the exception return resumes at the callee's entry. Park it
        // with the preempted context instead of dropping it, or the callee's
        // return would pop its caller and shift every frame below it.
        cur().pending_call = pending_call_;
        cur().pending_ret = pending_ret_;
        pending_call_ = false;
        have_expect_ = false;
        const std::string nm = exception_name(e.exception_number);
        const int track = track_for_exception(e.exception_number);
        metrics_.exceptions++;
        ctx_.push_back({ track, {} });
        // The ISR frame itself is the root of this context's stack.
        cur().stack.push_back({ nm, 0 });
        emit(true, nm);
        break;
    }

    case ElementKind::ExceptionRet: {
        // Close the current ISR context: pop all its frames (the ISR frame +
        // any functions it called), then return to the preempted context.
        if (ctx_.size() > 1) {
            auto& stack = cur().stack;
            while (!stack.empty()) {
                emit(false, stack.back().fn);
                stack.pop_back();
            }
            ctx_.pop_back();
            // Resume the preempted context: a call it had announced is still
            // waiting for its callee's first range.
            pending_call_ = cur().pending_call;
            pending_ret_ = cur().pending_ret;
            cur().pending_call = false;
            check_reexec_ = true;
            reexec_start_ = cur().last_start;
            reexec_end_ = cur().last_end;
        } else {
            pending_call_ = false;
        }
        have_expect_ = false;
        after_blind_ = !pending_call_;
        break;
    }

    case ElementKind::AddrNacc:
    case ElementKind::TraceOn:
        // Discontinuity: any call/return straddling the gap is lost. Never
        // fabricate flow across it. A call left pending here loses its callee.
        ++gaps_;
        check_reexec_ = false;
        have_expect_ = false;
        if (pending_call_) {
            pending_call_ = false;
            CT_LOG_DEBUG("dropped call at byte %llu: pending call straddled a %s discontinuity",
                static_cast<unsigned long long>(last_byte_index_),
                e.kind == ElementKind::TraceOn ? "TraceOn" : "AddrNacc");
            metrics_.dropped_calls++;
        }
        after_blind_ = true;
        break;

    case ElementKind::Timestamp:
        // Record the execution-time anchor; subsequent slices carry it so the
        // Perfetto time base can use real ETM time instead of ETF-egress time.
        last_etm_ts_ = e.timestamp;
        break;

    case ElementKind::Unknown:
        break;
    }

    if (e.has_cc)
        close_window(e.cycle_count);
}

void CallStackMachine::finish()
{
    // Close every still-open context (innermost ISR first, down to the main
    // thread root) so the emitted slice stream is balanced on every track.
    while (!ctx_.empty()) {
        auto& stack = cur().stack;
        const bool is_main = (ctx_.size() == 1);
        while (stack.size() > (is_main ? 1u : 0u)) {
            const int thread = is_main ? cur_thread_ : kThreadUntagged;
            emit_to(false, stack.back().fn, cur().track, thread, stack.size(), true);
            stack.pop_back();
        }
        if (is_main)
            break;
        ctx_.pop_back();
    }
    // The threads that were switched out still have frames open: end them on
    // their own account, innermost first.
    for (auto& kv : parked_) {
        auto& stack = kv.second;
        while (stack.size() > 1) {
            emit_to(false, stack.back().fn, ctx_.front().track, kv.first, stack.size(), true);
            stack.pop_back();
        }
    }
    parked_.clear();
    // No cycle count came after the events of the last window: they are placed at
    // the last known clock.
    for (std::size_t k = win_slice_; k < slices_.size(); ++k)
        slices_[k].cycle_est = win_clock_;
    for (std::size_t k = win_switch_; k < switches_.size(); ++k)
        switches_[k].cycle_est = win_clock_;
    win_slice_ = slices_.size();
    win_switch_ = switches_.size();
}

} // namespace cortrace
