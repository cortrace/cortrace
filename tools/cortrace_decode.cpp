// cortrace-decode — offline CLI front end.
//
// Runs the full pipeline on a captured, already-deframed ETMv4 byte stream:
//   raw ETM bytes  ->  OpenCSD decode  ->  call-stack machine  ->  Perfetto
//
// Reports the golden quality criteria (nesting balance, per-function slice
// counts, call edges for ELF cross-check), a function-coverage report (which
// functions the instruction flow visited vs which rendered as slices, so
// "missed" functions are surfaced), and the blind-region rate. Writes a
// Perfetto trace that opens directly in ui.perfetto.dev.
//
// Usage:
//   cortrace-decode <etm.bin> <mem.bin> <mem_base_hex> <syms.nm>
//                   [--time time.bin] [--perf out.perftrace] [--edges out.tsv]
//
// SPDX-License-Identifier: MIT
#include "cortrace/callstack.hpp"
#include "cortrace/coverage.hpp"
#include "cortrace/decoder.hpp"
#include "cortrace/deframe.hpp"
#include "cortrace/log.hpp"
#include "cortrace/nxtrace.hpp"
#include "cortrace/perfetto_writer.hpp"
#include "cortrace/symbols.hpp"
#include "cortrace/timebase.hpp"

#include <argparse/argparse.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Set by CMake from the VERSION file (cmake/Version.cmake).
#ifndef CORTRACE_VERSION
#define CORTRACE_VERSION "unknown"
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace cortrace;

namespace {

// Tracks which functions the instruction flow actually visited, and the
// blind-region (no-coverage) time, straight off the element stream.
struct Coverage {
    const SymbolTable& syms;
    const TimeBase& tb;

    // fn -> # of instr ranges landing in it. Keyed by the symbol table's own
    // name string (function_at returns a stable reference), so the per-range hot
    // path is one pointer-hash bump instead of two string-keyed tree lookups;
    // ranges_by_fn() folds same-named symbols together for the report.
    std::unordered_map<const std::string*, long> range_counts;

    bool blind_pending = false;
    bool have_first = false;
    uint64_t first_ns = 0, last_cov_ns = 0, final_ns = 0, blind_ns = 0;
    long blind_regions = 0;

    explicit Coverage(const SymbolTable& s, const TimeBase& t)
        : syms(s)
        , tb(t)
    {
    }

    std::map<std::string, long> ranges_by_fn() const
    {
        std::map<std::string, long> out;
        for (const auto& kv : range_counts)
            out[*kv.first] += kv.second;
        return out;
    }

    void observe(const Element& e)
    {
        switch (e.kind) {
        case ElementKind::InstrRange: {
            range_counts[&syms.function_at(e.start_addr)]++;
            if (!tb.empty()) {
                const uint64_t now = tb.ns_for(e.byte_index);
                if (!have_first) {
                    first_ns = now;
                    have_first = true;
                }
                if (blind_pending && now > last_cov_ns) {
                    blind_ns += now - last_cov_ns;
                    blind_regions++;
                }
                blind_pending = false;
                last_cov_ns = now;
                final_ns = now;
            }
            break;
        }
        case ElementKind::AddrNacc:
            nacc_count++;
            blind_pending = true;
            break;
        case ElementKind::TraceOn:
            traceon_count++;
            blind_pending = true;
            break;
        case ElementKind::NoSync:
            // NO_SYNC means "waiting for sync": either the ONE normal startup
            // sync before the first instruction, or a mid-stream resync AFTER an
            // ETM/ETF overflow. Only the latter is data loss, so split them:
            // the initial run of NO_SYNC (before any InstrRange) is startup;
            // anything after we've decoded instructions is a real loss.
            if (range_counts.empty())
                startup_sync_count++;
            else
                nosync_count++;
            blind_pending = true;
            break;
        case ElementKind::Overflow:
            overflow_count++;
            blind_pending = true;
            break;
        case ElementKind::Benign:
            benign_count++; // PE_CONTEXT / SYNC_MARKER / EO_TRACE: normal, not loss
            break;
        case ElementKind::OtherUnknown:
            other_count++;
            break;
        case ElementKind::Timestamp:
            ts_count++;
            if (!have_first_ts) {
                first_ts = e.timestamp;
                have_first_ts = true;
            }
            last_ts = e.timestamp;
            break;
        default:
            break;
        }
        if (e.has_cc) {
            cc_count++;
            cc_total += e.cycle_count;
        }
    }

    long ts_count = 0;
    // Stream-health counters (never silently dropped): NO_SYNC is the overflow/
    // corrupt-stream symptom; TraceOn is a resync; AddrNacc a decode blind spot.
    long nosync_count = 0; // mid-stream resync = real overflow/loss
    long startup_sync_count = 0; // initial sync before first instruction (normal)
    long overflow_count = 0;
    long traceon_count = 0;
    long nacc_count = 0;
    long benign_count = 0; // modeled-but-inert (PE_CONTEXT/SYNC_MARKER/EO_TRACE)
    long other_count = 0;
    bool have_first_ts = false;
    uint64_t first_ts = 0, last_ts = 0;
    long cc_count = 0; // elements carrying a cycle count
    uint64_t cc_total = 0; // summed CPU cycles
};

// Minimal read-only view of an ELF's PT_LOAD segments, for the NuttX resolver's
// read_u32 (a static TCB's pid/entry come from the ELF image, never live
// memory). Only what nxtrace needs: little-endian u32 at a virtual/load address
// if some PT_LOAD segment's [p_paddr, p_paddr+p_filesz) covers it. Bytes beyond
// p_filesz (NOBITS/.bss tail) are not backed -> read fails (resolver then falls
// back to a pointer-named lane).
class ElfImage {
public:
    bool load(const std::string& path)
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0) {
            std::fclose(f);
            return false;
        }
        buf_.resize(static_cast<std::size_t>(sz));
        std::size_t got = std::fread(buf_.data(), 1, buf_.size(), f);
        std::fclose(f);
        if (got != buf_.size())
            return false;
        return parse();
    }

    // Read a little-endian u32 at load address `addr` from a covering PT_LOAD.
    bool read_u32(uint32_t addr, uint32_t& out) const
    {
        for (const auto& s : segs_) {
            if (addr >= s.paddr && addr + 4 <= s.paddr + s.filesz) {
                const uint8_t* p = &buf_[s.offset + (addr - s.paddr)];
                out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
                    | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
                return true;
            }
        }
        return false;
    }

