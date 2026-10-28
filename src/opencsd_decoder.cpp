// Cortrace — OpenCSD-backed ETMv4 decoder adapter.
//
// Wraps the OpenCSD C API and normalises its generic trace elements into
// cortrace::Element. The branch classification (call / return / indirect) is
// derived from OpenCSD's last_i_type / last_i_subtype so the call-stack machine
// never has to reason about ETM packet semantics.
//
// Compiled only when libopencsd is available (CORTRACE_HAVE_OPENCSD).
//
// SPDX-License-Identifier: MIT
#include "cortrace/decoder.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cortrace/log.hpp"

#include "opencsd/c_api/opencsd_c_api.h"
#include "opencsd/ocsd_if_types.h"
#include "opencsd/trc_gen_elem_types.h"

// The instruction decoder is C++ inside libopencsd; its header ships with the
// source tree (the vendored submodule) but not with every packaged install.
#if __has_include("i_dec/trc_i_decode.h")
#include "i_dec/trc_i_decode.h"
#define CORTRACE_HAVE_OCSD_IDEC 1
#endif

namespace cortrace {
namespace {

    // Parse the PT_LOAD program headers of a little-endian ELF32 into OpenCSD
    // memory regions (file offset -> physical/load address, filesz bytes). Using
    // the physical address (p_paddr) is what makes .data correct: its bytes live
    // in flash (LMA) even though they run from RAM (VMA). Only ELF32-LE is
    // supported (the Cortex-M target); returns false otherwise.
    bool parse_elf_load_segments(const std::string& path, std::vector<ocsd_file_mem_region_t>& out)
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        struct Closer {
            std::FILE* f;
            ~Closer() { std::fclose(f); }
        } closer { f };

        unsigned char e[52]; // ELF32 header size
        if (std::fread(e, 1, sizeof e, f) != sizeof e)
            return false;
        if (!(e[0] == 0x7f && e[1] == 'E' && e[2] == 'L' && e[3] == 'F'))
            return false;
        if (e[4] != 1 /*ELFCLASS32*/ || e[5] != 1 /*ELFDATA2LSB*/)
            return false;

        auto rd32 = [](const unsigned char* p) -> uint32_t {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
                | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        };
        auto rd16 = [](const unsigned char* p) -> uint16_t {
            return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
        };

        const uint32_t phoff = rd32(&e[28]); // e_phoff
        const uint16_t phentsize = rd16(&e[42]); // e_phentsize
        const uint16_t phnum = rd16(&e[44]); // e_phnum
        if (phoff == 0 || phnum == 0 || phentsize < 32)
            return false;

        for (uint16_t i = 0; i < phnum; ++i) {
            unsigned char ph[32]; // ELF32 program header
            if (std::fseek(f, static_cast<long>(phoff + i * phentsize), SEEK_SET) != 0)
                return false;
            if (std::fread(ph, 1, sizeof ph, f) != sizeof ph)
                return false;
            const uint32_t p_type = rd32(&ph[0]);
            if (p_type != 1 /*PT_LOAD*/)
                continue;
            const uint32_t p_offset = rd32(&ph[4]);
            const uint32_t p_paddr = rd32(&ph[12]); // load (physical) address
            const uint32_t p_filesz = rd32(&ph[16]);
            if (p_filesz == 0)
                continue; // .bss-only segment: nothing in the file
            ocsd_file_mem_region_t r;
            r.file_offset = p_offset;
            r.start_address = p_paddr;
            r.region_size = p_filesz;
            out.push_back(r);
        }
        return true;
    }

    // Translate an OpenCSD instruction type/subtype pair into a cortrace BranchKind.
    BranchKind classify(ocsd_instr_type itype, ocsd_instr_subtype sub)
    {
        switch (itype) {
        case OCSD_INSTR_BR:
            return sub == OCSD_S_INSTR_BR_LINK ? BranchKind::DirectCall : BranchKind::Direct;
        case OCSD_INSTR_BR_INDIRECT:
            if (sub == OCSD_S_INSTR_BR_LINK)
                return BranchKind::IndirectCall;
            if (sub == OCSD_S_INSTR_V7_IMPLIED_RET || sub == OCSD_S_INSTR_V8_RET
                || sub == OCSD_S_INSTR_V8_ERET)
                return BranchKind::Return;
            return BranchKind::IndirectBranch;
        default:
            return BranchKind::None;
        }
    }

