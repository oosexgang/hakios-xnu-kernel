#include "hakios-metal/compute.hpp"

#include <algorithm>
#include <cstring>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"

namespace hakios::metal {

namespace compute {

namespace {

using namespace a6xx;

std::string n(uint64_t v) { return std::to_string(v); }

bool refuse(std::string* why, const std::string& text) {
    *why = text;
    return false;
}

void reg(pm4::Stream& st, uint32_t r, uint32_t v) { st.registers(r, &v, 1); }

void reg64(pm4::Stream& st, uint32_t r, uint64_t a) {
    uint32_t v[2] = {uint32_t(a), uint32_t(a >> 32)};
    st.registers(r, v, 2);
}

// CP_LOAD_STATE6_FRAG with the data already in GPU memory
void loadIndirect(pm4::Stream& st, a6xx_state_type type, uint32_t dstOff, uint32_t units, uint64_t from) {
    uint32_t w[3] = {loadState6_0(dstOff, type, SS6_INDIRECT, SB6_CS_SHADER, units),
                     CP_LOAD_STATE6_1_EXT_SRC_ADDR(uint32_t(from)), CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(uint32_t(from >> 32))};
    st.packet(CP_LOAD_STATE6_FRAG, w, 3);
}

// CP_LOAD_STATE6_FRAG with the data in the packet (Mesa writes zeros for the two address words)
void loadDirect(pm4::Stream& st, a6xx_state_type type, uint32_t dstOff, uint32_t units, const uint32_t* data, uint32_t dwords) {
    std::vector<uint32_t> w;
    w.reserve(3 + dwords);
    w.push_back(loadState6_0(dstOff, type, SS6_DIRECT, SB6_CS_SHADER, units));
    w.push_back(0);
    w.push_back(0);
    w.insert(w.end(), data, data + dwords);
    st.packet(CP_LOAD_STATE6_FRAG, w.data(), uint32_t(w.size()));
}

void event(pm4::Stream& st, vgt_event_type e) {
    uint32_t w = eventWrite0(e, false);
    st.packet(CP_EVENT_WRITE, &w, 1);
}

// an event that writes VALUE to ADDRESS when it has completed: how the GPU says the caches are flushed
void eventTimestamp(pm4::Stream& st, vgt_event_type e, uint64_t address, uint32_t value, bool timestampBit) {
    uint32_t w[4] = {eventWrite0(e, timestampBit), uint32_t(address), uint32_t(address >> 32), value};
    st.packet(CP_EVENT_WRITE, w, 4);
}

// the command processor waits until the word at ADDRESS equals VALUE
void waitForWord(pm4::Stream& st, uint64_t address, uint32_t value) {
    uint32_t w[6] = {waitRegMem0(WRITE_EQ, POLL_MEMORY), uint32_t(address), uint32_t(address >> 32), value, ~0u, kDelayLoopCycles};
    st.packet(CP_WAIT_REG_MEM, w, 6);
}

// ... or until it is at least VALUE
void waitForWordAtLeast(pm4::Stream& st, uint64_t address, uint32_t value) {
    uint32_t w[4] = {0, uint32_t(address), uint32_t(address >> 32), value};
    st.packet(CP_WAIT_MEM_GTE, w, 4);
}

uint32_t log2u(uint32_t v) {
    uint32_t r = 0;
    while ((1u << (r + 1)) <= v) r++;
    return r;
}

}  // namespace

bool encode(pm4::Stream& stream, const Kernel& k, const uint32_t groups[3], const uint32_t local[3],
            const std::vector<Binding>& buffers, const uint32_t* immediates, uint64_t program, uint64_t table,
            uint8_t* tableCpu, std::string* why, const Options& o) {
    static const char* const axis = "xyz";
    // --- the grid ---
    uint64_t threads = 1;
    for (int i = 0; i < 3; i++) {
        if (groups[i] == 0) {
            return refuse(why, std::string("a grid of 0 work groups along ") + axis[i]);
        }
        if (local[i] == 0 || local[i] > kMaxLocalThreads) {
            return refuse(why, std::string("a threadgroup of ") + n(local[i]) + " along " + axis[i] + ": 1 to " + n(kMaxLocalThreads));
        }
        threads *= local[i];
    }
    if (threads > kMaxLocalThreads) {
        return refuse(why, "a threadgroup of " + n(threads) + " threads: the GPU runs at most " + n(kMaxLocalThreads));
    }
    if (!k.localSizeVariable) {
        for (int i = 0; i < 3; i++) {
            if (local[i] != k.localSize[i]) {
                return refuse(why, std::string(k.name) + " fixes its threadgroup at " + n(k.localSize[0]) + "x" + n(k.localSize[1]) + "x" +
                                       n(k.localSize[2]) + ", not " + n(local[0]) + "x" + n(local[1]) + "x" + n(local[2]));
            }
        }
    }
    // --- the program ---
    if (k.instrlen == 0 || uint64_t(k.numWords) * 4 < uint64_t(k.instrlen) * 128) {
        return refuse(why, std::string(k.name) + ": " + n(k.numWords) + " words do not hold its instrlen of " + n(k.instrlen) + " units");
    }
    if (k.instrlen > kInstrCacheSize) {
        // Gallium follows a longer program with SP_FS_INSTRLEN and a LABEL event (Mesa MR 19023), a hardware bug's workaround
        // that nothing here has run; until it is measured, the limit is the cache
        return refuse(why, std::string(k.name) + " is " + n(k.instrlen) + " units long: more than the " + n(kInstrCacheSize) +
                               " the instruction cache preloads, and the longer-program path is not written here");
    }
    if (k.constlen > 256) {
        return refuse(why, std::string(k.name) + " wants " + n(k.constlen) + " vec4 of constants: compute has 256 on this GPU");
    }
    if (k.pvtmemSize != 0) {
        return refuse(why, std::string(k.name) + " wants " + n(k.pvtmemSize) + " bytes of private memory a fiber: not served yet");
    }
    if (k.numSamp != 0) {
        return refuse(why, std::string(k.name) + " samples textures: none are served yet");
    }
    a6xx_threadsize threadsize = k.doubleThreadsize ? THREAD128 : THREAD64;
    uint32_t subgroup = k.doubleThreadsize ? 2 * kThreadsizeBase : kThreadsizeBase;

    // --- the buffers: one descriptor per IBO slot, sized as the kernel declared or as the buffer is ---
    uint32_t nibo = std::max(k.numBufs, k.numIbos);
    if (nibo > kMaxIbos) {
        return refuse(why, n(nibo) + " buffer slots: the descriptor table holds " + n(kMaxIbos));
    }
    auto bound = [&](uint32_t slot, uint64_t* address, uint64_t* bytes) -> bool {
        if (slot >= buffers.size() || buffers[slot].buffer == nullptr) {
            return refuse(why, std::string(k.name) + ": no buffer bound at index " + n(slot));
        }
        const Binding& b = buffers[slot];
        if (b.offset % 4 != 0 || b.offset > b.buffer->length()) {
            return refuse(why, "buffer " + n(slot) + ": offset " + n(b.offset) + " is not a multiple of 4 inside it");
        }
        *address = b.buffer->gpuAddress() + b.offset;
        *bytes = b.buffer->length() - b.offset;
        return true;
    };
    for (uint32_t slot = 0; slot < nibo; slot++) {
        uint64_t address = 0, bytes = 0;
        if (!bound(slot, &address, &bytes)) {
            return false;
        }
        if (slot < k.numBufs) {
            if (bytes < k.bufs[slot].sizeBytes) {
                return refuse(why, std::string(k.name) + " declares buffer " + n(slot) + " as " + n(k.bufs[slot].sizeBytes) + " bytes; " +
                                       n(bytes) + " are bound");
            }
            bytes = k.bufs[slot].sizeBytes;   // the descriptor covers what the kernel declared, not what lies beyond
        }
        if (bytes / 4 >= (1ull << 30)) {
            return refuse(why, "buffer " + n(slot) + " is longer than a descriptor counts");
        }
        TexConst d = bufferIbo(address, uint32_t(bytes / 4));
        memcpy(tableCpu + 64 * slot, d.dword, sizeof d.dword);
    }
    memset(tableCpu + kTableTimestampsAt, 0, 64);

    // --- the constants ---
    std::vector<uint32_t> imm(k.immediates, k.immediates + k.numImmediates);
    if (immediates != nullptr) {
        imm.assign(immediates, immediates + k.numImmediates);
    }
    imm.resize((imm.size() + 3) & ~size_t(3), 0);
    for (uint32_t i = 0; i < k.numBufs; i++) {
        if (k.bufs[i].constRegid != kNoConstRegid) {
            // a text kernel that reads its buffer's address out of a constant (ldg/stg): computerator patches it in here too
            uint64_t address = 0, bytes = 0;
            bound(i, &address, &bytes);
            int64_t at = int64_t(k.bufs[i].constRegid) - int64_t(k.immediatesOffsetVec4) * 4;
            if (at < 0 || uint64_t(at) + 1 >= imm.size()) {
                return refuse(why, std::string(k.name) + ": buffer " + n(i) + "'s address slot c" + n(k.bufs[i].constRegid / 4) + " is outside its constants");
            }
            imm[size_t(at)] = uint32_t(address);
            imm[size_t(at) + 1] = uint32_t(address >> 32);
        }
    }
    if (k.numwgConstRegid != kNoConstRegid) {
        int64_t at = int64_t(k.numwgConstRegid) - int64_t(k.immediatesOffsetVec4) * 4;
        if (at < 0 || uint64_t(at) + 2 >= imm.size()) {
            return refuse(why, std::string(k.name) + ": the work group count's slot is outside its constants");
        }
        for (int i = 0; i < 3; i++) imm[size_t(at) + i] = groups[i];
    }
    uint32_t immUnits = uint32_t(imm.size() / 4);
    if (immUnits != 0 && k.immediatesOffsetVec4 + immUnits > k.constlen) {
        return refuse(why, std::string(k.name) + ": " + n(immUnits) + " vec4 of constants at " + n(k.immediatesOffsetVec4) + " run past its constlen of " + n(k.constlen));
    }
    // struct ir3_driver_params_cs (Mesa ir3_shader.h), as Gallium's ir3_build_driver_params_cs fills it: a kernel whose
    // threadgroup size is a dispatch argument reads it from here, and reads garbage if this is left out
    uint32_t params[16] = {groups[0], groups[1], groups[2], 3, 0, 0, 0, subgroup, local[0], local[1], local[2], log2u(subgroup), 0, 0, 0, 0};
    if (k.driverParamsOffsetVec4 >= 0 && uint32_t(k.driverParamsOffsetVec4) + 4 > k.constlen) {
        return refuse(why, std::string(k.name) + ": its driver params at vec4 " + n(uint32_t(k.driverParamsOffsetVec4)) + " run past its constlen of " + n(k.constlen));
    }
    struct Range { uint32_t offsetVec4, units; uint64_t from; };
    std::vector<Range> ranges;
    for (uint32_t i = 0; i < k.numUboRanges; i++) {
        const KernelUboRange& r = k.uboRanges[i];
        uint64_t address = 0, bytes = 0;
        if (!bound(r.block, &address, &bytes)) {
            return false;
        }
        if (r.sizeDwords == 0 || r.sizeDwords % 4 != 0) {
            return refuse(why, std::string(k.name) + ": a constant range of " + n(r.sizeDwords) + " dwords, not a whole number of vec4");
        }
        if (uint64_t(r.startDword) * 4 + uint64_t(r.sizeDwords) * 4 > bytes) {
            return refuse(why, std::string(k.name) + " reads " + n(r.sizeDwords * 4) + " bytes at " + n(r.startDword * 4) + " of constant buffer " + n(r.block) +
                                   "; " + n(bytes) + " are bound");
        }
        if (r.offsetVec4 + r.sizeDwords / 4 > k.constlen) {
            return refuse(why, std::string(k.name) + ": constant buffer " + n(r.block) + "'s range at vec4 " + n(r.offsetVec4) + " runs past its constlen");
        }
        ranges.push_back(Range{r.offsetVec4, r.sizeDwords / 4, address + 4ull * r.startDword});
    }

    // --- nothing is refused from here on: the stream, in computerator's order ---
    if (o.prelude) {
        reg(stream, REG_A6XX_SP_MODE_CONTROL, A6XX_SP_MODE_CONTROL_CONSTANT_DEMOTION_ENABLE | A6XX_SP_MODE_CONTROL_ISAMMODE(ISAMMODE_GL));
        reg(stream, REG_A6XX_SP_PERFCTR_ENABLE, A6XX_SP_PERFCTR_ENABLE_CS);
        reg(stream, REG_A6XX_SP_FLOAT_CNTL, 0);
    }
    if (o.invalidateCmd) {
        // whatever state loads are still queued for any stage are dropped before the new program's
        reg(stream, REG_A6XX_HLSQ_INVALIDATE_CMD,
            A6XX_HLSQ_INVALIDATE_CMD_VS_STATE | A6XX_HLSQ_INVALIDATE_CMD_HS_STATE | A6XX_HLSQ_INVALIDATE_CMD_DS_STATE |
                A6XX_HLSQ_INVALIDATE_CMD_GS_STATE | A6XX_HLSQ_INVALIDATE_CMD_FS_STATE | A6XX_HLSQ_INVALIDATE_CMD_CS_STATE |
                A6XX_HLSQ_INVALIDATE_CMD_CS_IBO | A6XX_HLSQ_INVALIDATE_CMD_GFX_IBO);
    }
    reg(stream, REG_A6XX_HLSQ_CS_CNTL, hlsqCsCntl(k.constlen));
    {
        uint32_t v[2] = {spCsConfig(nibo, k.numSamp, k.numSamp), k.instrlen};   // SP_CS_CONFIG, SP_CS_INSTRLEN
        stream.registers(REG_A6XX_SP_CS_CONFIG, v, 2);
    }
    reg(stream, REG_A6XX_SP_CS_CTRL_REG0,
        spCsCtrlReg0(uint32_t(k.maxReg + 1), uint32_t(k.maxHalfReg + 1), k.branchstackHw, threadsize, k.mergedRegs, k.earlyPreamble));
    // the shared memory and constant split, in both the SP's and (gen4's) the HLSQ's copy, the same value: Gallium gives
    // the HLSQ the real size where computerator writes 1, and the one that is right when they disagree is not measured
    uint32_t ctrl1 = spCsCtrlReg1(k.sharedSize, k.constlen);
    reg(stream, REG_A6XX_SP_CS_CTRL_REG1, ctrl1);
    if (o.hlsqCopies) {
        reg(stream, REG_A6XX_HLSQ_CS_CTRL_REG1, ctrl1);
    }
    {
        uint32_t v[2] = {csCntl0(k.workGroupIdRegid, k.localIdRegid), csCntl1(threadsize)};
        stream.registers(REG_A6XX_HLSQ_CS_CNTL_0, v, 2);   // HLSQ_CS_CNTL_0, HLSQ_CS_CNTL_1
        if (o.hlsqCopies) {
            stream.registers(REG_A6XX_SP_CS_CNTL_0, v, 2); // SP_CS_CNTL_0, SP_CS_CNTL_1 (has_lpac)
        }
    }
    {
        uint32_t v[3] = {0, uint32_t(program), uint32_t(program >> 32)};   // SP_CS_OBJ_FIRST_EXEC_OFFSET, SP_CS_OBJ_START lo, hi
        stream.registers(REG_A6XX_SP_CS_OBJ_FIRST_EXEC_OFFSET, v, 3);
    }
    if (o.pvtmemZero) {
        uint32_t v[4] = {0, 0, 0, 0};   // SP_CS_PVT_MEM_PARAM, _ADDR lo, hi, _SIZE: no private memory
        stream.registers(REG_A6XX_SP_CS_PVT_MEM_PARAM, v, 4);
        reg(stream, REG_A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET, 0);
    }
    loadIndirect(stream, ST6_SHADER, 0, std::min(k.instrlen, kInstrCacheSize), program);
    if (immUnits != 0) {
        loadDirect(stream, ST6_CONSTANTS, k.immediatesOffsetVec4, immUnits, imm.data(), uint32_t(imm.size()));
    }
    if (k.driverParamsOffsetVec4 >= 0) {
        loadDirect(stream, ST6_CONSTANTS, uint32_t(k.driverParamsOffsetVec4), 4, params, 16);
    }
    for (const Range& r : ranges) {
        loadIndirect(stream, ST6_CONSTANTS, r.offsetVec4, r.units, r.from);
    }
    if (nibo != 0) {
        loadIndirect(stream, ST6_IBO, 0, nibo, table);
        reg64(stream, REG_A6XX_SP_CS_IBO, table);
        reg(stream, REG_A6XX_SP_CS_IBO_COUNT, nibo);
    }
    if (o.invalidate) {
        // what the CPU or a blit wrote last is what the kernel reads
        event(stream, CACHE_INVALIDATE);
    }
    if (o.marker) {
        uint32_t w = setMarker0(RM6_COMPUTE);
        stream.packet(CP_SET_MARKER, &w, 1);
    }
    {
        uint32_t v[7] = {ndrange0(3, local[0], local[1], local[2]), local[0] * groups[0], 0, local[1] * groups[1], 0, local[2] * groups[2], 0};
        stream.registers(REG_A6XX_HLSQ_CS_NDRANGE_0, v, 7);
        uint32_t ones[3] = {1, 1, 1};
        stream.registers(REG_A6XX_HLSQ_CS_KERNEL_GROUP_X, ones, 3);
    }
    {
        uint32_t w[4] = {0, groups[0], groups[1], groups[2]};
        stream.packet(CP_EXEC_CS, w, 4);
    }
    stream.waitForIdle();
    if (o.tail) {
        // the end, as computerator's cache_flush: the GPU says it is done, the caches are flushed, and the flush is waited for
        uint64_t ts = table + kTableTimestampsAt;
        if (o.tailRbDone) {
            eventTimestamp(stream, RB_DONE_TS, ts, 1, o.tailTimestampBit);
            waitForWord(stream, ts, 1);
        }
        if (o.tailCacheFlush) {
            eventTimestamp(stream, CACHE_FLUSH_TS, ts + 4, 2, o.tailTimestampBit);
            waitForWordAtLeast(stream, ts + 4, 2);
        }
    }
    return true;
}

}  // namespace compute

bool CommandBuffer::dispatch(const Kernel& k, const uint32_t groups[3], const uint32_t* local, const std::vector<Binding>& buffers,
                             const uint32_t* immediates, const compute::Options& options) {
    if (status_ > kEnqueued) {
        return fail("dispatch after commit");
    }
    std::string why;
    std::shared_ptr<Buffer> program = newBuffer(queue_.gpu(), 4ull * k.numWords, &why);
    if (program == nullptr) {
        return fail(std::string("no memory for the program: ") + why);
    }
    memcpy(program->contents(), k.words, 4ull * k.numWords);
    std::shared_ptr<Buffer> table = newBuffer(queue_.gpu(), compute::kTableBytes, &why);
    if (table == nullptr) {
        return fail(std::string("no memory for the descriptors: ") + why);
    }
    uint32_t loc[3] = {k.localSize[0], k.localSize[1], k.localSize[2]};
    if (local != nullptr) {
        for (int i = 0; i < 3; i++) loc[i] = local[i];
    }
    pm4::Stream added;
    if (!compute::encode(added, k, groups, loc, buffers, immediates, program->gpuAddress(), table->gpuAddress(),
                         static_cast<uint8_t*>(table->contents()), &why, options)) {
        return fail(why);
    }
    const std::vector<uint32_t>& w = added.words();
    stream_.packet(pm4::kNop, nullptr, 0);   // (a marker between encoders in the stream, for whoever reads it back)
    for (size_t i = 0; i < w.size();) {
        pm4::Header h = pm4::decode(w[i]);
        // the encoder's packets are copied whole; Stream has no append, and the header is re-read only to find each end
        if (h.kind == pm4::Header::kRegisters) {
            stream_.registers(h.reg, &w[i + 1], h.count);
        } else {
            stream_.packet(h.opcode, h.count ? &w[i + 1] : nullptr, h.count);
        }
        i += 1 + h.count;
    }
    use(*program, false);
    use(*table, true);
    for (const Binding& b : buffers) {
        if (b.buffer != nullptr) {
            use(*b.buffer, true);
        }
    }
    keep(program);
    keep(table);
    return true;
}

}  // namespace hakios::metal