private:
    struct Seg {
        uint32_t paddr, offset, filesz;
    };

    uint32_t rd32(std::size_t o) const
    {
        return static_cast<uint32_t>(buf_[o]) | (static_cast<uint32_t>(buf_[o + 1]) << 8)
            | (static_cast<uint32_t>(buf_[o + 2]) << 16)
            | (static_cast<uint32_t>(buf_[o + 3]) << 24);
    }
    uint16_t rd16(std::size_t o) const
    {
        return static_cast<uint16_t>(buf_[o]) | (static_cast<uint16_t>(buf_[o + 1]) << 8);
    }

    bool parse()
    {
        // ELF32 little-endian only (Cortex-M). Header: e_phoff@0x1C,
        // e_phentsize@0x2A, e_phnum@0x2C. Program header (PT_LOAD=1):
        // p_type@0, p_offset@4, p_paddr@0xC, p_filesz@0x10.
        if (buf_.size() < 0x34 || buf_[0] != 0x7F || buf_[1] != 'E' || buf_[2] != 'L'
            || buf_[3] != 'F' || buf_[4] != 1 /*ELFCLASS32*/)
            return false;
        const uint32_t phoff = rd32(0x1C);
        const uint16_t phentsize = rd16(0x2A);
        const uint16_t phnum = rd16(0x2C);
        for (uint16_t i = 0; i < phnum; ++i) {
            const std::size_t ph = phoff + static_cast<std::size_t>(i) * phentsize;
            if (ph + 0x14 > buf_.size())
                break;
            if (rd32(ph) != 1 /*PT_LOAD*/)
                continue;
            Seg s;
            s.offset = rd32(ph + 0x04);
            s.paddr = rd32(ph + 0x0C);
            s.filesz = rd32(ph + 0x10);
            if (s.offset + s.filesz <= buf_.size())
                segs_.push_back(s);
        }
        return !segs_.empty();
    }

    std::vector<uint8_t> buf_;
    std::vector<Seg> segs_;
};

} // namespace