    class OpenCsdDecoder final : public IDecoder {
    public:
        explicit OpenCsdDecoder(const EtmV4Config& cfg)
        {
            tree_ = ocsd_create_dcd_tree(OCSD_TRC_SRC_SINGLE, OCSD_DFRMTR_FRAME_MEM_ALIGN);
            if (tree_ == C_API_INVALID_TREE_HANDLE)
                return;

            ocsd_etmv4_cfg ocfg;
            std::memset(&ocfg, 0, sizeof ocfg);
            ocfg.reg_idr0 = cfg.idr0;
            ocfg.reg_idr1 = cfg.idr1;
            ocfg.reg_idr2 = cfg.idr2;
            ocfg.reg_idr8 = cfg.idr8;
            ocfg.reg_idr9 = cfg.idr9;
            ocfg.reg_idr10 = cfg.idr10;
            ocfg.reg_idr11 = cfg.idr11;
            ocfg.reg_idr12 = cfg.idr12;
            ocfg.reg_idr13 = cfg.idr13;
            ocfg.reg_configr = cfg.configr;
            ocfg.reg_traceidr = cfg.traceidr;
            ocfg.arch_ver = ARCH_V7;
            ocfg.core_prof = profile_CortexM;

            unsigned char csid = 0;
            if (ocsd_dt_create_decoder(
                    tree_, OCSD_BUILTIN_DCD_ETMV4I, OCSD_CREATE_FLG_FULL_DECODER, &ocfg, &csid)
                != OCSD_OK) {
                teardown();
                return;
            }
            if (ocsd_dt_set_gen_elem_outfn(tree_, &OpenCsdDecoder::elem_trampoline, this)
                != OCSD_OK) {
                teardown();
                return;
            }
            ok_ = true;
        }

        ~OpenCsdDecoder() override { teardown(); }

        bool valid() const { return ok_; }

        bool add_memory_image(uint32_t base, const std::string& path) override
        {
            if (!ok_)
                return false;
            return ocsd_dt_add_binfile_mem_acc(tree_, base, OCSD_MEM_SPACE_ANY, path.c_str())
                == OCSD_OK;
        }

        bool add_elf(const std::string& path) override
        {
            if (!ok_)
                return false;
            std::vector<ocsd_file_mem_region_t> regions;
            if (!parse_elf_load_segments(path, regions)) {
                CT_LOG_ERROR("add_elf: could not parse PT_LOAD segments from %s", path.c_str());
                return false;
            }
            if (regions.empty()) {
                CT_LOG_ERROR("add_elf: no loadable segments in %s", path.c_str());
                return false;
            }
            for (const auto& r : regions)
                CT_LOG_DEBUG("add_elf region: off=0x%zx addr=0x%llx size=0x%zx", r.file_offset,
                    static_cast<unsigned long long>(r.start_address), r.region_size);
            return ocsd_dt_add_binfile_region_mem_acc(tree_, regions.data(),
                       static_cast<int>(regions.size()), OCSD_MEM_SPACE_ANY, path.c_str())
                == OCSD_OK;
        }

        void set_sink(ElementSink sink) override { sink_ = std::move(sink); }

        bool process(const uint8_t* data, std::size_t len) override
        {
            if (!ok_)
                return false;
            std::size_t consumed = 0;
            while (consumed < len) {
                uint32_t used = 0;
                ocsd_datapath_resp_t r
                    = ocsd_dt_process_data(tree_, OCSD_OP_DATA, index_ + consumed,
                        static_cast<uint32_t>(len - consumed), data + consumed, &used);
                consumed += used;
                if (OCSD_DATA_RESP_IS_FATAL(r)) {
                    CT_LOG_ERROR("opencsd fatal decode response at byte %llu (resp=%d)",
                        static_cast<unsigned long long>(index_ + consumed), static_cast<int>(r));
                    return false;
                }
                if (used == 0) // decoder wants no more data from this buffer
                    break;
            }
            index_ += len;
            return true;
        }

        void flush() override
        {
            if (ok_)
                ocsd_dt_process_data(tree_, OCSD_OP_EOT, index_, 0, nullptr, nullptr);
        }

    private:
        void teardown()
        {
            if (tree_ != C_API_INVALID_TREE_HANDLE) {
                ocsd_destroy_dcd_tree(tree_);
                tree_ = C_API_INVALID_TREE_HANDLE;
            }
            ok_ = false;
        }

        void emit(const Element& e)
        {
            if (sink_)
                sink_(e);
        }

