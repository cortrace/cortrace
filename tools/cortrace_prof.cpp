// cortrace-prof — cycle-accurate profile from a raw ETM capture.
//
//   raw capture  ->  deframe  ->  OpenCSD  ->  Profiler  ->  profile.json
//
// The capture must have been made with cycle counting on (TRCCONFIGR.CCI:
// `cortrace arm --profile timing`). The file is read in pieces, so its size is
// not limited by memory. `cortrace profile` runs this and turns the file into
// a report.
//
// Usage:
//   cortrace-prof --raw-in capture.bin --elf fw.elf --syms fw.nm --out profile.json
//                 [--sysclk-hz 150e6 --tsgen-hz 75e6]
//
// SPDX-License-Identifier: MIT
#include "cortrace/cov_pipeline.hpp" // find_deframe_phase
#include "cortrace/decoder.hpp"
#include "cortrace/deframe.hpp"
#include "cortrace/dwt_events.hpp"
#include "cortrace/elfmem.hpp"
#include "cortrace/profile.hpp"
#include "cortrace/symbols.hpp"
#include <argparse/argparse.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef CORTRACE_VERSION
#define CORTRACE_VERSION "unknown"
#endif

using namespace cortrace;

namespace {

constexpr std::size_t kBlock = 1u << 20;

bool write_file(const std::string& path, const std::string& body)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace

int main(int argc, char** argv)
{
    argparse::ArgumentParser program("cortrace-prof", CORTRACE_VERSION);
    program.add_description("Cycle-accurate profile from a raw ETM capture made with cycle "
                            "counting on: cycles per function, exception entry and duration, "
                            "lateness of a periodic exception, spread of basic blocks.");
    program.add_argument("--raw-in").required().metavar("FILE").help("raw capture (cortrace-grab)");
    program.add_argument("--elf").required().help("firmware ELF (the code the trace ran)");
    program.add_argument("--syms").required().metavar("FILE").help(
        "`nm -n` of the ELF (function names)");
    program.add_argument("--out").required().metavar("profile.json").help("the profile");
    program.add_argument("--sysclk-hz")
        .scan<'g', double>()
        .default_value(0.0)
        .help("CPU clock; with --tsgen-hz it checks the cycle clock against the timestamps");
    program.add_argument("--tsgen-hz")
        .scan<'g', double>()
        .default_value(0.0)
        .help("ETM timestamp clock");
    program.add_argument("--period-exception")
        .scan<'i', int>()
        .default_value(15)
        .help("exception number whose lateness is measured (15 = SysTick)");
    program.add_argument("--period-cycles")
        .scan<'g', double>()
        .default_value(0.0)
        .help("its period in cycles (0: the median interval)");
    program.add_argument("--top-blocks")
        .scan<'i', int>()
        .default_value(20)
        .help("basic blocks listed");
    program.add_argument("--block-min-count")
        .scan<'i', int>()
        .default_value(1000)
        .help("a block is listed when it ran alone at least this often");
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
    program.add_argument("--itm-stream")
        .scan<'i', int>()
        .default_value(1)
        .help("ATB id of the ITM/DWT, whose event counter packets are placed in the code "
              "(-1: ignore them)");
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

    const int width = program.get<int>("--trace-width");
    if (width != 4) {
        std::fprintf(stderr, "error: cortrace-prof supports the 4-bit trace port only\n");
        return 2;
    }
    const int want_stream = program.get<int>("--want-stream");

    SymbolTable syms;
    try {
        if (syms.load_nm(program.get("--syms")) == 0) {
            std::fprintf(
                stderr, "error: no function symbols in %s\n", program.get("--syms").c_str());
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }

    std::string err;
    auto elf = ElfMemory::load(program.get("--elf"), err);
    if (!elf) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    EtmV4Config cfg;
    for (const auto& o : etm_opts)
        cfg.*(o.field) = static_cast<uint32_t>(program.get<long long>(o.name));
    auto decoder = make_opencsd_decoder(cfg);
    if (!decoder || !decoder->add_elf_memory(elf)) {
        std::fprintf(stderr, "error: cannot set up the OpenCSD decoder\n");
        return 1;
    }

    ProfileConfig pc;
    pc.sysclk_hz = program.get<double>("--sysclk-hz");
    pc.tsgen_hz = program.get<double>("--tsgen-hz");
    pc.period_exception
        = static_cast<uint32_t>(std::max(0, program.get<int>("--period-exception")));
    pc.period_cycles = static_cast<uint64_t>(std::max(0.0, program.get<double>("--period-cycles")));
    pc.top_blocks = static_cast<std::size_t>(std::max(0, program.get<int>("--top-blocks")));
    pc.block_min_count = static_cast<uint64_t>(std::max(1, program.get<int>("--block-min-count")));
    Profiler profiler(syms, pc);
    decoder->set_sink([&profiler](const Element& e) { profiler.process(e); });

    const std::string raw_path = program.get("--raw-in");
    FILE* f = std::fopen(raw_path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open %s\n", raw_path.c_str());
        return 1;
    }

    // The head of the file: the deframe phase is looked for here, then it is
    // decoded like the rest.
    std::vector<uint8_t> head(kPhaseSearchPrefix);
    head.resize(std::fread(head.data(), 1, head.size(), f));
    DeframePhase phase;
    phase.width = width;
    if (auto p = program.present("--phase")) {
        char* end = nullptr;
        phase.parity = static_cast<int>(std::strtol(p->c_str(), &end, 10));
        phase.order
            = (end && *end == ',') ? static_cast<int>(std::strtol(end + 1, nullptr, 10)) : 0;
    } else if (!find_deframe_phase(head.data(), head.size(), want_stream, width, phase)) {
        std::fprintf(stderr,
            "error: no ETM A-sync in the first %zu MiB: is the ETM armed (cortrace arm) and is "
            "--trace-width right?\n",
            head.size() >> 20);
        std::fclose(f);
        return 1;
    }
    std::fprintf(
        stderr, "[cortrace-prof] deframe phase (parity=%d, order=%d)\n", phase.parity, phase.order);

    StreamDeframer deframer(phase, want_stream);
    const int itm_stream = program.get<int>("--itm-stream");
    deframer.set_aux_stream(itm_stream);
    DwtEventParser event_parser;
    std::vector<uint8_t> etm;
    std::vector<AuxByte> aux;
    std::vector<DwtEvent> events;
    uint64_t raw_bytes = 0, etm_bytes = 0;
    bool fatal = false;
    auto feed = [&](const uint8_t* data, std::size_t n) {
        etm.clear();
        aux.clear();
        events.clear();
        deframer.feed(data, n, etm, itm_stream >= 0 ? &aux : nullptr);
        raw_bytes += n;
        etm_bytes += etm.size();
        // the events of this piece go in before its elements: the profiler
        // places them among the elements by their byte index
        event_parser.feed(aux.data(), aux.size(), events);
        for (const DwtEvent& ev : events)
            profiler.add_event(ev);
        if (!etm.empty() && !decoder->process(etm.data(), etm.size()))
            fatal = true;
    };
    for (std::size_t at = 0; at < head.size() && !fatal; at += kBlock)
        feed(head.data() + at, std::min(kBlock, head.size() - at));
    std::vector<uint8_t> buf(kBlock);
    while (!fatal && head.size() == kPhaseSearchPrefix) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0)
            break;
        feed(buf.data(), n);
    }
    std::fclose(f);
    decoder->flush();
    profiler.finish();
    if (fatal) {
        std::fprintf(stderr, "error: the OpenCSD decoder stopped on a fatal error\n");
        return 1;
    }

