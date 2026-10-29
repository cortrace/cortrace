// cortrace-cov — code coverage from a live ETM trace, for captures of hours.
//
//   UDP (or a raw file)  ->  deframe  ->  chunks at A-syncs  ->  N decoder threads
//                                                                  |
//   objcov.json  <-  checkpoint every few minutes  <-  one coverage in memory
//
// The raw trace is never written to disk: at 75 MB/s that is 270 GB an hour.
// What reaches the disk is the objcov file (about 1 MB), rewritten under a
// temporary name and renamed at every checkpoint and at the end. `cortrace
// cover stream` runs this and turns the file into a report.
//
// Usage:
//   cortrace-cov --elf fw.elf --out objcov.json --iface eth0 [--secs 3600]
//   cortrace-cov --elf fw.elf --out objcov.json --raw-in capture.bin   (replay)
//
// SIGINT / SIGTERM end the capture cleanly; SIGUSR1 writes a checkpoint now.
//
// SPDX-License-Identifier: MIT
#include "cortrace/cov_engine.hpp"
#include "cortrace/cov_pipeline.hpp"
#include "cortrace/log.hpp"
#include "cortrace/seq_tracker.hpp"
#include <argparse/argparse.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef CORTRACE_VERSION
#define CORTRACE_VERSION "unknown"
#endif

using namespace cortrace;
using Clock = std::chrono::steady_clock;

namespace {

volatile std::sig_atomic_t g_stop = 0;
volatile std::sig_atomic_t g_checkpoint = 0;

void on_stop(int)
{
    g_stop = 1;
    std::signal(SIGINT, SIG_DFL); // a second Ctrl-C ends it at once
    std::signal(SIGTERM, SIG_DFL);
}
void on_usr1(int) { g_checkpoint = 1; }

// ---- raw blocks from the input to the pipeline ------------------------------
struct Gap {
    std::size_t at; // offset in the block's data where frames are missing
    uint64_t lost; // frames missing there
};
struct Block {
    std::vector<uint8_t> data;
    std::vector<Gap> gaps;
};

class BlockQueue {
public:
    BlockQueue(std::size_t max_blocks, bool wait)
        : max_(max_blocks)
        , wait_(wait)
    {
    }
    // False if the block was not taken (queue full and not waiting, or closed).
    bool push(Block&& b)
    {
        std::unique_lock<std::mutex> lock(m_);
        if (wait_)
            space_.wait(lock, [this] { return closed_ || q_.size() < max_; });
        if (closed_ || q_.size() >= max_)
            return false;
        q_.push_back(std::move(b));
        cv_.notify_one();
        return true;
    }
    // False when closed and empty.
    bool pop(Block& out)
    {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return closed_ || !q_.empty(); });
        if (q_.empty())
            return false;
        out = std::move(q_.front());
        q_.pop_front();
        space_.notify_one();
        return true;
    }
    void close()
    {
        std::lock_guard<std::mutex> lock(m_);
        closed_ = true;
        cv_.notify_all();
        space_.notify_all();
    }
    std::size_t size()
    {
        std::lock_guard<std::mutex> lock(m_);
        return q_.size();
    }

private:
    std::mutex m_;
    std::condition_variable cv_, space_;
    std::deque<Block> q_;
    std::size_t max_;
    bool wait_;
    bool closed_ = false;
};

struct Counters {
    std::atomic<uint64_t> rx_bytes { 0 }; // raw payload bytes received
    std::atomic<uint64_t> rx_packets { 0 };
    std::atomic<uint64_t> rx_gaps { 0 };
    std::atomic<uint64_t> rx_lost_frames { 0 };
    std::atomic<uint64_t> rx_dropped_blocks { 0 };
    std::atomic<uint64_t> reordered { 0 };
    std::atomic<uint64_t> seq_restarts { 0 };
    std::atomic<uint64_t> etm_bytes { 0 };
    std::atomic<uint64_t> checkpoints { 0 };
    std::atomic<bool> input_done { false };
    std::atomic<bool> pipeline_done { false };
    std::atomic<bool> failed { false };
    std::string failure;
};

constexpr std::size_t kBlockBytes = 256 * 1024;
constexpr std::size_t kPayloadFrame = 1024; // FPGA payload per UDP frame

