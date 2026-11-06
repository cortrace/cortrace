// Cortrace — call-graph, per-function and per-thread statistics.
//
// SPDX-License-Identifier: MIT
#include "cortrace/callstats.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace cortrace {

namespace {

    constexpr int64_t kIsrBase = int64_t(1) << 20;

    // A call-stack machine context is a thread's base stack or the stack of one
    // ISR track.
    int64_t ctx_key(int track, int thread) { return track > 0 ? kIsrBase + track : thread; }

    struct Frame {
        std::string name;
        std::string caller;
        std::string path; // folded stack, root first
        uint64_t run_at_begin = 0; // run clock of the context at the begin
        uint64_t losses_at_begin = 0;
        uint64_t child_incl = 0; // inclusive cycles of the intact callees
        // Inclusive cycles of the intact calls of THIS function that are nested in
        // this frame (nearest instance only): recursion.
        uint64_t same_child_incl = 0;
    };

    struct Ctx {
        std::vector<Frame> stack;
        std::map<std::string, int> active; // open frames per function (recursion)
        uint64_t run = 0; // cycles this context was the running one
    };

    struct FnAcc {
        CallFunctionStat st;
        std::vector<uint32_t> durs;
    };

    uint64_t pct(const std::vector<uint32_t>& sorted, int q)
    {
        if (sorted.empty())
            return 0;
        return sorted[(sorted.size() - 1) * static_cast<size_t>(q) / 100];
    }

    void append(std::string& out, const char* fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        if (n > 0)
            out.append(
                buf, static_cast<size_t>(n) < sizeof buf ? static_cast<size_t>(n) : sizeof buf - 1);
    }

