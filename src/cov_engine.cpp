// Cortrace — CoverageEngine implementation (see cov_engine.hpp).
//
// SPDX-License-Identifier: MIT
#include "cortrace/cov_engine.hpp"

#include <algorithm>

namespace cortrace {

CoverageEngine::CoverageEngine(const CodeMap& map, std::shared_ptr<const ElfMemory> elf, Config cfg)
    : map_(map)
    , elf_(std::move(elf))
    , cfg_(std::move(cfg))
    , acc_(map, cfg_.lag)
{
    cfg_.workers = std::max(1, cfg_.workers);
    cfg_.queue_chunks = std::max<std::size_t>(1, cfg_.queue_chunks);
}

CoverageEngine::~CoverageEngine() { finish(); }

bool CoverageEngine::start(std::string& err)
{
    if (started_)
        return true;
    if (!cfg_.make_decoder) {
#if defined(CORTRACE_HAVE_OPENCSD)
        if (!elf_) {
            err = "no program image for the decoders";
            return false;
        }
        // One error logger serves every OpenCSD decoder in the process and is
        // not thread-safe; damaged trace makes decoders write to it all the time.
        opencsd_silence_errors();
        cfg_.make_decoder = [etm = cfg_.etm, elf = elf_]() -> std::unique_ptr<IDecoder> {
            auto d = make_opencsd_decoder(etm);
            if (d && !d->add_elf_memory(elf))
                d.reset();
            return d;
        };
#else
        err = "built without OpenCSD: no decoder available";
        return false;
#endif
    }
    started_ = true;
    for (int i = 0; i < cfg_.workers; ++i)
        threads_.emplace_back([this] { worker(); });
    return true;
}

bool CoverageEngine::submit(Chunk&& chunk, bool wait)
{
    {
        std::unique_lock<std::mutex> lock(q_mutex_);
        if (wait)
            space_cv_.wait(lock, [this] { return stopping_ || queue_.size() < cfg_.queue_chunks; });
        if (!stopping_ && queue_.size() < cfg_.queue_chunks) {
            queue_.push_back(std::move(chunk));
            q_cv_.notify_one();
            return true;
        }
    }
    CoverageHealth lost;
    lost.dropped_chunks = 1;
    lost.dropped_bytes = chunk.commit_end;
    {
        std::lock_guard<std::mutex> lock(acc_mutex_);
        acc_.add_health(lost);
    }
    ++chunks_dropped_;
    return false;
}

void CoverageEngine::note_udp_gap(uint64_t lost_frames)
{
    CoverageHealth h;
    h.udp_gaps = 1;
    h.udp_lost_frames = lost_frames;
    std::lock_guard<std::mutex> lock(acc_mutex_);
    acc_.add_health(h);
}

void CoverageEngine::note_dropped_bytes(uint64_t bytes)
{
    CoverageHealth h;
    h.dropped_bytes = bytes;
    std::lock_guard<std::mutex> lock(acc_mutex_);
    acc_.add_health(h);
}

void CoverageEngine::finish()
{
    {
        std::lock_guard<std::mutex> lock(q_mutex_);
        stopping_ = true;
    }
    q_cv_.notify_all();
    space_cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();
}

void CoverageEngine::worker()
{
    for (;;) {
        Chunk c;
        {
            std::unique_lock<std::mutex> lock(q_mutex_);
            q_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty())
                return; // stopping, and nothing left
            c = std::move(queue_.front());
            queue_.pop_front();
            ++busy_;
            space_cv_.notify_one();
        }
        try {
            process(c);
        } catch (...) {
            CoverageHealth h;
            h.decode_fatal = 1;
            std::lock_guard<std::mutex> lock(acc_mutex_);
            acc_.add_health(h);
        }
        {
            std::lock_guard<std::mutex> lock(q_mutex_);
            --busy_;
        }
        ++chunks_done_;
        bytes_done_ += c.commit_end;
    }
}

void CoverageEngine::process(const Chunk& c)
{
    CoverageSink sink(map_, cfg_.lag);
    // Everything after commit_end is the overlap that follows the chunk.
    if (c.commit_end < c.data.size())
        sink.set_commit_limit(c.commit_end);
    std::vector<uint64_t> starts;
    AsyncScanner().scan(c.data.data(), c.data.size(), 0, starts);
    for (const uint64_t s : starts)
        sink.add_segment_start(s);

    bool fatal = false;
    std::unique_ptr<IDecoder> dec = cfg_.make_decoder();
    if (!dec) {
        fatal = true;
    } else {
        dec->set_sink([&sink](const Element& e) { sink.on_element(e); });
        fatal = !dec->process(c.data.data(), c.data.size());
        dec->flush();
    }
    // Where the decoder gave up, or where frames were lost after the data, the
    // end of what was decoded cannot be trusted.
    if (fatal || c.tail_untrusted)
        sink.mark_discontinuity();
    sink.finish(c.data.size());

    CoverageHealth extra;
    extra.decode_fatal = fatal ? 1 : 0;
    std::lock_guard<std::mutex> lock(acc_mutex_);
    acc_.merge(sink);
    acc_.add_health(extra);
}

CoverageEngine::Status CoverageEngine::status() const
{
    Status s;
    s.chunks_done = chunks_done_;
    s.chunks_dropped = chunks_dropped_;
    s.bytes_done = bytes_done_;
    s.workers = cfg_.workers;
    {
        std::lock_guard<std::mutex> lock(q_mutex_);
        s.queued = queue_.size();
        s.busy = busy_;
    }
    std::lock_guard<std::mutex> lock(acc_mutex_);
    s.instructions_hit = acc_.instructions_hit();
    return s;
}

CoverageHealth CoverageEngine::health() const
{
    std::lock_guard<std::mutex> lock(acc_mutex_);
    return acc_.health();
}

bool CoverageEngine::write_json(const std::string& path, const std::string& elf_path) const
{
    std::lock_guard<std::mutex> lock(acc_mutex_);
    return acc_.write_json(path, elf_path);
}

} // namespace cortrace