// ---- input: UDP -------------------------------------------------------------
// One datagram: [4-byte big-endian sequence number][payload].
void udp_input(int fd, BlockQueue& q, Counters& c, double secs)
{
    constexpr int kBatch = 512;
    constexpr std::size_t kPktMax = 2048;
    std::vector<mmsghdr> msgs(kBatch);
    std::vector<iovec> iov(kBatch);
    std::vector<uint8_t> bufs(static_cast<std::size_t>(kBatch) * kPktMax);
    std::memset(msgs.data(), 0, sizeof(mmsghdr) * kBatch);
    for (int i = 0; i < kBatch; ++i) {
        iov[i].iov_base = bufs.data() + static_cast<std::size_t>(i) * kPktMax;
        iov[i].iov_len = kPktMax;
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    Block cur;
    cur.data.reserve(kBlockBytes + kPktMax);
    SeqTracker seq_tracker;
    uint64_t pending_lost = 0; // frames lost in blocks we had to drop
    const auto end = Clock::now()
        + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(secs > 0 ? secs : 1e9));

    auto flush = [&] {
        if (cur.data.empty() && cur.gaps.empty())
            return;
        const std::size_t frames = cur.data.size() / kPayloadFrame + 1;
        if (!q.push(std::move(cur))) {
            // The pipeline is behind. Say so as a gap in what follows.
            ++c.rx_dropped_blocks;
            pending_lost += frames;
        }
        cur = Block();
        cur.data.reserve(kBlockBytes + kPktMax);
        if (pending_lost) {
            cur.gaps.push_back({ 0, pending_lost });
            c.rx_gaps += 1;
            c.rx_lost_frames += pending_lost;
            pending_lost = 0;
        }
    };

    while (!g_stop && Clock::now() < end) {
        timespec to { 0, 200 * 1000 * 1000 };
        const int n = recvmmsg(fd, msgs.data(), kBatch, MSG_WAITFORONE, &to);
        if (n <= 0) {
            flush(); // quiet moment: hand over what we have
            continue;
        }
        for (int i = 0; i < n; ++i) {
            const uint8_t* p = static_cast<const uint8_t*>(iov[i].iov_base);
            const std::size_t len = msgs[i].msg_len;
            if (len < 4)
                continue;
            const uint32_t seq = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
                | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            ++c.rx_packets;
            switch (seq_tracker.next(seq)) {
            case SeqTracker::Verdict::Skip: // a repeat, or a late frame
                ++c.reordered;
                continue;
            case SeqTracker::Verdict::TakeAfterGap:
                cur.gaps.push_back({ cur.data.size(), seq_tracker.lost() });
                ++c.rx_gaps;
                c.rx_lost_frames += seq_tracker.lost();
                if (seq_tracker.restarted()) {
                    ++c.seq_restarts;
                    std::fprintf(stderr,
                        "[cortrace-cov] warning: the FPGA's frame numbers started over (now %u): "
                        "the streamer was restarted or stalled; following the new numbers\n",
                        seq);
                }
                break;
            case SeqTracker::Verdict::Take:
                break;
            }
            cur.data.insert(cur.data.end(), p + 4, p + len);
            c.rx_bytes += len - 4;
        }
        if (cur.data.size() >= kBlockBytes)
            flush();
    }
    flush();
}

// ---- input: a raw file (replay) ---------------------------------------------
void file_input(const std::string& path, BlockQueue& q, Counters& c)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        c.failure = "cannot open " + path;
        c.failed = true;
        return;
    }
    for (;;) {
        Block b;
        b.data.resize(1 << 20);
        const std::size_t n = std::fread(b.data.data(), 1, b.data.size(), f);
        if (n == 0)
            break;
        b.data.resize(n);
        c.rx_bytes += n;
        if (!q.push(std::move(b)) || g_stop)
            break;
    }
    std::fclose(f);
}

// ---- the pipeline thread ----------------------------------------------------
constexpr std::size_t kPhasePrefix = 8u << 20;

