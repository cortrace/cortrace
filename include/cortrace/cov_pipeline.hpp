// Cortrace — raw FPGA bytes in, chunks for the coverage engine out.
//
// This is the middle of cortrace-cov, without the input and the threads: the
// raw capture bytes (the payloads of the UDP packets, in order) are deframed
// incrementally, cut into chunks at A-syncs and handed to a CoverageEngine.
// Frames lost on the way are reported with gap().
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_COV_PIPELINE_HPP
#define CORTRACE_COV_PIPELINE_HPP

#include "cortrace/chunker.hpp"
#include "cortrace/cov_engine.hpp"
#include "cortrace/deframe.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cortrace {

// The deframe phase is a property of the wiring. Find it on a stretch of raw
// capture: the phase under which the most ETMv4 A-syncs come out. False if
// there is none (the target is not tracing, or the port width is wrong).
bool find_deframe_phase(
    const uint8_t* raw, std::size_t len, int want_stream, int width, DeframePhase& phase);

class CovPipeline {
public:
    // `wait`: when the engine's queue is full, wait instead of dropping the chunk
    // (for a file, which can be held back; a live stream cannot).
    CovPipeline(CoverageEngine& engine, const DeframePhase& phase, int want_stream,
        std::size_t chunk_bytes, int lag, bool wait);

    // The next raw bytes of the stream, in pieces of any size.
    void feed_raw(const uint8_t* raw, std::size_t len);

    // `lost_frames` UDP frames are missing between the bytes fed so far and the
    // next ones.
    void gap(uint64_t lost_frames);

    // End of the stream.
    void finish();

    uint64_t raw_bytes() const { return raw_bytes_; }
    uint64_t etm_bytes() const { return etm_bytes_; }
    uint64_t chunks() const { return chunker_.chunks(); }

private:
    void account_dropped();

    CoverageEngine& engine_;
    bool wait_;
    StreamDeframer deframer_;
    Chunker chunker_;
    std::vector<uint8_t> etm_; // scratch: the deframed bytes of one feed
    uint64_t raw_bytes_ = 0;
    uint64_t etm_bytes_ = 0;
    uint64_t dropped_reported_ = 0;
};

} // namespace cortrace

#endif // CORTRACE_COV_PIPELINE_HPP
