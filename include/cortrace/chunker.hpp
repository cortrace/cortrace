// Cortrace — cut an endless ETM byte stream into chunks that decode alone.
//
// The ETM stream is re-synchronised by an A-sync packet every few KB, and a
// decoder started at one needs nothing that came before. So the stream can be
// cut at A-syncs and the pieces decoded on different threads. A chunk is
//
//     [ commit region | overlap ]
//
// The commit region is what the chunk owns. The overlap is the next few
// segments, copied from the following chunk, which the coverage sink decodes
// only to learn whether the end of the commit region was followed by a loss
// (see CoverageSink::set_commit_limit). Together the commit regions of all
// chunks are the whole stream, each byte exactly once.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_CHUNKER_HPP
#define CORTRACE_CHUNKER_HPP

#include "cortrace/coverage.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace cortrace {

struct Chunk {
    uint64_t seq = 0; // chunk number, from 0
    std::vector<uint8_t> data; // starts with an A-sync; commit region + overlap
    uint64_t commit_end = 0; // data[0, commit_end) is this chunk's; the rest is overlap
    // The stream does not continue after `data` (frames were lost): the end of
    // the data cannot be trusted, with or without overlap.
    bool tail_untrusted = false;
};

class Chunker {
public:
    using Emit = std::function<void(Chunk&&)>;

    // `target_bytes`: minimum size of a commit region. `overlap_segments`: how
    // many complete segments follow it (the commit lag + 1 of the sink).
    Chunker(std::size_t target_bytes, int overlap_segments, Emit emit);

    // Appends deframed ETM bytes of the continuous stream, in pieces of any
    // size. Calls emit for every chunk that is complete.
    void feed(const uint8_t* data, std::size_t len);

    // The stream broke here (UDP frames lost). What is held is emitted as a
    // chunk with tail_untrusted; bytes are dropped until the next A-sync.
    void discontinuity();

    // End of the stream: emits what is held, without overlap.
    void finish();

    // Bytes thrown away: before the first A-sync, after a discontinuity, or
    // because no A-sync came for too long.
    uint64_t dropped_bytes() const { return dropped_; }
    uint64_t chunks() const { return next_seq_; }
    // Bytes waiting for the next cut.
    std::size_t held_bytes() const { return buf_.size(); }

private:
    void restart();
    void cut();

    std::size_t target_;
    std::size_t overlap_;
    std::size_t max_hold_;
    Emit emit_;
    AsyncScanner scanner_;
    std::vector<uint8_t> buf_; // from the start of the chunk in progress
    std::vector<uint64_t> syncs_; // A-sync offsets in buf_ (syncs_[0] == 0 once started)
    std::vector<uint64_t> found_;
    uint64_t abs_base_ = 0; // stream offset of buf_[0]
    uint64_t abs_end_ = 0; // stream offset after the last byte fed
    bool started_ = false; // buf_ begins at an A-sync
    uint64_t next_seq_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace cortrace

#endif // CORTRACE_CHUNKER_HPP
