// Cortrace — Chunker implementation (see chunker.hpp).
//
// SPDX-License-Identifier: MIT
#include "cortrace/chunker.hpp"

#include <algorithm>

namespace cortrace {

namespace {
    // An A-sync is eleven 0x00 bytes then 0x80. While no A-sync has been seen,
    // this many trailing bytes are kept in case the zeros are the start of one.
    constexpr std::size_t kAsyncLead = 11;
    // Without an A-sync for this many chunk sizes the stream is not usable.
    constexpr std::size_t kMaxHoldChunks = 64;
} // namespace

Chunker::Chunker(std::size_t target_bytes, int overlap_segments, Emit emit)
    : target_(target_bytes ? target_bytes : 1)
    , overlap_(overlap_segments < 0 ? 0 : static_cast<std::size_t>(overlap_segments))
    , max_hold_(target_ * kMaxHoldChunks)
    , emit_(std::move(emit))
{
}

void Chunker::restart()
{
    scanner_ = AsyncScanner();
    buf_.clear();
    syncs_.clear();
    started_ = false;
    abs_base_ = abs_end_;
}

void Chunker::feed(const uint8_t* data, std::size_t len)
{
    if (len == 0)
        return;
    const uint64_t first_new = abs_end_;
    buf_.insert(buf_.end(), data, data + len);
    abs_end_ += len;

    found_.clear();
    scanner_.scan(data, len, first_new, found_);
    for (const uint64_t at : found_) {
        if (!started_) {
            // The stream starts here: what came before cannot be decoded.
            const uint64_t skip = at - abs_base_;
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(skip));
            dropped_ += skip;
            abs_base_ = at;
            started_ = true;
            syncs_.assign(1, 0);
        } else {
            syncs_.push_back(at - abs_base_);
        }
    }

    if (!started_) {
        if (buf_.size() > kAsyncLead) {
            const std::size_t skip = buf_.size() - kAsyncLead;
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(skip));
            dropped_ += skip;
            abs_base_ += skip;
        }
        return;
    }
    cut();
    if (buf_.size() > max_hold_) {
        dropped_ += buf_.size();
        restart();
    }
}

// Emits every chunk whose commit region is long enough and which is followed
// by the whole overlap.
void Chunker::cut()
{
    for (;;) {
        // first A-sync at or after the target size
        const std::size_t j = static_cast<std::size_t>(
            std::lower_bound(syncs_.begin(), syncs_.end(), static_cast<uint64_t>(target_))
            - syncs_.begin());
        if (j == 0 || j + overlap_ >= syncs_.size())
            return;
        Chunk c;
        c.seq = next_seq_++;
        c.commit_end = syncs_[j];
        c.data.assign(
            buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(syncs_[j + overlap_]));
        emit_(std::move(c));

        const uint64_t cut_at = syncs_[j];
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(cut_at));
        abs_base_ += cut_at;
        syncs_.erase(syncs_.begin(), syncs_.begin() + static_cast<std::ptrdiff_t>(j));
        for (auto& s : syncs_)
            s -= cut_at;
    }
}

void Chunker::discontinuity()
{
    if (started_ && !buf_.empty()) {
        Chunk c;
        c.seq = next_seq_++;
        c.commit_end = buf_.size();
        c.data = std::move(buf_);
        c.tail_untrusted = true;
        emit_(std::move(c));
    }
    restart();
}

void Chunker::finish()
{
    if (started_ && !buf_.empty()) {
        Chunk c;
        c.seq = next_seq_++;
        c.commit_end = buf_.size();
        c.data = std::move(buf_);
        emit_(std::move(c));
    }
    restart();
}

} // namespace cortrace
