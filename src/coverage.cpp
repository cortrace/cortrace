// Cortrace — object-code coverage implementation (see coverage.hpp).
//
// SPDX-License-Identifier: MIT
#include "cortrace/coverage.hpp"

#include <cstdio>
#include <limits>

namespace cortrace {

void AsyncScanner::scan(
    const uint8_t* data, std::size_t len, uint64_t base, std::vector<uint64_t>& out)
{
    for (std::size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];
        if (c == 0) {
            ++zeros_;
        } else {
            if (c == 0x80 && zeros_ >= 11)
                out.push_back(base + i - 11);
            zeros_ = 0;
        }
    }
}

CoverageSink::CoverageSink(const CodeMap& map, int commit_lag)
    : map_(map)
    , lag_(commit_lag < 0 ? 0 : commit_lag)
    , hits_(map.size(), 0)
    , taken_(map.size(), 0)
    , not_taken_(map.size(), 0)
    , segs_(map.size(), 0)
    , taken_segs_(map.size(), 0)
    , not_taken_segs_(map.size(), 0)
    , last_seg_(map.size(), 0)
    , last_taken_seg_(map.size(), 0)
    , last_not_taken_seg_(map.size(), 0)
{
    open_.id = next_id_++;
}

void CoverageSink::add_segment_start(uint64_t offset)
{
    if (offset <= open_.start || (!starts_.empty() && offset <= starts_.back()))
        return; // the stream start, or not increasing
    starts_.push_back(offset);
}

void CoverageSink::advance_to(uint64_t byte_index)
{
    while (!starts_.empty() && starts_.front() <= byte_index) {
        const uint64_t boundary = starts_.front();
        starts_.pop_front();
        close_open(boundary);
    }
}

void CoverageSink::close_open(uint64_t end)
{
    open_.end = end;
    ++health_.segments;
    closed_.push_back(std::move(open_));
    open_ = Segment {};
    open_.id = next_id_++;
    open_.start = end;
    while (closed_.size() > static_cast<std::size_t>(lag_)) {
        commit(closed_.front());
        closed_.pop_front();
    }
    // The flow checks carry on across the boundary: a periodic A-sync does not
    // interrupt a decoder that is in sync (one that is not reports a loss).
}

void CoverageSink::commit(Segment& s)
{
    if (s.dirty) {
        ++health_.quarantined;
        health_.quarantined_bytes += s.end > s.start ? s.end - s.start : 0;
        return;
    }
    ++health_.committed;
    health_.ranges_committed += s.ranges;
    constexpr uint32_t kMax32 = std::numeric_limits<uint32_t>::max();
    for (const auto& sp : s.spans) {
        for (uint32_t k = sp.first; k <= sp.second; ++k) {
            if (hits_[k] != kMax32)
                ++hits_[k];
            if (last_seg_[k] != s.id) {
                last_seg_[k] = s.id;
                if (segs_[k] != std::numeric_limits<uint16_t>::max())
                    ++segs_[k];
            }
        }
    }
    for (const auto& o : s.outcomes) {
        uint32_t& n = o.second ? taken_[o.first] : not_taken_[o.first];
        if (n != kMax32)
            ++n;
        uint64_t& last = o.second ? last_taken_seg_[o.first] : last_not_taken_seg_[o.first];
        uint16_t& segs = o.second ? taken_segs_[o.first] : not_taken_segs_[o.first];
        if (last != s.id) {
            last = s.id;
            if (segs != std::numeric_limits<uint16_t>::max())
                ++segs;
        }
    }
    for (const auto& t : s.indirect)
        indirect_[t.first].insert(t.second);
}

void CoverageSink::dirty_current() { open_.dirty = true; }

void CoverageSink::taint()
{
    // The bad bytes may have come before the point where it shows: a wrong
    // address decodes into a plausible path (a loop absorbs any atoms) until
    // the next A-sync puts the decoder back on the real one, where the start
    // no longer matches. So the segments still waiting are dropped as well.
    dirty_current();
    for (auto& s : closed_)
        s.dirty = true;
    // what was learnt about the nesting may come from the bad bytes too
    exc_returns_.clear();
    calls_.clear();
}

void CoverageSink::lose()
{
    taint();
    reset_flow();
}

void CoverageSink::reset_flow()
{
    expect_kind_ = Expect::None;
    indirect_site_ = -1;
}

void CoverageSink::on_exception(const Element& e)
{
    reset_flow();
    if (e.has_return_addr) {
        if (exc_returns_.size() >= kMaxExceptionDepth)
            exc_returns_.pop_front();
        exc_returns_.push_back(e.return_addr);
    }
    // The handler's calls and returns nest on top of the interrupted code's.
    if (calls_.size() >= kMaxCallDepth)
        calls_.erase(calls_.begin(), calls_.begin() + kMaxCallDepth / 2);
    calls_.push_back(kExceptionMark);
    // The handler starts at its vector, when the vector table is known.
    const auto& vec = map_.vectors();
    if (e.exception_number < vec.size() && vec[e.exception_number] != 0) {
        expect_kind_ = Expect::Exact;
        expect_ = vec[e.exception_number];
    }
}

