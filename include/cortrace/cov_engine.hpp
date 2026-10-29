// Cortrace — decode chunks of an ETM stream on many threads into one coverage.
//
// A producer cuts the stream into chunks (chunker.hpp) and submits them. Each
// worker takes a chunk, decodes it with its own decoder into its own
// CoverageSink and adds the result to a shared accumulator. The chunks are
// independent, so nothing but the accumulator is shared.
//
// The producer is never made to wait: if the workers fall behind and the queue
// is full the chunk is dropped and counted (dropped_chunks, a loss). Waiting
// instead would let the kernel drop UDP frames, which is the same loss but with
// no count to show for it.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_COV_ENGINE_HPP
#define CORTRACE_COV_ENGINE_HPP

#include "cortrace/chunker.hpp"
#include "cortrace/codemap.hpp"
#include "cortrace/coverage.hpp"
#include "cortrace/decoder.hpp"
#include "cortrace/elfmem.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cortrace {

class CoverageEngine {
public:
    using DecoderFactory = std::function<std::unique_ptr<IDecoder>()>;

    struct Config {
        int workers = 4;
        int lag = 8; // the sinks' commit lag; the chunker's overlap is lag + 1
        std::size_t queue_chunks = 64; // chunks waiting for a worker
        EtmV4Config etm;
        // One decoder per chunk. Default: OpenCSD reading the ElfMemory. Tests
        // put a fake here.
        DecoderFactory make_decoder;
    };

    struct Status {
        uint64_t chunks_done = 0;
        uint64_t chunks_dropped = 0;
        uint64_t bytes_done = 0; // ETM bytes of the commit regions decoded
        std::size_t queued = 0;
        int busy = 0;
        int workers = 0;
        uint64_t instructions_hit = 0;
    };

    // `map` and `elf` must outlive the engine.
    CoverageEngine(const CodeMap& map, std::shared_ptr<const ElfMemory> elf, Config cfg);
    ~CoverageEngine();
    CoverageEngine(const CoverageEngine&) = delete;
    CoverageEngine& operator=(const CoverageEngine&) = delete;

    // Starts the workers. False (and `err`) if there is no way to make a decoder.
    bool start(std::string& err);

    // Queues a chunk. If the queue is full the chunk is dropped and counted and
    // false is returned, or, with `wait`, the call waits for room (for input
    // that can be held back, such as a file). Thread-safe.
    bool submit(Chunk&& chunk, bool wait = false);

    // Events of the stream the chunks cannot show.
    void note_udp_gap(uint64_t lost_frames);
    void note_dropped_bytes(uint64_t bytes);

    // Decodes what is queued, then stops the workers. Call once the producer is
    // done; later submit() calls are dropped.
    void finish();

    Status status() const;
    CoverageHealth health() const;
    // The coverage so far, as the objcov file `cortrace cover` reads. Safe while
    // workers run; each file is a consistent snapshot.
    bool write_json(const std::string& path, const std::string& elf_path) const;

private:
    void worker();
    void process(const Chunk& c);

    const CodeMap& map_;
    std::shared_ptr<const ElfMemory> elf_;
    Config cfg_;

    mutable std::mutex q_mutex_;
    std::condition_variable q_cv_; // work for the workers
    std::condition_variable space_cv_; // room for the producer
    std::deque<Chunk> queue_;
    bool stopping_ = false;
    int busy_ = 0;
    std::vector<std::thread> threads_;
    bool started_ = false;

    mutable std::mutex acc_mutex_;
    CoverageAccumulator acc_;
    std::atomic<uint64_t> chunks_done_ { 0 };
    std::atomic<uint64_t> chunks_dropped_ { 0 };
    std::atomic<uint64_t> bytes_done_ { 0 };
};

} // namespace cortrace

#endif // CORTRACE_COV_ENGINE_HPP