    std::string quote(const std::string& s)
    {
        std::string o = "\"";
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') {
                o += '\\';
                o += static_cast<char>(c);
            } else if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += static_cast<char>(c);
            }
        }
        return o + "\"";
    }

    class Engine {
    public:
        Engine(const std::map<int, std::string>& names, bool has_switches)
            : names_(names)
            , has_switches_(has_switches)
        {
        }

        void on_slice(const SliceEvent& ev);
        void on_switch(const ThreadSwitch& sw);
        CallStats finish();

    private:
        std::string thread_name(int id) const;
        CallThreadStat& thread(int id);
        void advance(uint64_t cc, uint64_t losses);
        void credit(uint64_t dt);
        void close_interval(bool cut);

        const std::map<int, std::string>& names_;
        bool has_switches_;
        std::map<int64_t, Ctx> ctxs_;
        std::map<std::string, FnAcc> fns_;
        std::map<std::pair<std::string, std::string>, CallEdgeStat> edges_;
        std::map<int, CallThreadStat> threads_;
        std::map<std::string, uint64_t> folded_;
        std::vector<int64_t> isr_stack_; // ISR contexts that are running, outermost first
        int base_ = kThreadBoot; // the current thread
        bool started_ = false;
        uint64_t last_cc_ = 0;
        uint64_t last_losses_ = 0;
        uint64_t first_losses_ = 0;
        uint64_t credited_ = 0;
        uint64_t dropped_ = 0;
        uint64_t crossed_frames_ = 0;
        uint64_t truncated_frames_ = 0;
        uint64_t interval_credit_ = 0;
        bool interval_lossy_ = false;
    };

    std::string Engine::thread_name(int id) const
    {
        auto it = names_.find(id);
        std::string n;
        if (it != names_.end())
            n = it->second;
        else if (id == kThreadBoot)
            n = has_switches_ ? "boot" : "main";
        else if (id == kThreadUntagged)
            n = "untagged";
        else
            n = "thread " + std::to_string(id);
        std::replace(n.begin(), n.end(), ';', ',');
        return n;
    }

    CallThreadStat& Engine::thread(int id)
    {
        auto it = threads_.find(id);
        if (it == threads_.end()) {
            CallThreadStat t;
            t.id = id;
            t.name = thread_name(id);
            it = threads_.emplace(id, t).first;
        }
        return it->second;
    }

    void Engine::credit(uint64_t dt)
    {
        const int64_t key = isr_stack_.empty() ? static_cast<int64_t>(base_) : isr_stack_.back();
        ctxs_[key].run += dt;
        credited_ += dt;
        CallThreadStat& t = thread(base_);
        if (isr_stack_.empty())
            t.run += dt;
        else
            t.isr += dt;
        interval_credit_ += dt;
    }

    void Engine::advance(uint64_t cc, uint64_t losses)
    {
        if (!started_) {
            started_ = true;
            last_cc_ = cc;
            last_losses_ = first_losses_ = losses;
            return;
        }
        const uint64_t dt = cc >= last_cc_ ? cc - last_cc_ : 0;
        if (losses != last_losses_) {
            // Trace was lost since the previous event: the cycles in between
            // cannot be placed.
            dropped_ += dt;
            last_losses_ = losses;
            interval_lossy_ = true;
        } else if (dt) {
            credit(dt);
        }
        last_cc_ = cc;
    }

    void Engine::close_interval(bool cut)
    {
        CallThreadStat& t = thread(base_);
        t.intervals++;
        if (interval_lossy_ || cut)
            t.lossy_intervals++;
        else
            t.longest = std::max(t.longest, interval_credit_);
        interval_credit_ = 0;
        interval_lossy_ = false;
    }

    void Engine::on_switch(const ThreadSwitch& sw)
    {
        advance(sw.cycle_est, sw.losses);
        close_interval(false);
        base_ = sw.to;
        thread(base_);
    }

    void Engine::on_slice(const SliceEvent& ev)
    {
        advance(ev.cycle_est, ev.losses);
        // Slices of the base context carry the thread; a stream that has no thread
        // information (kThreadUntagged) belongs to the current one.
        const int64_t key = ctx_key(ev.track, ev.thread == kThreadUntagged ? base_ : ev.thread);
        Ctx& c = ctxs_[key];
        if (ev.begin) {
            if (ev.track > 0 && c.stack.empty()) {
                isr_stack_.push_back(key);
                thread(base_).isr_entries++;
            }
            Frame f;
            f.name = ev.name;
            std::string seg = ev.name; // ';' separates the frames of a folded stack
            std::replace(seg.begin(), seg.end(), ';', ',');
            if (c.stack.empty()) {
                f.caller = "<root>";
                f.path = ev.track > 0 ? seg : thread_name(static_cast<int>(key)) + ";" + seg;
            } else {
                f.caller = c.stack.back().name;
                f.path = c.stack.back().path + ";" + seg;
            }
            f.run_at_begin = c.run;
            f.losses_at_begin = ev.losses;
            c.active[f.name]++;
            c.stack.push_back(std::move(f));
            return;
        }
        if (c.stack.empty())
            return; // an end without a begin: nothing to account
        // The end is matched by position, not by name: a tail call relabels
        // the top frame, so its end carries the new name.
        Frame f = std::move(c.stack.back());
        c.stack.pop_back();
        const bool has_outer = --c.active[f.name] > 0; // a call of f is still open below
        FnAcc& a = fns_[f.name];
        if (a.st.name.empty())
            a.st.name = f.name;
        if (ev.truncated) {
            a.st.truncated++;
            truncated_frames_++;
        } else if (ev.losses != f.losses_at_begin) {
            a.st.crossed++;
            crossed_frames_++;
        } else {
            const uint64_t incl = c.run - f.run_at_begin;
            const uint64_t self = incl >= f.child_incl ? incl - f.child_incl : 0;
            if (a.st.calls == 0 || incl < a.st.incl_min)
                a.st.incl_min = incl;
            a.st.incl_max = std::max(a.st.incl_max, incl);
            // Recursion: a call adds only the time not already in an intact call of
            // the same function nested in it, so the total is the time the function
            // was on the stack, once.
            const uint64_t fresh = incl - std::min(incl, f.same_child_incl);
            a.st.calls++;
            a.st.incl += fresh;
            a.st.self += self;
            a.durs.push_back(static_cast<uint32_t>(std::min<uint64_t>(incl, 0xFFFFFFFFu)));
            CallEdgeStat& e = edges_[{ f.caller, f.name }];
            e.caller = f.caller;
            e.callee = f.name;
            e.calls++;
            e.incl += fresh;
            if (!c.stack.empty())
                c.stack.back().child_incl += incl;
            if (has_outer) {
                for (auto it = c.stack.rbegin(); it != c.stack.rend(); ++it)
                    if (it->name == f.name) {
                        it->same_child_incl += incl;
                        break;
                    }
            }
            folded_[f.path] += self;
        }
        if (ev.track > 0 && c.stack.empty() && !isr_stack_.empty() && isr_stack_.back() == key)
            isr_stack_.pop_back();
    }

    CallStats Engine::finish()
    {
        close_interval(true);
        CallStats r;
        for (auto& kv : fns_) {
            FnAcc& a = kv.second;
            std::sort(a.durs.begin(), a.durs.end());
            a.st.p50 = pct(a.durs, 50);
            a.st.p90 = pct(a.durs, 90);
            a.st.p99 = pct(a.durs, 99);
            r.functions.push_back(a.st);
        }
        std::sort(r.functions.begin(), r.functions.end(),
            [](const CallFunctionStat& x, const CallFunctionStat& y) {
                return x.incl != y.incl ? x.incl > y.incl : x.name < y.name;
            });
        for (auto& kv : edges_)
            r.edges.push_back(kv.second);
        std::sort(r.edges.begin(), r.edges.end(), [](const CallEdgeStat& x, const CallEdgeStat& y) {
            return x.incl != y.incl ? x.incl > y.incl
                                    : (x.caller + x.callee) < (y.caller + y.callee);
        });
        for (auto& kv : threads_) {
            // A thread that never ran and was never preempted is not a row.
            const CallThreadStat& t = kv.second;
            if (t.run || t.isr || t.isr_entries || t.intervals > 1)
                r.threads.push_back(t);
        }
        std::sort(r.threads.begin(), r.threads.end(),
            [](const CallThreadStat& x, const CallThreadStat& y) {
                return x.run != y.run ? x.run > y.run : x.id < y.id;
            });
        r.folded = std::move(folded_);
        r.credited = credited_;
        r.dropped = dropped_;
        r.losses = last_losses_ - first_losses_;
        r.crossed_frames = crossed_frames_;
        r.truncated_frames = truncated_frames_;
        return r;
    }

} // namespace