void pipeline_thread(BlockQueue& q, CoverageEngine& eng, Counters& c, bool have_phase,
    DeframePhase phase, int want_stream, int width, std::size_t chunk_bytes, int lag, bool lossless)
{
    std::unique_ptr<CovPipeline> pipe;
    std::vector<uint8_t> pre; // raw bytes held while the phase is looked for
    auto feed_block = [&](const Block& b) {
        std::size_t at = 0;
        for (const Gap& g : b.gaps) {
            pipe->feed_raw(b.data.data() + at, g.at - at);
            at = g.at;
            pipe->gap(g.lost);
        }
        pipe->feed_raw(b.data.data() + at, b.data.size() - at);
        c.etm_bytes = pipe->etm_bytes();
    };
    auto make_pipe = [&] {
        pipe = std::make_unique<CovPipeline>(eng, phase, want_stream, chunk_bytes, lag, lossless);
    };
    if (have_phase)
        make_pipe();

    int attempts = 0;
    Block b;
    while (q.pop(b)) {
        if (pipe) {
            feed_block(b);
            continue;
        }
        // Looking for the wiring: the phase that gives A-syncs.
        for (const Gap& g : b.gaps)
            eng.note_udp_gap(g.lost);
        pre.insert(pre.end(), b.data.begin(), b.data.end());
        if (pre.size() < kPhasePrefix)
            continue;
        if (find_deframe_phase(pre.data(), pre.size(), want_stream, width, phase)) {
            std::fprintf(stderr,
                "[cortrace-cov] deframe phase (parity=%d, order=%d) from %zu MiB\n", phase.parity,
                phase.order, pre.size() >> 20);
            make_pipe();
            std::vector<uint8_t> held;
            held.swap(pre);
            pipe->feed_raw(held.data(), held.size());
            c.etm_bytes = pipe->etm_bytes();
        } else {
            pre.clear();
            if (++attempts >= 5) {
                c.failure = "no ETM A-sync in " + std::to_string(attempts * (kPhasePrefix >> 20))
                    + " MiB of trace: is the ETM armed (cortrace arm) and is --trace-width right?";
                c.failed = true;
                g_stop = 1;
                break;
            }
        }
    }
    if (!pipe && !pre.empty() && !c.failed) { // a stream shorter than the search window
        if (find_deframe_phase(pre.data(), pre.size(), want_stream, width, phase)) {
            std::fprintf(stderr,
                "[cortrace-cov] deframe phase (parity=%d, order=%d) from %zu KiB\n", phase.parity,
                phase.order, pre.size() >> 10);
            make_pipe();
            pipe->feed_raw(pre.data(), pre.size());
            c.etm_bytes = pipe->etm_bytes();
        } else {
            c.failure = "no ETM A-sync in the " + std::to_string(pre.size() >> 10)
                + " KiB received: is the ETM armed and is --trace-width right?";
            c.failed = true;
        }
    }
    if (pipe) {
        pipe->finish();
        c.etm_bytes = pipe->etm_bytes();
    }
    // drain, so that a failed run does not leave the input blocked
    while (q.pop(b)) { }
}

std::string clock_str(double s)
{
    const long t = static_cast<long>(s);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    return buf;
}

long rss_mb()
{
    long pages = 0, dummy = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &dummy, &pages) != 2)
            pages = 0;
        std::fclose(f);
    }
    return pages * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
}

int open_udp(const std::string& iface, const std::string& ip, int port, int rcvbuf_mb)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    int rb = rcvbuf_mb * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    socklen_t len = sizeof rb;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, &len);
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface.c_str(), iface.size()) < 0) {
        std::perror("SO_BINDTODEVICE");
        close(fd);
        return -1;
    }
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
        std::fprintf(stderr, "error: bad --bind-ip %s\n", ip.c_str());
        close(fd);
        return -1;
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        std::perror("bind");
        close(fd);
        return -1;
    }
    std::fprintf(stderr, "[cortrace-cov] udp %s:%d on %s, SO_RCVBUF %d MB\n", ip.c_str(), port,
        iface.c_str(), rb / (1024 * 1024));
    // The kernel caps the request at net.core.rmem_max (and reports twice what it
    // allows). Anything much below what a second of trace needs loses frames
    // whenever this process is held up for a moment.
    if (rb < (rcvbuf_mb * 1024 * 1024) / 2 && rb < 64 * 1024 * 1024)
        std::fprintf(stderr,
            "[cortrace-cov] warning: the kernel gave a %d MB receive buffer (asked for %d MB): "
            "that is %.2f s of trace at 75 MB/s and frames are lost when this process is held "
            "up longer. Raise it with: sudo sysctl -w net.core.rmem_max=%d\n",
            rb / (1024 * 1024), rcvbuf_mb, double(rb) / 75e6, rcvbuf_mb * 1024 * 1024);
    return fd;
}

} // namespace