int main(int argc, char** argv)
{
    argparse::ArgumentParser program("cortrace-decode", CORTRACE_VERSION);
    program.add_description(
        "Decode a captured ETMv4 stream: raw ETM bytes -> OpenCSD -> call-stack "
        "-> Perfetto, with quality metrics and a function-coverage report.");

    program.add_argument("etm").help("input ETM bytes, or a raw FPGA capture with --raw");
    program.add_argument("syms").help("ELF symbol table (arm-none-eabi-nm output)");
    // Program memory: EITHER --elf (preferred, always consistent) OR the legacy
    // flat-binary pair. The flat binary is error-prone (objcopy -O binary
    // mis-lays-out .data/gapped images), so --elf is recommended.
    program.add_argument("--elf").metavar("fw.elf").help(
        "program ELF; memory is read per PT_LOAD segment (preferred over mem/mem_base)");
    program.add_argument("mem")
        .metavar("mem.bin")
        .default_value(std::string())
        .help("flat memory image of the ELF (legacy; prefer --elf)");
    program.add_argument("mem_base")
        .metavar("mem_base_hex")
        .default_value(std::string())
        .help("load address (hex) of mem's first byte, e.g. 08000000 (legacy)");

    program.add_argument("--time")
        .metavar("time.bin")
        .help("FPGA ETF-egress ns-per-ETM-byte time base");
    program.add_argument("--etm-time")
        .flag()
        .help("use the ETM global timestamp (execution time) as the Perfetto time base");
    program.add_argument("--tsgen-hz")
        .metavar("HZ")
        .scan<'g', double>()
        .default_value(0.0)
        .help("TSGEN frequency for --etm-time count->ns (0 = raw counts as ticks)");
    program.add_argument("--hybrid-time")
        .flag()
        .help("wall-clock ETM timestamps (needs --tsgen-hz) interpolated by the CPU cycle count "
              "(needs --sysclk-hz): TSGEN accuracy over sleeps, cycle resolution in between");
    program.add_argument("--cycle-time")
        .flag()
        .help("use the ETM cycle-count clock (CPU-cycle resolution) as the time base");
    program.add_argument("--sysclk-hz")
        .metavar("HZ")
        .scan<'g', double>()
        .default_value(0.0)
        .help("CPU clock for --cycle-time cycles->ns (0 = raw cycles as ticks)");
    program.add_argument("--perf").metavar("out.perftrace").help("write a Perfetto trace");
    program.add_argument("--edges").metavar("out.tsv").help("write call edges for ELF cross-check");
    program.add_argument("--events")
        .metavar("out.log")
        .help("plain +B/-E event log for structural verify");
    program.add_argument("--memory-limit-mb")
        .metavar("N")
        .scan<'i', long>()
        .default_value<long>(4096)
        .help("cap virtual memory (0 disables); guards a libopencsd allocation blowup");
    program.add_argument("--strict").flag().help("exit 1 if ANY loss/patch heuristic fired");
    program.add_argument("--raw").flag().help(
        "<etm> is a raw FPGA capture; deframe it first (nibble reassemble + TPIU stream 2)");
    program.add_argument("--dump-etm")
        .metavar("out.bin")
        .help("with --raw, also write the deframed ETM bytes");
    program.add_argument("--phase").metavar("P,O").help(
        "lock the deframe phase (skip the search), e.g. 1,0 for the A7-Lite default");
    program.add_argument("--trace-width")
        .metavar("W")
        .scan<'i', int>()
        .default_value(4)
        .help("parallel TRACED port width: 4 (default), 2 or 1. Non-4 uses the "
              "width-generic reassembly (8/W TRACECLK periods per TPIU byte).");
    program.add_argument("--want-stream")
        .metavar("ID")
        .scan<'i', int>()
        .default_value(2)
        .help("TPIU ATB stream to deframe (default 2 = ETM; 1 = DWT/ITM packets)");
    // ETMv4 trace-unit description. The defaults describe a Cortex-M7 ETMv4;
    // for another core give the values read from its TRCIDRn / TRCCONFIGR.
    const EtmV4Config etm_defaults;
    const struct {
        const char* name;
        const char* what;
        uint32_t EtmV4Config::*field;
    } etm_opts[] = {
        { "--etm-idr0", "TRCIDR0", &EtmV4Config::idr0 },
        { "--etm-idr1", "TRCIDR1", &EtmV4Config::idr1 },
        { "--etm-idr2", "TRCIDR2", &EtmV4Config::idr2 },
        { "--etm-idr8", "TRCIDR8", &EtmV4Config::idr8 },
        { "--etm-idr12", "TRCIDR12", &EtmV4Config::idr12 },
        { "--etm-configr", "TRCCONFIGR", &EtmV4Config::configr },
        { "--etm-trace-id", "TRCTRACEIDR (ATB id; keep --want-stream in step)",
            &EtmV4Config::traceidr },
    };
    for (const auto& o : etm_opts) {
        char def[16];
        std::snprintf(def, sizeof def, "0x%08x", etm_defaults.*(o.field));
        program.add_argument(o.name)
            .metavar("VAL")
            .scan<'i', long long>()
            .default_value(static_cast<long long>(etm_defaults.*(o.field)))
            .help(std::string(o.what) + " of the target's ETMv4 (default " + def + ")");
    }
    // RTOS thread-switch overlay from the DWT data-value stream. The decoder
    // knows nothing about the OS: it sees writes of a "current task" pointer and
    // names the pointers through a thread map prepared by an OS plugin.
    program.add_argument("--switch-stream", "--nx-switch-stream")
        .metavar("ID")
        .scan<'i', int>()
        .default_value(-1)
        .help("enable the thread overlay: TPIU stream carrying DWT current-task writes "
              "(e.g. 1). Adds a 'Threads' track to the Perfetto output alongside ETM. "
              "--nx-switch-stream is the old name.");
    program.add_argument("--watch-comp", "--nx-comp")
        .metavar("N")
        .scan<'i', int>()
        .default_value(0)
        .help("DWT comparator id that watches the current-task pointer (default 0)");
    program.add_argument("--thread-map")
        .metavar("FILE")
        .help("pointer -> thread map (lines '0xPTR<TAB>tid<TAB>name'); pointers it does "
              "not list are shown as raw values. Replaces the built-in NuttX lookup.");
    program.add_argument("--dump-switch-values")
        .metavar("FILE")
        .help("write the distinct values seen on the watched comparator, one 0xPTR per "
              "line, and exit without decoding (needs --raw and --switch-stream). Feed "
              "the file to an OS plugin to build --thread-map.");
    program.add_argument("--nx-tcb-pid-off")
        .metavar("OFF")
        .scan<'i', int>()
        .default_value(0x30)
        .help("deprecated, use --thread-map: byte offset of pid in struct tcb_s");
    program.add_argument("--nx-tcb-entry-off")
        .metavar("OFF")
        .scan<'i', int>()
        .default_value(0x3C)
        .help("deprecated, use --thread-map: byte offset of entry in struct tcb_s");
    program.add_argument("--nx-tcbmap")
        .metavar("FILE")
        .help("deprecated, use --thread-map: tcb->name map for heap TCBs not in the "
              "ELF image");
    program.add_argument("--runs-out", "--nx-runs-out")
        .metavar("runs.tsv")
        .help("write the thread runs on the final time base as TSV "
              "(begin_tick, name); small, for checking thread switches against "
              "another trace without parsing the whole Perfetto file. "
              "--nx-runs-out is the old name.");
    program.add_argument("--itm-note-port")
        .metavar("N")
        .scan<'i', int>()
        .default_value(1)
        .help("ITM stimulus port carrying software trace bytes (NuttX notes) in the "
              "--switch-stream stream (default 1; matches CONFIG_ARMV7M_NOTE_ITM_PORT)");
    program.add_argument("--itm-note-unwrap")
        .flag()
        .help("extend the 32-bit DWT_CYCCNT note timestamps to 64 bits across counter wraps "
              "(every ~28.6 s at 150 MHz) before writing --itm-note-out");
    program.add_argument("--itm-note-out")
        .metavar("notes.bin")
        .help("write the ITM stimulus-port byte stream (the NuttX note stream) here; "
              "requires --raw and --switch-stream. Feed it to `nxtrace ... file`.");
    program.add_argument("--cov-out")
        .metavar("objcov.json")
        .help("write object-code coverage (every instruction of the ELF's Thumb code with "
              "its execution count, conditional branch outcomes, indirect targets and the "
              "trace health) for `cortrace cover`; needs --elf. Only clean trace "
              "segments between A-syncs are counted.");
    program.add_argument("--cov-lag")
        .metavar("N")
        .scan<'i', int>()
        .default_value(8)
        .help("with --cov-out: segments kept back before they count, so that a loss or a "
              "cross-check failure can still drop them (default 8, about 8 KB of trace: a "
              "wrong address can decode as a plausible path for a while before it shows)");
    program.add_argument("--log-level")
        .metavar("LVL")
        .default_value(std::string("warn"))
        .help("diagnostic log level: error|warn|info|debug|trace (default warn)");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::fprintf(stderr, "%s\n", err.what());
        std::cerr << program;
        return 2;
    }

    cortrace::log::set_level_from_str(program.get("--log-level"));

    const std::string etm_path = program.get("etm");
    const std::string syms_path = program.get("syms");
    const std::string elf_path = program.is_used("--elf") ? program.get("--elf") : std::string();
    const std::string mem_path = program.get("mem");
    const uint32_t mem_base = mem_path.empty()
        ? 0u
        : static_cast<uint32_t>(std::strtoul(program.get("mem_base").c_str(), nullptr, 16));

    if (elf_path.empty() && mem_path.empty()) {
        std::fprintf(stderr, "error: provide --elf <fw.elf> (preferred) or mem.bin + mem_base\n");
        std::cerr << program;
        return 2;
    }

    const auto time_opt = program.present("--time");
    const auto perf_opt = program.present("--perf");
    const auto edges_opt = program.present("--edges");
    const auto events_opt = program.present("--events");
    const char* time_path = time_opt ? time_opt->c_str() : nullptr;
    const char* perf_path = perf_opt ? perf_opt->c_str() : nullptr;
    const bool etm_time = program.get<bool>("--etm-time");
    const double tsgen_hz = program.get<double>("--tsgen-hz");
    const bool hybrid_time = program.get<bool>("--hybrid-time");
    const bool cycle_time = program.get<bool>("--cycle-time");
    const double sysclk_hz = program.get<double>("--sysclk-hz");
    const char* edges_path = edges_opt ? edges_opt->c_str() : nullptr;
    const char* events_path = events_opt ? events_opt->c_str() : nullptr;

    // Self-cap virtual memory. libopencsd 1.8.3 has a pathological allocation
    // on some (currently uncharacterised) input patterns above ~1.9 MB of ETM
    // -- observed on-board with S2 selftrace captures: RSS jumps from ~40 MB
    // to 22+ GB across a small input-size increase (2026-09-07 investigation).
    // Once malloc fails, cortrace treats it as a normal fatal decode response
    // and gracefully returns partial-but-balanced results.
    const long mem_limit_mb = program.get<long>("--memory-limit-mb");
    // --strict: any nonzero loss/patch metric => exit non-zero. This is the
    // mode you use when you must prove "no silent loss / no silent heuristic
    // patching" -- e.g. the deterministic self-trace verification loop.
    const bool strict = program.get<bool>("--strict");

    const auto dump_opt = program.present("--dump-etm");
    const char* dump_etm_path = dump_opt ? dump_opt->c_str() : nullptr;

    // --phase P,O locks the deframe phase and implies --raw. Defaults to the
    // A7-Lite board (parity=1, order=0).
    DeframePhase phase;
    phase.width = program.get<int>("--trace-width");
    bool phase_locked = false;
    if (auto phase_opt = program.present("--phase")) {
        const std::string& pv = *phase_opt;
        char* end = nullptr;
        phase.parity = static_cast<int>(std::strtol(pv.c_str(), &end, 10));
        phase.order
            = (end && *end == ',') ? static_cast<int>(std::strtol(end + 1, nullptr, 10)) : 0;
        phase_locked = true;
    }
    const bool raw_input = program.get<bool>("--raw") || phase_locked;
    const int want_stream = program.get<int>("--want-stream");
    const int nx_switch_stream = program.get<int>("--switch-stream");
    const bool nx_enabled = nx_switch_stream >= 0;
    const uint8_t nx_comp = static_cast<uint8_t>(program.get<int>("--watch-comp"));
    const uint32_t nx_pid_off = static_cast<uint32_t>(program.get<int>("--nx-tcb-pid-off"));
    const uint32_t nx_entry_off = static_cast<uint32_t>(program.get<int>("--nx-tcb-entry-off"));

    // Apply the memory cap before we allocate anything decoder-related.
    if (mem_limit_mb > 0) {
#if defined(__unix__) || defined(__APPLE__)
        struct rlimit rl;
        rl.rlim_cur = static_cast<rlim_t>(mem_limit_mb) * 1024ULL * 1024ULL;
        rl.rlim_max = rl.rlim_cur;
        if (setrlimit(RLIMIT_AS, &rl) != 0) {
            std::fprintf(
                stderr, "warning: could not set memory cap %ld MB (continuing)\n", mem_limit_mb);
        } else {
            std::fprintf(stderr, "memory cap: %ld MB (RLIMIT_AS)\n", mem_limit_mb);
        }
#endif
    }

    SymbolTable syms;
    try {
        std::size_t n = syms.load_nm(syms_path);
        std::fprintf(stderr, "loaded %zu function symbols\n", n);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    TimeBase tb;
    if (time_path) {
        try {
            std::size_t n = tb.load(time_path);
            std::fprintf(stderr, "loaded %zu ns time-base entries\n", n);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }

    CallStackMachine machine(syms);
    Coverage cov(syms, tb);

    EtmV4Config cfg; // Cortex-M7 ETMv4 defaults, overridable with --etm-*
    for (const auto& o : etm_opts)
        cfg.*(o.field) = static_cast<uint32_t>(program.get<long long>(o.name));
    auto decoder = make_opencsd_decoder(cfg);
    if (!decoder) {
        std::fprintf(stderr, "error: failed to create OpenCSD decoder\n");
        return 1;
    }
    const bool mem_ok = elf_path.empty() ? decoder->add_memory_image(mem_base, mem_path)
                                         : decoder->add_elf(elf_path);
    if (!mem_ok) {
        if (elf_path.empty())
            std::fprintf(stderr, "error: failed to add memory image %s @ 0x%08x\n",
                mem_path.c_str(), mem_base);
        else
            std::fprintf(stderr, "error: failed to add ELF %s\n", elf_path.c_str());
        return 1;
    }

    // --cov-out: the static code map of the ELF and the coverage sink.
    const auto cov_opt = program.present("--cov-out");
    CodeMap code_map;
    std::unique_ptr<CoverageSink> cov_sink;
    AsyncScanner async_scanner;
    std::vector<uint64_t> async_offsets;
    if (cov_opt) {
        if (elf_path.empty()) {
            std::fprintf(stderr, "error: --cov-out needs --elf\n");
            return 2;
        }
        auto idec = make_opencsd_insn_decoder();
        if (!idec) {
            std::fprintf(stderr,
                "error: --cov-out needs OpenCSD's instruction decoder headers (build with the "
                "vendored OpenCSD submodule)\n");
            return 1;
        }
        std::string err;
        if (!CodeMap::from_elf(elf_path, *idec, code_map, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "coverage: %zu instructions in the code map\n", code_map.size());
        cov_sink = std::make_unique<CoverageSink>(code_map, program.get<int>("--cov-lag"));
    }
    // Segments for the coverage sink start at the A-syncs of each chunk; they
    // must be announced before the chunk is decoded.
    auto announce_segments = [&](const uint8_t* data, std::size_t n, uint64_t base) {
        if (!cov_sink)
            return;
        async_offsets.clear();
        async_scanner.scan(data, n, base, async_offsets);
        for (uint64_t off : async_offsets)
            cov_sink->add_segment_start(off);
    };

    FILE* f = std::fopen(etm_path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open %s\n", etm_path.c_str());
        return 1;
    }

    std::size_t total = 0;
    bool fatal = false;

    // nxtrace state: the deframed DWT stream + a map from DWT event src_index
    // (into the assembled byte stream) to the ETM byte index the TimeBase uses.
    std::vector<uint8_t> dwt_bytes;
    std::vector<std::size_t> dwt_src;
    std::vector<std::size_t> etm_src; // etm byte i came from assembled src etm_src[i]
    // Thread overlay. The runs are built BEFORE the ETM stream is decoded and
    // handed to the call-stack machine as a schedule, so every thread gets its
    // own call stack; the Threads track and the per-thread lanes are made from
    // the same runs afterwards.
    std::vector<SliceEvent> nx_slices;
    std::map<int, std::string> nx_tracks;
    std::vector<ThreadRun> nx_runs;
    std::size_t nx_event_count = 0;
    std::vector<std::pair<std::size_t, int>> thread_schedule; // (ETM byte index, tid)
    std::size_t next_switch = 0;
    auto build_threads = [&]() {
        if (!nx_enabled)
            return;
        auto dwt_events = parse_dwt_data_values(dwt_bytes, dwt_src);
        // Map assembled src_index -> ETM byte index by binary search in etm_src
        // (monotonic). Rewrite each event's src_index into ETM-byte space so
        // thread_runs_to_slices' byte_index lines up with the ETM TimeBase.
        auto to_etm_index = [&](std::size_t asm_src) -> std::size_t {
            if (etm_src.empty())
                return asm_src;
            auto it = std::lower_bound(etm_src.begin(), etm_src.end(), asm_src);
            return static_cast<std::size_t>(it - etm_src.begin());
        };
        for (auto& e : dwt_events)
            e.src_index = to_etm_index(e.src_index);
        ThreadResolver resolver;
        ElfImage img;
        const int nx_track_id = 1000; // high id so it never collides with ISR tracks
        if (auto map_opt = program.present("--thread-map")) {
            auto m = load_tcb_map(*map_opt);
            std::fprintf(stderr, "threads: loaded %zu thread map entries from %s\n", m.size(),
                map_opt->c_str());
            resolver = make_map_resolver(std::move(m));
        } else if (!elf_path.empty() && img.load(elf_path)) {
            auto reader = [img](uint32_t addr, uint32_t& out) { return img.read_u32(addr, out); };
            auto nx = std::make_shared<NuttxResolver>(reader, syms);
            nx->set_offsets(nx_pid_off, nx_entry_off);
            if (auto tcbmap_opt = program.present("--nx-tcbmap")) {
                auto m = load_tcb_map(*tcbmap_opt);
                std::fprintf(stderr, "nxtrace: loaded %zu TCB map entries from %s\n", m.size(),
                    tcbmap_opt->c_str());
                nx->set_tcb_map(std::move(m));
            }
            resolver = [nx](uint32_t v) { return (*nx)(v); };
        }
        const std::size_t stream_end = etm_src.empty() ? 0 : etm_src.size();
        nx_event_count = dwt_events.size();
        nx_runs = build_thread_runs(dwt_events, resolver, nx_comp, stream_end);
        nx_slices = thread_runs_to_slices(nx_runs, nx_track_id, nx_tracks);
        for (const auto& r : nx_runs)
            thread_schedule.emplace_back(r.begin_src, r.id.tid);
    };

    decoder->set_sink([&](const Element& e) {
        cov.observe(e);
        if (cov_sink)
            cov_sink->on_element(e);
        // Threads switched in at or before this element's ETM byte run it.
        while (next_switch < thread_schedule.size()
            && thread_schedule[next_switch].first <= e.byte_index) {
            machine.switch_thread(thread_schedule[next_switch].second);
            ++next_switch;
        }
        machine.process(e);
    });
    // --raw: read the whole capture, deframe it in-process (nibble reassemble +
    // TPIU demux), then feed the resulting ETM bytes to the decoder. This
    // replaces the slow host Python deframe_to_etm.py path.
    if (raw_input) {
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> capture(sz > 0 ? static_cast<std::size_t>(sz) : 0);
        std::size_t got = capture.empty() ? 0 : std::fread(capture.data(), 1, capture.size(), f);
        std::fclose(f);
        f = nullptr;

        std::vector<uint8_t> etm_bytes;
        DeframePhase used_phase = phase;
        int async_count = 0;
        std::size_t frames = 0;

        if (nx_enabled) {
            // Multi-stream: demux ETM (want_stream) and the DWT stream in one
            // pass so both share the assembled-byte space (-> one timeline).
            MultiDeframeResult mr
                = deframe_raw_capture_multi(capture.data(), got, /*search=*/!phase_locked, phase);
            used_phase = mr.phase;
            async_count = mr.async_count;
            frames = mr.frames;
            auto eit = mr.streams.find(want_stream);
            if (eit != mr.streams.end()) {
                etm_bytes = eit->second;
                etm_src = mr.src_index[want_stream];
            }
            auto dit = mr.streams.find(nx_switch_stream);
            if (dit != mr.streams.end()) {
                dwt_bytes = dit->second;
                dwt_src = mr.src_index[nx_switch_stream];
            }
            std::fprintf(stderr,
                "deframe(multi): raw=%zu B -> etm[%d]=%zu B  dwt[%d]=%zu B  "
                "phase=(parity=%d,order=%d)  A-syncs=%d  frames=%zu\n",
                got, want_stream, etm_bytes.size(), nx_switch_stream, dwt_bytes.size(),
                used_phase.parity, used_phase.order, async_count, frames);
        } else {
            DeframeResult dr = deframe_raw_capture(
                capture.data(), got, want_stream, /*search=*/!phase_locked, phase);
            etm_bytes = std::move(dr.etm);
            used_phase = dr.phase;
            async_count = dr.async_count;
            frames = dr.frames;
            std::fprintf(stderr,
                "deframe: raw=%zu B -> etm=%zu B  phase=(parity=%d,order=%d)  "
                "A-syncs=%d  frames=%zu\n",
                got, etm_bytes.size(), used_phase.parity, used_phase.order, async_count, frames);
        }

        if (auto dump_out = program.present("--dump-switch-values")) {
            if (!nx_enabled) {
                std::fprintf(stderr, "error: --dump-switch-values needs --switch-stream\n");
                return 2;
            }
            std::set<uint32_t> values;
            for (const auto& e : parse_dwt_data_values(dwt_bytes, dwt_src))
                if (e.comparator == nx_comp)
                    values.insert(e.value);
            FILE* vf = std::fopen(dump_out->c_str(), "w");
            if (!vf) {
                std::fprintf(stderr, "error: cannot write %s\n", dump_out->c_str());
                return 1;
            }
            for (uint32_t v : values)
                std::fprintf(vf, "0x%08x\n", v);
            std::fclose(vf);
            std::fprintf(stderr, "wrote %zu distinct switch values -> %s\n", values.size(),
                dump_out->c_str());
            return 0;
        }
        if (auto note_out = program.present("--itm-note-out")) {
            if (!nx_enabled) {
                std::fprintf(stderr, "error: --itm-note-out needs --switch-stream\n");
                return 2;
            }
            const uint8_t note_port = static_cast<uint8_t>(program.get<int>("--itm-note-port"));
            ItmStats ist;
            auto notes = extract_itm_stimulus(dwt_bytes, note_port, &ist);
            if (program.get<bool>("--itm-note-unwrap")) {
                NoteUnwrapStats ust;
                unwrap_note_systime(notes, &ust);
                std::fprintf(stderr,
                    "itm notes: unwrapped %zu timestamps, %zu counter wrap(s), %zu byte(s) "
                    "skipped\n",
                    ust.notes, ust.wraps, ust.skipped_bytes);
            }
            FILE* nf = std::fopen(note_out->c_str(), "wb");
            if (!nf) {
                std::fprintf(stderr, "error: cannot write %s\n", note_out->c_str());
                return 1;
            }
            std::fwrite(notes.data(), 1, notes.size(), nf);
            std::fclose(nf);
            std::fprintf(stderr,
                "itm notes: port %u -> %zu B in %zu packets (all sw packets %zu, dwt hw %zu, "
                "timestamps %zu, OVERFLOW %zu, other %zu%s) -> %s\n",
                note_port, notes.size(), ist.port_packets, ist.sw_packets, ist.hw_packets,
                ist.timestamps, ist.overflows, ist.other, ist.truncated ? ", TRUNCATED" : "",
                note_out->c_str());
        }

        if (dump_etm_path) {
            FILE* df = std::fopen(dump_etm_path, "wb");
            if (df) {
                std::fwrite(etm_bytes.data(), 1, etm_bytes.size(), df);
                std::fclose(df);
                std::fprintf(stderr, "wrote deframed ETM -> %s\n", dump_etm_path);
            }
        }

        total = etm_bytes.size();
        build_threads();
        announce_segments(etm_bytes.data(), etm_bytes.size(), 0);
        if (!decoder->process(etm_bytes.data(), etm_bytes.size())) {
            std::fprintf(stderr, "error: opencsd fatal while decoding deframed stream\n");
            fatal = true;
        }
    } else {
        build_threads(); // no DWT stream without --raw: nothing to schedule
        std::vector<uint8_t> buf(1 << 16);
        std::size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
            announce_segments(buf.data(), n, total);
            total += n;
            if (!decoder->process(buf.data(), n)) {
                std::fprintf(stderr,
                    "error: opencsd fatal at %zu bytes (byte index into ETM stream). "
                    "This is a hard decode failure on Layer F (libopencsd). Silent recovery "
                    "would mask real data corruption or a decoder bug; use "
                    "--allow-fatal-restart for triage only.\n",
                    total);
                fatal = true;
                break;
            }
        }
        std::fclose(f);
        f = nullptr;
    }
    decoder->flush();
    machine.finish();

    const StackMetrics& m = machine.metrics();
    std::fprintf(stderr, "\n=== cortrace decode result ===\n");
    std::fprintf(
        stderr, "  etm bytes processed : %zu%s\n", total, fatal ? " (stopped: fatal)" : "");
    std::fprintf(stderr, "  begins / ends       : %ld / %ld  (%s)\n", m.begins, m.ends,
        m.balanced() ? "balanced" : "UNBALANCED");
    std::fprintf(stderr, "  max depth           : %d\n", m.max_depth);
    std::fprintf(stderr, "  mismatched returns  : %ld\n", m.mismatched_returns);
    std::fprintf(
        stderr, "  dropped calls       : %ld  (callee lost to blind spot)\n", m.dropped_calls);
    std::fprintf(stderr, "  recovered returns   : %ld\n", m.recovered_missed_returns);
    std::fprintf(stderr, "  exceptions rendered : %ld\n", m.exceptions);
    std::fprintf(stderr, "  slice events        : %zu\n", machine.slices().size());
    std::fprintf(stderr, "  ETM timestamps      : %ld", cov.ts_count);
    if (cov.ts_count > 0) {
        std::fprintf(stderr, "  (0x%llx .. 0x%llx, span %llu counts)",
            static_cast<unsigned long long>(cov.first_ts),
            static_cast<unsigned long long>(cov.last_ts),
            static_cast<unsigned long long>(cov.last_ts - cov.first_ts));
    }
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "  cycle counts        : %ld", cov.cc_count);
    if (cov.cc_count > 0) {
        std::fprintf(
            stderr, "  (%llu CPU cycles total)", static_cast<unsigned long long>(cov.cc_total));
    }
    std::fprintf(stderr, "\n");

    // ---- stream health (overflow / resync / blind spots) -------------------
    // NO_SYNC is the ETM/ETF overflow (or corrupt-stream) symptom; a nonzero
    // count means the trace is LOSSY and the call graph downstream of each loss
    // is unreliable. These are surfaced, never silently dropped.
    const bool lossy = cov.nosync_count > 0 || cov.overflow_count > 0 || m.dropped_calls > 0;
    std::fprintf(stderr,
        "  stream health       : lost-sync=%ld overflow=%ld resync(TraceOn)=%ld "
        "addr-nacc=%ld startup-sync=%ld benign=%ld other=%ld  -> %s\n",
        cov.nosync_count, cov.overflow_count, cov.traceon_count, cov.nacc_count,
        cov.startup_sync_count, cov.benign_count, cov.other_count,
        lossy ? "LOSSY (trace dropped data -- see gotchas: raise TRACECLK/widen port/lower CPU)"
              : "clean");

    // ---- function-coverage report: flow-visited vs slice-rendered ----------
    // A function the instruction flow visited but that never rendered as a
    // begin slice is "missed" on the timeline -- usually because it was only
    // ever entered across a blind spot (dropped call) or is a leaf reached by
    // fall-through/tail-call. Surface these explicitly.
    const auto& begins = machine.slices();
    // Millions of begin events share a few hundred names: hash, don't tree-insert.
    std::unordered_set<std::string> rendered_set;
    for (const auto& s : begins)
        if (s.begin)
            rendered_set.insert(s.name);
    const std::set<std::string> rendered(rendered_set.begin(), rendered_set.end());

    const auto ranges_by_fn = cov.ranges_by_fn();
    std::vector<std::pair<long, std::string>> missed; // (range count, fn)
    for (const auto& kv : ranges_by_fn) {
        const std::string& fn = kv.first;
        if (fn == "?" || fn == "<root>")
            continue;
        if (rendered.find(fn) == rendered.end())
            missed.emplace_back(kv.second, fn);
    }
    std::sort(missed.begin(), missed.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    std::fprintf(stderr, "\n=== function coverage ===\n");
    std::fprintf(stderr, "  functions with instruction flow : %zu\n", ranges_by_fn.size());
    std::fprintf(stderr, "  functions rendered as slices    : %zu\n", rendered.size());
    if (missed.empty()) {
        std::fprintf(stderr, "  MISSED (flow but no slice)      : none\n");
    } else {
        std::fprintf(stderr, "  MISSED (flow but no slice)      : %zu\n", missed.size());
        for (const auto& p : missed)
            std::fprintf(stderr, "      %-28s (%ld instr ranges)\n", p.second.c_str(), p.first);
    }

    // ---- object-code coverage (--cov-out) ----------------------------------
    if (cov_sink) {
        cov_sink->finish(total);
        const CoverageHealth& h = cov_sink->health();
        std::size_t hit = 0;
        for (uint32_t c : cov_sink->hits())
            hit += c != 0;
        std::fprintf(stderr, "\n=== code coverage (clean segments only) ===\n");
        std::fprintf(stderr, "  instructions hit    : %zu / %zu\n", hit, code_map.size());
        std::fprintf(stderr,
            "  segments            : %llu (committed %llu, quarantined %llu = %llu bytes)\n",
            static_cast<unsigned long long>(h.segments),
            static_cast<unsigned long long>(h.committed),
            static_cast<unsigned long long>(h.quarantined),
            static_cast<unsigned long long>(h.quarantined_bytes));
        std::fprintf(stderr,
            "  losses              : lost-sync=%llu addr-nacc=%llu trace-on=%llu "
            "overflow=%llu\n",
            static_cast<unsigned long long>(h.lost_sync),
            static_cast<unsigned long long>(h.addr_nacc),
            static_cast<unsigned long long>(h.trace_on),
            static_cast<unsigned long long>(h.overflow));
        std::fprintf(stderr,
            "  static cross-check  : outside=%llu count=%llu kind=%llu cond=%llu flow=%llu "
            "(flow checked %llu, cond outcomes %llu, ranges %llu)\n",
            static_cast<unsigned long long>(h.outside_code),
            static_cast<unsigned long long>(h.count_mismatch),
            static_cast<unsigned long long>(h.kind_mismatch),
            static_cast<unsigned long long>(h.cond_mismatch),
            static_cast<unsigned long long>(h.flow_mismatch),
            static_cast<unsigned long long>(h.flow_checked),
            static_cast<unsigned long long>(h.cond_outcomes),
            static_cast<unsigned long long>(h.ranges));
        if (!cov_sink->write_json(*cov_opt, elf_path)) {
            std::fprintf(stderr, "error: cannot write coverage to %s\n", cov_opt->c_str());
            return 1;
        }
        std::fprintf(stderr, "wrote object coverage -> %s\n", cov_opt->c_str());
    }

    // ---- blind-region (no-coverage) time rate ------------------------------
    if (!tb.empty() && cov.final_ns > cov.first_ns) {
        const uint64_t span = cov.final_ns - cov.first_ns;
        const double blind_pct
            = 100.0 * static_cast<double>(cov.blind_ns) / static_cast<double>(span);
        std::fprintf(stderr, "\n=== blind-region rate ===\n");
        std::fprintf(stderr, "  trace span : %.3f us\n", span / 1000.0);
        std::fprintf(stderr, "  blind time : %.3f us over %ld regions\n", cov.blind_ns / 1000.0,
            cov.blind_regions);
        std::fprintf(
            stderr, "  BLIND RATE : %.2f%%  (covered %.2f%%)\n", blind_pct, 100.0 - blind_pct);
    }

    // ---- nxtrace: RTOS thread-switch overlay -------------------------------
    // Parse the DWT data-value stream into current-task writes, build thread-run
    // intervals, resolve identities from the ELF image, and emit them as slices
    // on a dedicated 'Threads' track. Each DWT event's assembled-byte src_index
    // is mapped to the ETM byte index (via etm_src) so the Threads track shares
    // the ETM timeline (--time / byte-index base).
    // (The runs were built before the decode, because the call-stack machine
    // needs the switches while it runs: see build_threads.)
    if (nx_enabled) {
        std::fprintf(stderr,
            "\n=== nxtrace ===\n  DWT stream %d: %zu bytes -> %zu switch events, %zu runs\n",
            nx_switch_stream, dwt_bytes.size(), nx_event_count, nx_runs.size());
    }

    // ---- outputs -----------------------------------------------------------
    if (perf_path) {
        std::vector<SliceEvent> timed;
        const char* base_desc;
        if (hybrid_time) {
            timed = apply_hybrid_time(machine.slices(), tsgen_hz, sysclk_hz);
            base_desc = " (hybrid: ETM timestamp + cycle count, ns)";
        } else if (cycle_time) {
            timed = apply_cycle_time(machine.slices(), sysclk_hz);
            base_desc = sysclk_hz > 0.0 ? " (cycle count, ns)" : " (cycle count, raw cycles)";
        } else if (etm_time) {
            timed = apply_etm_timestamp(machine.slices(), tsgen_hz);
            base_desc = tsgen_hz > 0.0 ? " (ETM timestamp, ns)" : " (ETM timestamp, raw counts)";
        } else {
            timed = apply_timebase(machine.slices(), tb);
            base_desc = tb.empty() ? " (tick order; pass --time or --etm-time)" : " (ns time base)";
        }

        std::map<int, std::string> tracks = machine.tracks();
        if (nx_enabled && !nx_slices.empty()) {
            // Re-attribute the ETM call-stack to per-thread tracks: each slice
            // moves to the lane of whichever RTOS thread was running at its
            // byte_index, so every thread shows its OWN call stack instead of
            // all stacks piling onto one "main thread" lane. ISR tracks are
            // left as-is. Per-thread tracks start at 2000 (clear of ISR ids and
            // the Threads summary track at 1000).
            const int nx_thread_base = 2000;
            timed = reattribute_slices_to_threads(timed, nx_runs, nx_thread_base, tracks);

            // Thread slices carry an ETM byte_index but no cycle_clock/etm_ts of
            // their own, so apply_cycle_time/apply_etm_timestamp can't time them
            // directly. Instead ride the SAME execution-time base as the ETM
            // callstack: build a (byte_index -> tick) table from the timed ETM
            // slices and map each thread slice's byte_index through it (nearest
            // preceding anchor). This puts the Threads summary track on ETM
            // execution time, NOT the lagging FPGA ETF-egress time.
            std::vector<std::pair<uint64_t, uint64_t>> anchors; // (byte_index, tick)
            anchors.reserve(timed.size());
            for (const auto& s : timed)
                anchors.emplace_back(s.byte_index, s.tick);
            std::sort(anchors.begin(), anchors.end());

            auto tick_for_byte = [&](uint64_t bidx) -> uint64_t {
                if (anchors.empty())
                    return bidx;
                auto it = std::upper_bound(
                    anchors.begin(), anchors.end(), std::make_pair(bidx, UINT64_MAX));
                if (it == anchors.begin())
                    return anchors.front().second;
                --it;
                return it->second;
            };

            std::vector<SliceEvent> nx_timed = nx_slices;
            for (auto& s : nx_timed)
                s.tick = tick_for_byte(s.byte_index);
            if (auto runs_out = program.present("--runs-out")) {
                if (FILE* rf = std::fopen(runs_out->c_str(), "w")) {
                    for (const auto& s : nx_timed)
                        if (s.begin)
                            std::fprintf(rf, "%llu\t%s\n", static_cast<unsigned long long>(s.tick),
                                s.name.c_str());
                    std::fclose(rf);
                    std::fprintf(stderr, "wrote thread runs -> %s\n", runs_out->c_str());
                }
            }
            // The Threads track goes first in the file: it is the thread switch
            // log of this trace (`<ns> <name> (pid N)` per run), and a reader
            // that wants only that can stop after the first packets instead of
            // scanning millions of call-stack events (nxtrace merge does).
            timed.insert(timed.begin(), nx_timed.begin(), nx_timed.end());
            for (const auto& kv : nx_tracks)
                tracks[kv.first] = kv.second;
        }

        if (!write_perfetto_trace_multi(perf_path, timed, tracks)) {
            std::fprintf(stderr, "error: cannot write perf to %s\n", perf_path);
            return 1;
        }
        std::fprintf(
            stderr, "\nwrote %zu slice events -> %s%s\n", timed.size(), perf_path, base_desc);
    }
    if (edges_path) {
        FILE* ef = std::fopen(edges_path, "w");
        if (!ef) {
            std::fprintf(stderr, "error: cannot write edges to %s\n", edges_path);
            return 1;
        }
        for (const auto& kv : machine.edges())
            std::fprintf(ef, "%ld\t%s\n", kv.second, kv.first.c_str());
        std::fclose(ef);
        std::fprintf(stderr, "wrote %zu call edges -> %s\n", machine.edges().size(), edges_path);
    }
    if (events_path) {
        // Begin/end event log for downstream structural + timing verification.
        // One line per slice event: "<+|->funcname<TAB>cycle_clock<TAB>etm_ts".
        // The cycle_clock column (accumulated CPU cycles) lets a checker verify
        // timing precision, e.g. that each iteration of a deterministic loop is
        // a constant number of cycles apart.
        FILE* ef = std::fopen(events_path, "w");
        if (!ef) {
            std::fprintf(stderr, "error: cannot write events to %s\n", events_path);
            return 1;
        }
        for (const auto& s : machine.slices())
            std::fprintf(ef, "%c%s\t%llu\t%llu\n", s.begin ? '+' : '-', s.name.c_str(),
                static_cast<unsigned long long>(s.cycle_clock),
                static_cast<unsigned long long>(s.etm_ts));
        std::fclose(ef);
        std::fprintf(stderr, "wrote %zu events -> %s\n", machine.slices().size(), events_path);
    }

    // --strict verdict. balanced() is necessary but nowhere near sufficient
    // on a deterministic loop -- mismatched_returns / dropped_calls /
    // recovered_missed_returns are all silent heuristic patches that could
    // hide a real fault. Blind time > 0 also means byte-level loss.
    if (strict) {
        bool clean = !fatal && m.balanced() && m.mismatched_returns == 0 && m.dropped_calls == 0
            && m.recovered_missed_returns == 0 && cov.blind_ns == 0 && cov.blind_regions == 0;
        std::fprintf(stderr, "\n=== --strict verdict ===\n");
        std::fprintf(stderr, "  fatal          : %s\n", fatal ? "YES (FAIL)" : "no");
        std::fprintf(stderr, "  balanced       : %s\n", m.balanced() ? "yes" : "NO (FAIL)");
        std::fprintf(stderr, "  mismatched     : %ld%s\n", m.mismatched_returns,
            m.mismatched_returns ? "  (FAIL: heuristic patched a hole)" : "");
        std::fprintf(stderr, "  dropped calls  : %ld%s\n", m.dropped_calls,
            m.dropped_calls ? "  (FAIL: blind-spot swallowed a call)" : "");
        std::fprintf(stderr, "  recovered      : %ld%s\n", m.recovered_missed_returns,
            m.recovered_missed_returns ? "  (FAIL: return patched)" : "");
        std::fprintf(stderr, "  blind regions  : %ld  (%.3f us)\n", cov.blind_regions,
            cov.blind_ns / 1000.0);
        std::fprintf(stderr, "  verdict        : %s\n",
            clean ? "CLEAN (100% observable, no silent patches)"
                  : "DIRTY (any nonzero count above is silent data loss)");
        return clean ? 0 : 1;
    }
    return (m.balanced() && !fatal) ? 0 : 1;
}
