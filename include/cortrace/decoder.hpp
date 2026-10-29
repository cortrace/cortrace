// Cortrace — decoder interface + OpenCSD-backed implementation.
//
// The decoder turns raw (already deframed) ETMv4 bytes into a stream of
// normalised cortrace::Element objects, delivered one-by-one to a sink
// callback. The upper layers (call-stack machine, sinks) only ever see
// `Element`, never OpenCSD types, so they stay unit-testable with synthetic
// streams and free of a live-decoder dependency.
//
// The OpenCSD implementation is compiled only when libopencsd is present
// (guarded by CORTRACE_HAVE_OPENCSD); the core library links fine without it.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_DECODER_HPP
#define CORTRACE_DECODER_HPP

#include "cortrace/codemap.hpp"
#include "cortrace/element.hpp"
#include "cortrace/elfmem.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace cortrace {

// Called for every decoded element, in trace order.
using ElementSink = std::function<void(const Element&)>;

// Minimal ETMv4 target configuration. The defaults describe a Cortex-M7 ETMv4
// (the core the pipeline was validated on); for other cores set the IDR/config
// words from the target's TRCIDRn / TRCCONFIGR (cortrace-decode --etm-*).
struct EtmV4Config {
    uint32_t idr0 = 0x080006e1;
    uint32_t idr1 = 0x4100f401;
    uint32_t idr2 = 0x00000004;
    uint32_t idr8 = 0x00000001;
    uint32_t idr9 = 0;
    uint32_t idr10 = 0;
    uint32_t idr11 = 0;
    uint32_t idr12 = 0x00000001;
    uint32_t idr13 = 0;
    uint32_t configr = 0x00000000;
    uint32_t traceidr = 0x00000002; // ETM trace ID on the ATB
};

// Abstract decoder so the CLI / tests can swap in a fake without OpenCSD.
class IDecoder {
public:
    virtual ~IDecoder() = default;

    // Register the memory image OpenCSD reads to follow the instruction flow.
    // `base` is the load address of `path`'s first byte (= lowest LMA of the
    // ELF; see AGENT.md gotcha #2 on the .isr_vector offset).
    //
    // PREFER add_elf(): a flat binary made with `objcopy -O binary` silently
    // mis-lays-out any image whose sections have gaps or a load address (LMA)
    // different from their virtual address (e.g. .data), so the bytes at a
    // given address no longer match the program. add_elf reads each PT_LOAD
    // segment at its own file offset and physical address, so it is always
    // consistent with the build and needs no base argument.
    virtual bool add_memory_image(uint32_t base, const std::string& path) = 0;

    // Register the program memory directly from an ELF, one region per PT_LOAD
    // segment (file offset -> physical/load address, filesz bytes). This is the
    // consistency-safe alternative to a flat binary. Returns false on failure.
    virtual bool add_elf(const std::string& path) = 0;

    // The same, from an ElfMemory that is already in RAM. Unlike add_elf this
    // is safe with decoders on several threads: they all read the one
    // immutable image (the decoder keeps a reference to it). A decoder that
    // cannot do this returns false.
    virtual bool add_elf_memory(const std::shared_ptr<const ElfMemory>& elf)
    {
        (void)elf;
        return false;
    }

    // Set the sink that receives normalised elements.
    virtual void set_sink(ElementSink sink) = 0;

    // Feed `len` bytes of deframed ETMv4 stream. May be called repeatedly.
    // Returns false on a fatal decode response.
    virtual bool process(const uint8_t* data, std::size_t len) = 0;

    // Signal end-of-trace so the decoder flushes any pending element.
    virtual void flush() = 0;
};

#if defined(CORTRACE_HAVE_OPENCSD)
// Construct an OpenCSD-backed ETMv4 decoder. Returns nullptr on setup failure.
// Creating and destroying decoders is serialised inside, so it may be done
// from several threads; decoding (process) needs nothing shared except the
// error logger, see opencsd_silence_errors().
std::unique_ptr<IDecoder> make_opencsd_decoder(const EtmV4Config& cfg);

// OpenCSD has one error logger for the whole process and it updates it without
// a lock whenever a decoder reports a problem, which damaged trace does all
// the time. Call this once, before decoders run on several threads: errors are
// then still returned to the caller as element kinds, just not logged by
// OpenCSD.
void opencsd_silence_errors();

// OpenCSD's own Thumb instruction decoder (the one its ETMv4 decoder follows
// the program with), set up for a Cortex-M ETMv4 like make_opencsd_decoder.
// Returns nullptr when this OpenCSD build does not ship its decoder headers.
std::unique_ptr<IInsnDecoder> make_opencsd_insn_decoder();
#endif

} // namespace cortrace

#endif // CORTRACE_DECODER_HPP