void CoverageSink::on_exception_return()
{
    reset_flow();
    // Drop what is left of the handler's frames (normally nothing: its own
    // return popped the mark already).
    for (std::size_t i = calls_.size(); i-- > 0;) {
        if (calls_[i] == kExceptionMark) {
            calls_.resize(i);
            break;
        }
    }
    // Returns to where the innermost pending exception was taken.
    if (!exc_returns_.empty()) {
        expect_kind_ = Expect::ExceptionReturn;
        expect_ = exc_returns_.back();
        exc_returns_.pop_back();
    }
}

void CoverageSink::on_element(const Element& e)
{
    advance_to(e.byte_index);
    switch (e.kind) {
    case ElementKind::InstrRange:
        on_range(e);
        seen_range_ = true;
        break;
    case ElementKind::NoSync:
        // Before the first range this is the normal start of the stream.
        if (seen_range_) {
            ++health_.lost_sync;
            lose();
        } else {
            reset_flow();
        }
        break;
    case ElementKind::TraceOn:
        if (seen_range_) {
            ++health_.trace_on;
            lose();
        } else {
            reset_flow();
        }
        break;
    case ElementKind::AddrNacc:
        ++health_.addr_nacc;
        lose();
        break;
    case ElementKind::Overflow:
        ++health_.overflow;
        lose();
        break;
    case ElementKind::Exception:
        on_exception(e);
        break;
    case ElementKind::ExceptionRet:
        on_exception_return();
        break;
    default:
        break;
    }
}

void CoverageSink::on_range(const Element& e)
{
    ++health_.ranges;
    ++open_.ranges;
    const auto& v = map_.insns();
    const long si = map_.index_at(e.start_addr);
    long li;
    if (e.last_instr_sz) {
        li = map_.index_at(e.end_addr - e.last_instr_sz);
    } else {
        li = map_.index_at(e.end_addr - 2);
        if (li < 0 || v[li].size != 2)
            li = map_.index_at(e.end_addr - 4);
    }
    if (si < 0 || li < si || v[si].region != v[li].region
        || v[li].addr + v[li].size != e.end_addr) {
        ++health_.outside_code;
        taint();
        reset_flow();
        return;
    }
    const Insn& last = v[li];
    bool bad = false;
    if (indirect_site_ >= 0) {
        open_.indirect.emplace_back(v[indirect_site_].addr, e.start_addr);
        indirect_site_ = -1;
    }
    switch (expect_kind_) {
    case Expect::Exact:
        ++health_.flow_checked;
        if (e.start_addr != expect_) {
            ++health_.flow_mismatch;
            bad = true;
        }
        break;
    case Expect::Return:
        // The return address of the matching call (shadow call stack).
        ++health_.return_checked;
        if (e.start_addr != expect_) {
            ++health_.return_mismatch;
            bad = true;
        }
        break;
    case Expect::AfterCall: {
        // A return with no known caller still lands right after a call.
        ++health_.return_checked;
        const bool after_call = si > 0 && v[si - 1].region == v[si].region
            && (v[si - 1].kind == BranchKind::DirectCall
                || v[si - 1].kind == BranchKind::IndirectCall);
        if (!after_call) {
            ++health_.return_mismatch;
            bad = true;
        }
        break;
    }
    case Expect::ExceptionReturn:
        // Back where the exception was taken -- unless an OS switched threads
        // in the handler, which looks the same as a bad address; not a fault.
        if (e.start_addr != expect_) {
            ++health_.exception_return_elsewhere;
            calls_.clear();
        }
        break;
    case Expect::None:
        break;
    }
    if (e.num_instr && e.num_instr != static_cast<uint32_t>(li - si + 1)) {
        ++health_.count_mismatch;
        bad = true;
    }
    if (last.kind != e.branch) {
        ++health_.kind_mismatch;
        bad = true;
    }
    if (e.last_instr_sz && last.cond != e.last_instr_cond) {
        ++health_.cond_mismatch;
        bad = true;
    }
    if (bad)
        taint();
    open_.spans.emplace_back(static_cast<uint32_t>(si), static_cast<uint32_t>(li));
    if (last.waypoint && last.cond) {
        open_.outcomes.emplace_back(static_cast<uint32_t>(li), e.last_executed);
        ++health_.cond_outcomes;
    }
    // Where the next range has to start.
    expect_kind_ = Expect::Exact;
    const bool taken = last.waypoint && e.last_executed;
    if (taken && (last.kind == BranchKind::DirectCall || last.kind == BranchKind::IndirectCall)) {
        if (calls_.size() >= kMaxCallDepth)
            calls_.erase(calls_.begin(), calls_.begin() + kMaxCallDepth / 2);
        calls_.push_back(last.addr + last.size);
    }
    if (!taken) {
        expect_ = e.end_addr; // fall-through (or a range cut by an exception: reset then)
    } else if (last.kind == BranchKind::Direct || last.kind == BranchKind::DirectCall) {
        expect_ = last.target;
    } else if (last.kind == BranchKind::IndirectBranch || last.kind == BranchKind::IndirectCall) {
        expect_kind_ = Expect::None;
        indirect_site_ = li;
    } else if (last.kind == BranchKind::Return) {
        if (calls_.empty()) {
            expect_kind_ = Expect::AfterCall;
        } else if (calls_.back() == kExceptionMark) {
            // The handler's own return (EXC_RETURN): the exception return
            // element that follows says where it goes.
            calls_.pop_back();
            expect_kind_ = Expect::None;
        } else {
            expect_kind_ = Expect::Return;
            expect_ = calls_.back();
            calls_.pop_back();
        }
    } else {
        expect_ = e.end_addr; // a waypoint that is not a branch (ISB, ...)
    }
}