CallStats compute_callstats(const std::vector<SliceEvent>& slices,
    const std::vector<ThreadSwitch>& switches, const std::map<int, std::string>& names)
{
    Engine eng(names, !switches.empty());
    size_t sw = 0;
    for (size_t i = 0; i < slices.size(); ++i) {
        while (sw < switches.size() && switches[sw].slice_index <= i)
            eng.on_switch(switches[sw++]);
        eng.on_slice(slices[i]);
    }
    while (sw < switches.size())
        eng.on_switch(switches[sw++]);
    return eng.finish();
}

std::string CallStats::folded_text() const
{
    std::string out;
    for (const auto& kv : folded)
        if (kv.second)
            append(out, "%s %llu\n", kv.first.c_str(), static_cast<unsigned long long>(kv.second));
    return out;
}

std::string CallStats::to_json(const std::string& elf_path, double sysclk_hz) const
{
    using ull = unsigned long long;
    std::string o = "{\n \"format\": \"cortrace-callstats\",\n \"version\": 1,\n";
    append(o, " \"elf\": %s,\n \"unit\": \"cycles\",\n", quote(elf_path).c_str());
    if (sysclk_hz > 0)
        append(o, " \"sysclk_hz\": %.0f,\n", sysclk_hz);
    else
        o += " \"sysclk_hz\": null,\n";
    append(o,
        " \"health\": {\"credited\": %llu, \"dropped\": %llu, \"losses\": %llu, "
        "\"crossed_frames\": %llu, \"truncated_frames\": %llu},\n",
        (ull)credited, (ull)dropped, (ull)losses, (ull)crossed_frames, (ull)truncated_frames);
    o += " \"functions\": [";
    for (size_t i = 0; i < functions.size(); ++i) {
        const CallFunctionStat& f = functions[i];
        append(o,
            "%s\n  {\"name\": %s, \"calls\": %llu, \"crossed\": %llu, \"truncated\": %llu, "
            "\"incl\": %llu, \"self\": %llu, \"min\": %llu, \"p50\": %llu, \"p90\": %llu, "
            "\"p99\": %llu, \"max\": %llu}",
            i ? "," : "", quote(f.name).c_str(), (ull)f.calls, (ull)f.crossed, (ull)f.truncated,
            (ull)f.incl, (ull)f.self, (ull)f.incl_min, (ull)f.p50, (ull)f.p90, (ull)f.p99,
            (ull)f.incl_max);
    }
    o += "\n ],\n \"edges\": [";
    for (size_t i = 0; i < edges.size(); ++i) {
        const CallEdgeStat& e = edges[i];
        append(o, "%s\n  {\"caller\": %s, \"callee\": %s, \"calls\": %llu, \"incl\": %llu}",
            i ? "," : "", quote(e.caller).c_str(), quote(e.callee).c_str(), (ull)e.calls,
            (ull)e.incl);
    }
    o += "\n ],\n \"threads\": [";
    for (size_t i = 0; i < threads.size(); ++i) {
        const CallThreadStat& t = threads[i];
        append(o,
            "%s\n  {\"id\": %d, \"name\": %s, \"run\": %llu, \"isr\": %llu, \"isr_entries\": %llu, "
            "\"intervals\": %llu, \"longest\": %llu, \"lossy_intervals\": %llu}",
            i ? "," : "", t.id, quote(t.name).c_str(), (ull)t.run, (ull)t.isr, (ull)t.isr_entries,
            (ull)t.intervals, (ull)t.longest, (ull)t.lossy_intervals);
    }
    o += "\n ]\n}\n";
    return o;
}

} // namespace cortrace