int main(int argc, char** argv)
{
    argparse::ArgumentParser program("cortrace-cov", CORTRACE_VERSION);
    program.add_description(
        "Code coverage from a live ETM trace, for captures of hours. The raw "
        "trace is never written to disk; the coverage is checkpointed to --out.");
    program.add_argument("--elf").required().help("firmware ELF (the code the trace ran)");
    program.add_argument("--out")
        .required()
        .metavar("objcov.json")
        .help("coverage file, rewritten at every checkpoint and at the end");
    program.add_argument("--iface").metavar("NIC").help(
        "capture from the FPGA streamer on this NIC");
    program.add_argument("--raw-in")
        .metavar("FILE")
        .help("replay a raw capture file instead (nothing is dropped)");
    program.add_argument("--bind-ip")
        .default_value(std::string("192.168.10.245"))
        .help("address the FPGA streams to");
    program.add_argument("--port").scan<'i', int>().default_value(5555).help("UDP port");
    program.add_argument("--rcvbuf-mb")
        .scan<'i', int>()
        .default_value(256)
        .help("kernel receive buffer (the kernel drops frames silently when it overflows)");
    program.add_argument("--secs").scan<'g', double>().default_value(0.0).help(
        "stop after this many seconds (0: until SIGINT/SIGTERM)");
    program.add_argument("--workers")
        .scan<'i', int>()
        .default_value(0)
        .help("decoder threads (0: cores - 4, at most 16)");
    program.add_argument("--cov-lag")
        .scan<'i', int>()
        .default_value(8)
        .help("segments kept back before they count, as for cortrace-decode");
    program.add_argument("--chunk-kb")
        .scan<'i', int>()
        .default_value(1024)
        .help("ETM bytes per chunk (a chunk is decoded by one thread)");
    program.add_argument("--queue-chunks")
        .scan<'i', int>()
        .default_value(128)
        .help("chunks that may wait for a decoder before the next one is dropped");
    program.add_argument("--checkpoint-secs")
        .scan<'g', double>()
        .default_value(300.0)
        .help("write --out this often (0: only at the end)");
    program.add_argument("--status-secs")
        .scan<'g', double>()
        .default_value(10.0)
        .help("print a status line this often (0: never)");
    program.add_argument("--phase").metavar("P,O").help(
        "deframe phase, e.g. 1,0 (default: found on the first 8 MiB)");
    program.add_argument("--trace-width")
        .scan<'i', int>()
        .default_value(4)
        .help("TPIU port width; only 4 is supported here");
    program.add_argument("--want-stream")
        .scan<'i', int>()
        .default_value(2)
        .help("ATB id of the ETM");
    const EtmV4Config etm_defaults;
    const struct {
        const char* name;
        uint32_t EtmV4Config::*field;
    } etm_opts[] = {
        { "--etm-idr0", &EtmV4Config::idr0 },
        { "--etm-idr1", &EtmV4Config::idr1 },
        { "--etm-idr2", &EtmV4Config::idr2 },
        { "--etm-idr8", &EtmV4Config::idr8 },
        { "--etm-idr12", &EtmV4Config::idr12 },
        { "--etm-configr", &EtmV4Config::configr },
        { "--etm-trace-id", &EtmV4Config::traceidr },
    };
    for (const auto& o : etm_opts)
        program.add_argument(o.name)
            .metavar("VAL")
            .scan<'i', long long>()
            .default_value(static_cast<long long>(etm_defaults.*(o.field)))
            .help("TRCIDRn / TRCCONFIGR / TRCTRACEIDR of the target's ETMv4");
    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        std::cerr << program;
        return 2;
    }

    const std::string elf_path = program.get("--elf");
    const std::string out_path = program.get("--out");
    const auto iface = program.present("--iface");
    const auto raw_in = program.present("--raw-in");
    if (!iface == !raw_in) {
        std::fprintf(stderr, "error: give exactly one of --iface and --raw-in\n");
        return 2;
    }
    const int width = program.get<int>("--trace-width");
    if (width != 4) {
        std::fprintf(stderr, "error: cortrace-cov supports the 4-bit trace port only\n");
        return 2;
    }
    const int want_stream = program.get<int>("--want-stream");
    const int lag = std::max(0, program.get<int>("--cov-lag"));
    const std::size_t chunk_bytes
        = static_cast<std::size_t>(std::max(16, program.get<int>("--chunk-kb"))) * 1024;
    int workers = program.get<int>("--workers");
    if (workers <= 0) {
        const int hw = static_cast<int>(std::thread::hardware_concurrency());
        workers = std::max(1, std::min(16, hw > 8 ? hw - 4 : hw - 1));
    }
    DeframePhase phase;
    phase.width = width;
    bool have_phase = false;
    if (auto p = program.present("--phase")) {
        char* end = nullptr;
        phase.parity = static_cast<int>(std::strtol(p->c_str(), &end, 10));
        phase.order
            = (end && *end == ',') ? static_cast<int>(std::strtol(end + 1, nullptr, 10)) : 0;
        have_phase = true;
    }

    // The program: the code map (the denominator) and the bytes the decoders read.
    std::string err;
    auto elf = ElfMemory::load(elf_path, err);
    if (!elf) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    auto idec = make_opencsd_insn_decoder();
    if (!idec) {
        std::fprintf(stderr,
            "error: needs OpenCSD's instruction decoder headers (build with the "
            "vendored OpenCSD)\n");
        return 1;
    }
    CodeMap code_map;
    if (!CodeMap::from_elf(elf_path, *idec, code_map, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "[cortrace-cov] %zu instructions in the code map; %d decoder threads\n",
        code_map.size(), workers);

    CoverageEngine::Config cfg;
    cfg.workers = workers;
    cfg.lag = lag;
    cfg.queue_chunks = static_cast<std::size_t>(std::max(1, program.get<int>("--queue-chunks")));
    for (const auto& o : etm_opts)
        cfg.etm.*(o.field) = static_cast<uint32_t>(program.get<long long>(o.name));
    CoverageEngine engine(code_map, elf, cfg);
    if (!engine.start(err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    int fd = -1;
    if (iface) {
        fd = open_udp(*iface, program.get("--bind-ip"), program.get<int>("--port"),
            program.get<int>("--rcvbuf-mb"));
        if (fd < 0)
            return 1;
    }
    std::signal(SIGINT, on_stop);
    std::signal(SIGTERM, on_stop);
    std::signal(SIGUSR1, on_usr1);

    const bool lossless = static_cast<bool>(raw_in); // a file can be held back, UDP cannot
    BlockQueue queue(lossless ? 16 : 256, lossless);
    Counters counters;
    const double secs = program.get<double>("--secs");
    std::thread input([&] {
        if (raw_in)
            file_input(*raw_in, queue, counters);
        else
            udp_input(fd, queue, counters, secs);
        counters.input_done = true;
        queue.close();
    });
    std::thread pipeline([&] {
        pipeline_thread(queue, engine, counters, have_phase, phase, want_stream, width, chunk_bytes,
            lag, lossless);
        counters.pipeline_done = true;
    });

    // ---- main thread: status, checkpoints, the end -------------------------
    const double status_every = program.get<double>("--status-secs");
    const double ckpt_every = program.get<double>("--checkpoint-secs");
    const auto t0 = Clock::now();
    auto last_status = t0, last_ckpt = t0;
    uint64_t last_rx = 0, last_etm = 0;
    bool warned_silent = false;
    auto checkpoint = [&](const char* why) {
        if (engine.write_json(out_path, elf_path)) {
            ++counters.checkpoints;
            std::fprintf(stderr, "[cortrace-cov] checkpoint (%s) -> %s\n", why, out_path.c_str());
        } else {
            std::fprintf(stderr, "[cortrace-cov] warning: cannot write %s\n", out_path.c_str());
        }
    };
    while (!counters.pipeline_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = Clock::now();
        const double since = std::chrono::duration<double>(now - t0).count();
        if (g_checkpoint) {
            g_checkpoint = 0;
            checkpoint("SIGUSR1");
            last_ckpt = now;
        }
        if (ckpt_every > 0
            && std::chrono::duration<double>(now - last_ckpt).count() >= ckpt_every) {
            checkpoint("periodic");
            last_ckpt = now;
        }
        if (iface && !warned_silent && since > 5 && counters.rx_packets == 0) {
            std::fprintf(stderr,
                "[cortrace-cov] warning: no UDP packets after 5 s: is the FPGA "
                "streaming and is the NIC / address right?\n");
            warned_silent = true;
        }
        if (status_every > 0
            && std::chrono::duration<double>(now - last_status).count() >= status_every) {
            const double dt = std::chrono::duration<double>(now - last_status).count();
            const auto st = engine.status();
            const CoverageHealth h = engine.health();
            const uint64_t rx = counters.rx_bytes, etm = counters.etm_bytes;
            std::fprintf(stderr,
                "[cortrace-cov] %s rx %.1f MB/s etm %.1f MB/s | chunks %llu done %llu dropped, "
                "queue %zu, busy %d/%d | segments %llu quarantined %llu | insns hit %llu | "
                "udp gaps %llu (lost frames %llu) | rss %ld MB\n",
                clock_str(since).c_str(), double(rx - last_rx) / dt / 1e6,
                double(etm - last_etm) / dt / 1e6, (unsigned long long)st.chunks_done,
                (unsigned long long)st.chunks_dropped, st.queued, st.busy, st.workers,
                (unsigned long long)h.segments, (unsigned long long)h.quarantined,
                (unsigned long long)st.instructions_hit, (unsigned long long)h.udp_gaps,
                (unsigned long long)h.udp_lost_frames, rss_mb());
            last_status = now;
            last_rx = rx;
            last_etm = etm;
        }
    }
    input.join();
    pipeline.join();
    engine.finish();
    if (fd >= 0)
        close(fd);

    const CoverageHealth h = engine.health();
    const auto st = engine.status();
    std::fprintf(stderr,
        "\n=== cortrace-cov result ===\n"
        "  duration            : %s\n"
        "  raw received        : %llu B in %llu packets (%llu reordered/duplicate skipped, %llu "
        "sequence restarts)\n"
        "  etm decoded         : %llu B in %llu chunks (%llu dropped)\n"
        "  segments            : %llu (committed %llu, quarantined %llu = %llu bytes)\n"
        "  losses              : udp gaps %llu (%llu frames), dropped chunks %llu, dropped bytes "
        "%llu, "
        "decode fatal %llu, lost-sync %llu, addr-nacc %llu, trace-on %llu, overflow %llu\n"
        "  static cross-check  : outside=%llu count=%llu kind=%llu cond=%llu flow=%llu "
        "return=%llu\n"
        "  instructions hit    : %llu / %zu\n",
        clock_str(std::chrono::duration<double>(Clock::now() - t0).count()).c_str(),
        (unsigned long long)counters.rx_bytes, (unsigned long long)counters.rx_packets,
        (unsigned long long)counters.reordered, (unsigned long long)counters.seq_restarts,
        (unsigned long long)counters.etm_bytes, (unsigned long long)st.chunks_done,
        (unsigned long long)st.chunks_dropped, (unsigned long long)h.segments,
        (unsigned long long)h.committed, (unsigned long long)h.quarantined,
        (unsigned long long)h.quarantined_bytes, (unsigned long long)h.udp_gaps,
        (unsigned long long)h.udp_lost_frames, (unsigned long long)h.dropped_chunks,
        (unsigned long long)h.dropped_bytes, (unsigned long long)h.decode_fatal,
        (unsigned long long)h.lost_sync, (unsigned long long)h.addr_nacc,
        (unsigned long long)h.trace_on, (unsigned long long)h.overflow,
        (unsigned long long)h.outside_code, (unsigned long long)h.count_mismatch,
        (unsigned long long)h.kind_mismatch, (unsigned long long)h.cond_mismatch,
        (unsigned long long)h.flow_mismatch, (unsigned long long)h.return_mismatch,
        (unsigned long long)st.instructions_hit, code_map.size());
    if (counters.failed) {
        std::fprintf(stderr, "error: %s\n", counters.failure.c_str());
        return 3;
    }
    if (!engine.write_json(out_path, elf_path)) {
        std::fprintf(stderr, "error: cannot write %s\n", out_path.c_str());
        return 1;
    }
    std::fprintf(stderr, "wrote object coverage -> %s\n", out_path.c_str());
    return 0;
}
