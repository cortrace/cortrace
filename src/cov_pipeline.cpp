// Cortrace — CovPipeline implementation (see cov_pipeline.hpp).
//
// SPDX-License-Identifier: MIT
#include "cortrace/cov_pipeline.hpp"

namespace cortrace {

bool find_deframe_phase(
    const uint8_t* raw, std::size_t len, int want_stream, int width, DeframePhase& phase)
{
    DeframePhase start;
    start.width = width;
    const DeframeResult r = deframe_raw_capture(raw, len, want_stream, /*search=*/true, start);
    if (r.async_count <= 0)
        return false;
    phase = r.phase;
    return true;
}

CovPipeline::CovPipeline(CoverageEngine& engine, const DeframePhase& phase, int want_stream,
    std::size_t chunk_bytes, int lag, bool wait)
    : engine_(engine)
    , wait_(wait)
    , deframer_(phase, want_stream)
    , chunker_(chunk_bytes, lag + 1, [this](Chunk&& c) { engine_.submit(std::move(c), wait_); })
{
}

void CovPipeline::account_dropped()
{
    const uint64_t d = chunker_.dropped_bytes();
    if (d > dropped_reported_) {
        engine_.note_dropped_bytes(d - dropped_reported_);
        dropped_reported_ = d;
    }
}

void CovPipeline::feed_raw(const uint8_t* raw, std::size_t len)
{
    raw_bytes_ += len;
    etm_.clear();
    deframer_.feed(raw, len, etm_);
    etm_bytes_ += etm_.size();
    chunker_.feed(etm_.data(), etm_.size());
    account_dropped();
}

void CovPipeline::gap(uint64_t lost_frames)
{
    engine_.note_udp_gap(lost_frames);
    chunker_.discontinuity(); // what is held is cut off from what follows
    deframer_.reset(); // and the TPIU framing is gone until the next sync
    account_dropped();
}

void CovPipeline::finish()
{
    chunker_.finish();
    account_dropped();
}

} // namespace cortrace