        void on_element(ocsd_trc_index_t idx, const ocsd_generic_trace_elem* elem)
        {
            const uint64_t bidx = static_cast<uint64_t>(idx);
            switch (elem->elem_type) {
            case OCSD_GEN_TRC_ELEM_INSTR_RANGE: {
                Element e = Element::instr_range(static_cast<uint32_t>(elem->st_addr),
                    static_cast<uint32_t>(elem->en_addr),
                    classify(elem->last_i_type, elem->last_i_subtype), elem->last_instr_exec != 0,
                    bidx, elem->has_cc != 0, elem->cycle_count);
                e.last_instr_sz = static_cast<uint8_t>(elem->last_instr_sz);
                e.last_instr_cond = elem->last_instr_cond != 0;
                e.num_instr = elem->num_instr_range;
                emit(e);
                break;
            }
            case OCSD_GEN_TRC_ELEM_CYCLE_COUNT: {
                // Standalone cycle-count element: cycles since the last counted
                // point, not tied to a range. Its own kind so the call-stack
                // machine accumulates the cycle clock without touching frames.
                Element e = Element::simple(ElementKind::CycleCount, bidx);
                e.has_cc = elem->has_cc != 0;
                e.cycle_count = elem->cycle_count;
                emit(e);
                break;
            }
            case OCSD_GEN_TRC_ELEM_EXCEPTION: {
                Element e = Element::exception(elem->exception_number, bidx);
                e.has_return_addr = elem->excep_ret_addr != 0;
                e.return_addr = static_cast<uint32_t>(elem->en_addr);
                emit(e);
                break;
            }
            case OCSD_GEN_TRC_ELEM_EXCEPTION_RET:
                emit(Element::simple(ElementKind::ExceptionRet, bidx));
                break;
            case OCSD_GEN_TRC_ELEM_TRACE_ON:
                emit(Element::simple(ElementKind::TraceOn, bidx));
                break;
            case OCSD_GEN_TRC_ELEM_ADDR_NACC:
                emit(Element::simple(ElementKind::AddrNacc, bidx));
                break;
            case OCSD_GEN_TRC_ELEM_TIMESTAMP:
                emit(Element::make_timestamp(elem->timestamp, bidx));
                break;
            case OCSD_GEN_TRC_ELEM_NO_SYNC:
                // Decoder lost sync -- the symptom of an ETM/ETF overflow or a
                // corrupt/truncated stream. Do NOT swallow it: surface it so the
                // report can tell the user the trace is lossy.
                emit(Element::simple(ElementKind::NoSync, bidx));
                break;
            case OCSD_GEN_TRC_ELEM_EO_TRACE:
            case OCSD_GEN_TRC_ELEM_PE_CONTEXT:
            case OCSD_GEN_TRC_ELEM_SYNC_MARKER:
            case OCSD_GEN_TRC_ELEM_EVENT:
                // Modeled-but-inert elements: end-of-buffer marker, PE context
                // updates (emitted on every context/thread change), sync markers
                // and numbered events. These are NORMAL stream content, not a
                // loss symptom -- count them as Benign so stream-health can tell
                // "healthy but unmodeled" apart from "actually lost data".
                emit(Element::simple(ElementKind::Benign, bidx));
                break;
            default:
                // Any element type we don't model is still counted (never
                // silently dropped), so anomalies can't hide.
                emit(Element::simple(ElementKind::OtherUnknown, bidx));
                break;
            }
        }

        static ocsd_datapath_resp_t elem_trampoline(const void* ctx, const ocsd_trc_index_t idx_sop,
            const uint8_t /*cs_id*/, const ocsd_generic_trace_elem* elem)
        {
            auto* self = static_cast<OpenCsdDecoder*>(const_cast<void*>(ctx));
            self->on_element(idx_sop, elem);
            return OCSD_RESP_CONT;
        }

        dcd_tree_handle_t tree_ = C_API_INVALID_TREE_HANDLE;
        ElementSink sink_;
        std::size_t index_ = 0;
        bool ok_ = false;
    };

} // namespace

std::unique_ptr<IDecoder> make_opencsd_decoder(const EtmV4Config& cfg)
{
    auto dec = std::make_unique<OpenCsdDecoder>(cfg);
    if (!dec->valid())
        return nullptr;
    return dec;
}

#if defined(CORTRACE_HAVE_OCSD_IDEC)
namespace {
    // Thumb decode with exactly the settings OpenCSD's ETMv4 decoder uses for
    // a Cortex-M (trc_pkt_decode_etmv4i.cpp): v7-M, IT blocks tracked, DSB/DMB
    // not waypoints, WFI/WFE not branches (TRCIDR2 of the M7 does not claim it).
    class OpenCsdInsnDecoder final : public IInsnDecoder {
    public:
        bool decode(uint32_t addr, uint32_t opcode, InsnDecodeState& st, Insn& out) override
        {
            ocsd_instr_info info;
            std::memset(&info, 0, sizeof info);
            info.pe_type.arch = ARCH_V7;
            info.pe_type.profile = profile_CortexM;
            info.isa = ocsd_isa_thumb2;
            info.instr_addr = addr;
            info.opcode = opcode;
            info.dsb_dmb_waypoints = 0;
            info.wfi_wfe_branch = 0;
            info.track_it_block = 1;
            info.thumb_it_conditions = st.it_remaining;
            if (idec_.DecodeInstruction(&info) != OCSD_OK)
                return false;
            st.it_remaining = info.thumb_it_conditions;
            out.size = info.instr_size;
            out.cond = info.is_conditional != 0;
            out.waypoint = info.type != OCSD_INSTR_OTHER;
            out.kind = classify(info.type, info.sub_type);
            out.target = info.type == OCSD_INSTR_BR ? static_cast<uint32_t>(info.branch_addr) : 0;
            return true;
        }

    private:
        TrcIDecode idec_;
    };
} // namespace

std::unique_ptr<IInsnDecoder> make_opencsd_insn_decoder()
{
    return std::make_unique<OpenCsdInsnDecoder>();
}
#else
std::unique_ptr<IInsnDecoder> make_opencsd_insn_decoder() { return nullptr; }
#endif

} // namespace cortrace