    const ProfileTotals& t = profiler.totals();
    if (t.cc_packets == 0) {
        std::fprintf(stderr,
            "error: the trace has no cycle counts. Arm the ETM with cycle counting: `cortrace "
            "arm --profile timing`.\n");
        return 2;
    }
    if (!write_file(program.get("--out"), profiler.to_json(program.get("--elf")))) {
        std::fprintf(stderr, "error: cannot write %s\n", program.get("--out").c_str());
        return 1;
    }
    std::fprintf(stderr,
        "[cortrace-prof] %llu B raw -> %llu B ETM; %llu cycles, %llu instructions (CPI %.3f); "
        "%llu losses\n",
        (unsigned long long)raw_bytes, (unsigned long long)etm_bytes, (unsigned long long)t.cycles,
        (unsigned long long)t.instructions,
        t.instructions ? double(t.cycles) / double(t.instructions) : 0.0,
        (unsigned long long)t.discontinuities());
    if (t.event_packets > 0)
        std::fprintf(stderr,
            "[cortrace-prof] DWT events: %llu packets (cpi %llu, exc %llu, sleep %llu, lsu %llu, "
            "fold %llu wraps), %llu rejected\n",
            (unsigned long long)t.event_packets, (unsigned long long)t.event_wraps[0],
            (unsigned long long)t.event_wraps[1], (unsigned long long)t.event_wraps[2],
            (unsigned long long)t.event_wraps[3], (unsigned long long)t.event_wraps[4],
            (unsigned long long)event_parser.rejected());
    if (auto d = profiler.clock_drift_ppm())
        std::fprintf(stderr, "[cortrace-prof] cycle clock vs timestamps: %+.1f ppm\n", *d);
    return 0;
}