void CoverageSink::finish(uint64_t total_bytes)
{
    // The last segment has no successor whose start could confirm where it
    // ended, so it is not trusted (when there is more than one segment).
    if (health_.segments > 0)
        open_.dirty = true;
    close_open(total_bytes > open_.start ? total_bytes : open_.start);
    while (!closed_.empty()) {
        commit(closed_.front());
        closed_.pop_front();
    }
}

namespace {
    void put_json_string(FILE* f, const std::string& s)
    {
        std::fputc('"', f);
        for (unsigned char c : s) {
            if (c == '"' || c == '\\')
                std::fprintf(f, "\\%c", c);
            else if (c < 0x20)
                std::fprintf(f, "\\u%04x", c);
            else
                std::fputc(c, f);
        }
        std::fputc('"', f);
    }
} // namespace

bool CoverageSink::write_json(const std::string& path, const std::string& elf_path) const
{
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    const CoverageHealth& h = health_;
    std::fprintf(f, "{\"format\": \"cortrace-objcov\", \"version\": 1, \"elf\": ");
    put_json_string(f, elf_path);
    std::fprintf(f, ", \"commit_lag\": %d,\n\"health\": {", lag_);
    const std::pair<const char*, uint64_t> fields[] = {
        { "segments", h.segments },
        { "committed", h.committed },
        { "quarantined", h.quarantined },
        { "quarantined_bytes", h.quarantined_bytes },
        { "ranges", h.ranges },
        { "ranges_committed", h.ranges_committed },
        { "lost_sync", h.lost_sync },
        { "addr_nacc", h.addr_nacc },
        { "trace_on", h.trace_on },
        { "overflow", h.overflow },
        { "outside_code", h.outside_code },
        { "count_mismatch", h.count_mismatch },
        { "kind_mismatch", h.kind_mismatch },
        { "cond_mismatch", h.cond_mismatch },
        { "flow_mismatch", h.flow_mismatch },
        { "flow_checked", h.flow_checked },
        { "return_mismatch", h.return_mismatch },
        { "return_checked", h.return_checked },
        { "exception_return_elsewhere", h.exception_return_elsewhere },
        { "cond_outcomes", h.cond_outcomes },
    };
    bool first = true;
    for (const auto& kv : fields) {
        std::fprintf(f, "%s\"%s\": %llu", first ? "" : ", ", kv.first,
            static_cast<unsigned long long>(kv.second));
        first = false;
    }
    // insns: [addr, size, flags, hits, segments]; flags = kind | cond << 3 | waypoint << 4
    std::fprintf(f, "},\n\"insns\": [");
    const auto& v = map_.insns();
    for (std::size_t i = 0; i < v.size(); ++i) {
        const unsigned flags
            = static_cast<unsigned>(v[i].kind) | (v[i].cond ? 8u : 0u) | (v[i].waypoint ? 16u : 0u);
        std::fprintf(f, "%s[%u, %u, %u, %u, %u]", i ? ",\n" : "\n", v[i].addr, v[i].size, flags,
            hits_[i], segs_[i]);
    }
    // branches: [addr, taken, not_taken, taken segments, not-taken segments]
    // for every conditional waypoint
    std::fprintf(f, "],\n\"branches\": [");
    first = true;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (!(v[i].waypoint && v[i].cond))
            continue;
        std::fprintf(f, "%s[%u, %u, %u, %u, %u]", first ? "\n" : ",\n", v[i].addr, taken_[i],
            not_taken_[i], taken_segs_[i], not_taken_segs_[i]);
        first = false;
    }
    std::fprintf(f, "],\n\"indirect\": [");
    first = true;
    for (const auto& kv : indirect_) {
        std::fprintf(f, "%s[%u, [", first ? "\n" : ",\n", kv.first);
        bool ft = true;
        for (uint32_t t : kv.second) {
            std::fprintf(f, "%s%u", ft ? "" : ", ", t);
            ft = false;
        }
        std::fprintf(f, "]]");
        first = false;
    }
    std::fprintf(f, "]}\n");
    return std::fclose(f) == 0;
}

} // namespace cortrace
