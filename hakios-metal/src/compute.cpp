#include "hakios-metal/compute.hpp"

#include <algorithm>
#include <cstring>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/texture.hpp"

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

// CP_LOAD_STATE6_FRAG with the data already in GPU memory. The block is the compute shader's (program, constants,
// IBOs) or, for the texture and sampler tables, SB6_CS_TEX, where ST6_SHADER means sampler descriptors of 16 bytes and
// ST6_CONSTANTS texture descriptors of 64 (Gallium's fd6_texture.cc:593-687 is the only reading of that double meaning).
void loadIndirect(pm4::Stream& st, a6xx_state_type type, uint32_t dstOff, uint32_t units, uint64_t from,
                  a6xx_state_block block = SB6_CS_SHADER) {
    uint32_t w[3] = {loadState6_0(dstOff, type, SS6_INDIRECT, block, units),
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

const char* elementName(uint8_t e) { return e == kElemFloat ? "float" : e == kElemUint ? "uint" : e == kElemInt ? "int" : "?"; }

std::string dimName(const KernelTexture& t) {
    const char* d = t.dim == kTexDim1D ? "1D" : t.dim == kTexDim2D ? "2D" : t.dim == kTexDim3D ? "3D" : t.dim == kTexDimCube ? "a cube" : t.dim == kTexDimMS ? "2D multisample" : "an unknown dimension";
    return std::string(d) + (t.isArray ? " array" : "");
}

}  // namespace

bool encode(pm4::Stream& stream, const Kernel& k, const uint32_t groups[3], const uint32_t local[3],
            const std::vector<Binding>& buffers, const uint32_t* immediates, uint64_t program, uint64_t table,
            uint8_t* tableCpu, std::string* why, const Options& o) {
    Bindings b;
    b.buffers = buffers;
    return encode(stream, k, groups, local, b, immediates, program, table, tableCpu, why, o);
}

namespace {
// the dispatch, direct (GROUPS) or indirect (the counts at INDIRECT, GROUPS then zeros)
bool encodeAny(pm4::Stream& stream, const Kernel& k, const uint32_t groups[3], uint64_t indirect, const uint32_t local[3], const Bindings& bindings,
               const uint32_t* immediates, uint64_t program, uint64_t table, uint8_t* tableCpu, std::string* why, const Options& o) {
    static const char* const axis = "xyz";
    const std::vector<Binding>& buffers = bindings.buffers;
    // --- the grid ---
    uint64_t threads = 1;
    for (int i = 0; i < 3; i++) {
        if (indirect == 0 && groups[i] == 0) {
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
    if (k.constlen > 256) {
        return refuse(why, std::string(k.name) + " wants " + n(k.constlen) + " vec4 of constants: compute has 256 on this GPU");
    }
    // private memory: the queue's (CommandBuffer::dispatch binds it), as large as the kernel's fibers want and in the
    // layout it was compiled for -- a smaller one would have one fiber's spills land in the next one's
    const PrivateMemory& pm = bindings.privateMemory;
    if (k.pvtmemSize != 0 && (pm.address == 0 || pm.perFiber < k.pvtmemSize || pm.perWave != k.pvtmemPerWave || pm.address % 32 != 0)) {
        return refuse(why, std::string(k.name) + " wants " + n(k.pvtmemSize) + " bytes of private memory a fiber" + (k.pvtmemPerWave ? " laid out a wave" : "") +
                               ": the private memory bound has " + n(pm.perFiber) + (pm.perWave ? " laid out a wave" : "") + " at " + n(pm.address));
    }
    const uint32_t shared = std::max(k.sharedSize, bindings.sharedBytes);
    if (shared > kMaxSharedBytes) {
        return refuse(why, std::string(k.name) + " with " + n(shared) + " bytes of threadgroup memory: a group has " + n(kMaxSharedBytes) + " on this GPU");
    }
    if (k.numSamp > 31) {
        // SP_CS_CONFIG.NSAMP is 5 bits (a6xx.hpp: A6XX_SP_CS_CONFIG_NSAMP__MASK) and field() masks silently: 32 would be written as 0
        return refuse(why, std::string(k.name) + ": numSamp " + n(k.numSamp) + " does not fit SP_CS_CONFIG.NSAMP's 5 bits (31 at most)");
    }
    // an indexed `constant` load ir3 kept as an ldc reads UBO descriptor n, Metal's [[buffer(n)]]: one per UBO ir3 counts,
    // the bound buffer past its offset and its length in vec4 (fd6_emit_ubos, fd6_const.cc:266-296), each bound and
    // starting on a vec4 -- the ldc would otherwise read a descriptor nobody wrote
    std::vector<uint32_t> ubos;
    if (k.numUbos > 31) {
        return refuse(why, std::string(k.name) + " indexes " + n(k.numUbos) + " constant buffers: Metal's buffer table has 31");
    }
    for (uint32_t u = 0; u < k.numUbos; u++) {
        if (k.constantDataUbo != 0 && u == k.constantDataUbo - 1) {
            // the constant data (a module's constant table) where the program's words were uploaded (render.cpp's way)
            uint32_t d[2];
            uboDescriptor(program + k.constantDataOffset, (k.constantDataSize + 15) / 16, d);
            ubos.push_back(d[0]);
            ubos.push_back(d[1]);
            continue;
        }
        if (u >= buffers.size() || buffers[u].buffer == nullptr) {
            // the highest is what an ldc reads; one below it may be read by none (render.cpp: uboDescriptors)
            if (u + 1 == k.numUbos) {
                return refuse(why, std::string(k.name) + ": no buffer bound at index " + n(u) + " (a constant buffer it indexes)");
            }
            ubos.push_back(0);
            ubos.push_back(0);
            continue;
        }
        const Binding& x = buffers[u];
        // (a multiple of 4, as a draw's: render.cpp's uboDescriptors and probe 9o2)
        if (x.offset % 4 != 0 || x.offset > x.buffer->length()) {
            return refuse(why, std::string(k.name) + ": buffer " + n(u) + ": offset " + n(x.offset) + " is not a multiple of 4 inside it");
        }
        uint32_t d[2];
        uboDescriptor(x.buffer->gpuAddress() + x.offset, uint32_t(std::min<uint64_t>((x.buffer->length() - x.offset + 15) / 16, kUboMaxVec4)), d);
        ubos.push_back(d[0]);
        ubos.push_back(d[1]);
    }
    if (k.numTextureSlots > kMaxTextureSlots || (k.samplerMask >> kMaxTextureSlots) != 0) {
        return refuse(why, std::string(k.name) + " names texture or sampler indices past " + n(kMaxTextureSlots - 1) + ": the tables hold " + n(kMaxTextureSlots));
    }
    a6xx_threadsize threadsize = k.doubleThreadsize ? THREAD128 : THREAD64;
    uint32_t subgroup = k.doubleThreadsize ? 2 * kThreadsizeBase : kThreadsizeBase;

    // --- the buffers: one descriptor per IBO slot, sized as the kernel declared or as the buffer is ---
    uint32_t nibo = std::max(k.numBufs, k.numIbos);
    if (nibo > kMaxIbos) {
        return refuse(why, n(nibo) + " buffer slots: the descriptor table holds " + n(kMaxIbos));
    }
    // bytes: what the program bound (its length past the offset); mapped: what the GPU can read there without a
    // fault (the object's pages past the offset; the object is whole pages, so it is never less)
    auto bound = [&](uint32_t slot, uint64_t* address, uint64_t* bytes, uint64_t* mapped = nullptr) -> bool {
        if (slot >= buffers.size() || buffers[slot].buffer == nullptr) {
            return refuse(why, std::string(k.name) + ": no buffer bound at index " + n(slot));
        }
        const Binding& b = buffers[slot];
        if (b.offset % 4 != 0 || b.offset > b.buffer->length()) {
            return refuse(why, "buffer " + n(slot) + ": offset " + n(b.offset) + " is not a multiple of 4 inside it");
        }
        *address = b.buffer->gpuAddress() + b.offset;
        *bytes = b.buffer->length() - b.offset;
        if (mapped != nullptr) *mapped = b.buffer->bo().size() - b.offset;
        return true;
    };
    // the texture at Metal index I as the kernel's instructions address it: the kernel's shape (textures[I]) against the
    // texture's; a sampled texture is read through the texture table, an image stored (and loaded) through an IBO slot
    auto boundTexture = [&](uint32_t i, const Texture** out) -> bool {
        const KernelTexture& t = k.textures[i];
        if (i >= bindings.textures.size() || bindings.textures[i] == nullptr) {
            return refuse(why, std::string(k.name) + ": no texture bound at index " + n(i));
        }
        const Texture* tex = bindings.textures[i];
        // an image is stored through storageDescriptor, which knows the 2D, 2D-array and 3D layouts (and a cube's
        // faces, but Metal has no writable cube in a kernel's signature that reaches here as one); a sampled texture is
        // read through the same descriptor a draw uses, so every type a draw takes
        const bool storable = tex->type() == TextureType::k2D || tex->type() == TextureType::k2DArray || tex->type() == TextureType::k3D;
        if (!tex->isShaped(t) || (t.kind == kTexImage && !storable)) {
            return refuse(why, std::string(k.name) + " addresses texture " + n(i) + " as " + dimName(t) + (t.kind == kTexImage ? " image" : "") +
                                   "; texture " + n(i) + " is of type " + n(uint32_t(tex->type())) +
                                   ": a kernel stores into 2D, 2D-array and 3D textures and samples every type, each as itself");
        }
        if (t.element != tex->info().element) {
            return refuse(why, "texture " + n(i) + " is " + tex->info().name + " (" + elementName(tex->info().element) + "); " + k.name +
                                   (t.kind == kTexImage ? " stores through it as texture2d<" : " reads it as texture2d<") + elementName(t.element) + ">");
        }
        if (t.kind == kTexImage) {
            if (t.write && (tex->usage() & kTextureUsageWrite) == 0) {
                return refuse(why, "texture " + n(i) + " was made without shader-write usage; " + k.name + " writes it");
            }
            if (t.read && (tex->usage() & kTextureUsageRead) == 0) {
                return refuse(why, "texture " + n(i) + " was made without shader-read usage; " + k.name + " loads from it");
            }
        } else if ((tex->usage() & kTextureUsageRead) == 0) {
            return refuse(why, "texture " + n(i) + " was made without shader-read usage; " + k.name + " reads it");
        }
        *out = tex;
        return true;
    };
    for (uint32_t slot = 0; slot < nibo; slot++) {
        TexConst d{};
        if (k.numBufs != 0 || slot < k.numSsbos) {
            // a text kernel's @buf, or a device buffer (an SSBO) of an AIR kernel: Metal's [[buffer(slot)]]
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
            d = bufferIbo(address, uint32_t(bytes / 4));
        } else {
            // an image slot: Metal's [[texture(i)]] declared write or read_write lands at IBO numSsbos + i (ir3_image.c:50-52).
            // An index in that range the kernel does not store through (tex_copy: in at texture 1 is sampled, out at 0 is
            // the image, and ir3 counts images up to the highest index) gets the hole descriptor: one FMT6_32_UINT element
            // over the table's own kTableHoleAt bytes, real memory nothing names (compute.hpp says why not zeros).
            uint32_t i = slot - k.numSsbos;
            if (i < kMaxTextureSlots && k.textures[i].kind == kTexImage) {
                const Texture* tex = nullptr;
                if (!boundTexture(i, &tex)) {
                    return false;
                }
                d = tex->storageDescriptor();
            } else {
                d = bufferIbo(table + kTableHoleAt, 1);
            }
        }
        memcpy(tableCpu + kTableIbosAt + 64 * slot, d.dword, sizeof d.dword);
    }
    memset(tableCpu + kTableHoleAt, 0, 64);
    writeBorderColors(tableCpu + kTableBorderColorAt);   // the entries a sampler's BCOLOR names
    memset(tableCpu + kTableTimestampsAt, 0, 64);

    // --- the textures and samplers: the two tables, only for a kernel with sampled textures (Gallium builds the CS_TEX
    // group only when num_textures > 0, fd6_emit.cc:43-44, and leaves SP_CS_TEX_* stale otherwise) ---
    uint32_t numTex = k.numTextureSlots, numSampDesc = 0;
    if (numTex != 0) {
        memset(tableCpu + kTableTexConstAt, 0, kMaxTextureSlots * 64);   // an unaddressed slot is a zero descriptor, Gallium's dummy_view
        memset(tableCpu + kTableTexSampAt, 0, kMaxTextureSlots * 16);    // ... and dummy_sampler
        for (uint32_t t = 0; t < numTex; t++) {
            if (k.textures[t].kind != kTexSampled) continue;
            const Texture* tex = nullptr;
            if (!boundTexture(t, &tex)) {
                return false;
            }
            TexConst d = tex->sampledDescriptor();
            memcpy(tableCpu + kTableTexConstAt + 64 * t, d.dword, sizeof d.dword);
        }
        // at least one sampler descriptor whenever textures are loaded: an isaml (texture.read) names s#0 and reads no
        // sampler, and whether the TP consults TEX_SAMP for it is unverified (Mesa never leaves a bound texture without one)
        numSampDesc = 1;
        for (uint32_t m = 0; m < kMaxTextureSlots; m++) {
            if ((k.samplerMask >> m) & 1) numSampDesc = m + 1;
        }
        for (uint32_t m = 0; m < numSampDesc; m++) {
            if (((k.samplerMask >> m) & 1) == 0) continue;
            if (m >= bindings.samplers.size() || bindings.samplers[m] == nullptr) {
                return refuse(why, std::string(k.name) + ": no sampler bound at index " + n(m));
            }
            memcpy(tableCpu + kTableTexSampAt + 16 * m, bindings.samplers[m]->words, 16);
        }
    }

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
        if (indirect != 0 && k.numwgConstRegid % 4 != 0) {
            // the counts reach the slot by a vec4 load of the copy (below), so the slot has to be a vec4's start
            return refuse(why, std::string(k.name) + ": its work group count is at c" + n(k.numwgConstRegid / 4) + "." + "xyzw"[k.numwgConstRegid % 4] +
                                   ", not a vec4's start: an indirect dispatch loads the counts a vec4 at a time");
        }
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
    // (a source off 16 bytes is loaded direct from a snapshot of the words: render.cpp's PushedRange says why)
    struct Range { uint32_t offsetVec4, units; uint64_t from; std::vector<uint32_t> words; };
    std::vector<Range> ranges;
    for (uint32_t i = 0; i < k.numUboRanges; i++) {
        const KernelUboRange& r = k.uboRanges[i];
        if (k.constantDataUbo != 0 && r.block == k.constantDataUbo - 1) {
            // the constant data's range: its words direct, zeros past their end (render.cpp: pushedRanges)
            if (r.offsetVec4 + r.sizeDwords / 4 > k.constlen) {
                return refuse(why, std::string(k.name) + ": its constant data's range runs past its constlen");
            }
            Range g{r.offsetVec4, r.sizeDwords / 4, 0, std::vector<uint32_t>(r.sizeDwords, 0u)};
            for (uint32_t d = 0; d < r.sizeDwords; d++) {
                if (uint64_t(r.startDword + d) * 4 + 4 <= k.constantDataSize) g.words[d] = k.words[k.constantDataOffset / 4 + r.startDword + d];
            }
            ranges.push_back(std::move(g));
            continue;
        }
        uint64_t address = 0, bytes = 0, mapped = 0;
        if (!bound(r.block, &address, &bytes, &mapped)) {
            return false;
        }
        if (r.sizeDwords == 0 || r.sizeDwords % 4 != 0) {
            return refuse(why, std::string(k.name) + ": a constant range of " + n(r.sizeDwords) + " dwords, not a whole number of vec4");
        }
        // The range is ir3's, in whole vec4 and padded: fill_id reads 4 bytes of its constant and the range is 64. The
        // GPU reads the padding too, so the read must stay inside the object's pages, where nothing faults -- not
        // inside the length the program bound. Metal binds 4 bytes by -setBytes:length:atIndex: to such a kernel as
        // a matter of course (the plug-in's test on the phone, 28 October), and a refusal here failed that dispatch.
        if (uint64_t(r.startDword) * 4 + uint64_t(r.sizeDwords) * 4 > mapped) {
            return refuse(why, std::string(k.name) + " reads " + n(r.sizeDwords * 4) + " bytes at " + n(r.startDword * 4) + " of constant buffer " + n(r.block) +
                                   "; " + n(bytes) + " are bound and " + n(mapped) + " mapped behind the offset");
        }
        if (r.offsetVec4 + r.sizeDwords / 4 > k.constlen) {
            return refuse(why, std::string(k.name) + ": constant buffer " + n(r.block) + "'s range at vec4 " + n(r.offsetVec4) + " runs past its constlen");
        }
        Range g{r.offsetVec4, r.sizeDwords / 4, address + 4ull * r.startDword, {}};
        if (g.from % 16 != 0) {
            const Binding& x = buffers[r.block];
            if (x.buffer->contents() == nullptr) {
                return refuse(why, std::string(k.name) + ": constant buffer " + n(r.block) + " is bound at an address the CP cannot load from (not a multiple of 16) and is mapped nowhere to copy from");
            }
            g.words.resize(r.sizeDwords);
            memcpy(g.words.data(), static_cast<const uint8_t*>(x.buffer->contents()) + x.offset + 4ull * r.startDword, 4ull * r.sizeDwords);
        }
        ranges.push_back(std::move(g));
    }

    // --- nothing is refused from here on: the stream, in computerator's order ---
    if (o.prelude) {
        reg(stream, REG_A6XX_SP_MODE_CONTROL, A6XX_SP_MODE_CONTROL_CONSTANT_DEMOTION_ENABLE | A6XX_SP_MODE_CONTROL_ISAMMODE(ISAMMODE_GL));
        reg(stream, REG_A6XX_SP_PERFCTR_ENABLE, A6XX_SP_PERFCTR_ENABLE_CS);
        reg(stream, REG_A6XX_SP_FLOAT_CNTL, 0);
    }
    if (o.tpState) {
        // the TP's own copy of ISAMMODE (isaml's coordinate convention depends on it, and SP_MODE_CONTROL's copy above is
        // not it: Gallium writes both, fd6_emit.cc:977-981), and the border colour table both TP base registers read for
        // a CLAMP_TO_BORDER sampler (fd6_emit.cc:1035-1038): entry 0 of it, zeros, is in the dispatch's own table
        reg(stream, REG_A6XX_SP_TP_MODE_CNTL, spTpModeCntl());
        reg64(stream, REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR, table + kTableBorderColorAt);
        reg64(stream, REG_A6XX_SP_PS_TP_BORDER_COLOR_BASE_ADDR, table + kTableBorderColorAt);
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
    uint32_t ctrl1 = spCsCtrlReg1(shared, k.constlen);
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
    if (k.pvtmemSize != 0) {
        // SP_CS_PVT_MEM_PARAM, _ADDR lo, hi, _SIZE and the stack offset as tu6_emit_xs writes a stage's (the draw's the same)
        uint32_t v[4] = {A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM(pm.perFiber), uint32_t(pm.address), uint32_t(pm.address >> 32),
                         A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(pm.perSp) | (pm.perWave ? A6XX_SP_CS_PVT_MEM_SIZE_PERWAVEMEMLAYOUT : 0u)};
        stream.registers(REG_A6XX_SP_CS_PVT_MEM_PARAM, v, 4);
        reg(stream, REG_A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET, A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET(pm.perSp));
    } else if (o.pvtmemZero) {
        uint32_t v[4] = {0, 0, 0, 0};   // SP_CS_PVT_MEM_PARAM, _ADDR lo, hi, _SIZE: no private memory
        stream.registers(REG_A6XX_SP_CS_PVT_MEM_PARAM, v, 4);
        reg(stream, REG_A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET, 0);
    }
    loadIndirect(stream, ST6_SHADER, 0, std::min({k.instrlen, kInstrCacheSize, o.preloadCap != 0 ? o.preloadCap : kInstrCacheSize}), program);
    if (immUnits != 0) {
        loadDirect(stream, ST6_CONSTANTS, k.immediatesOffsetVec4, immUnits, imm.data(), uint32_t(imm.size()));
    }
    if (k.driverParamsOffsetVec4 >= 0) {
        loadDirect(stream, ST6_CONSTANTS, uint32_t(k.driverParamsOffsetVec4), 4, params, 16);
    }
    for (const Range& r : ranges) {
        if (!r.words.empty()) {
            loadDirect(stream, ST6_CONSTANTS, r.offsetVec4, r.units, r.words.data(), uint32_t(r.words.size()));
        } else {
            loadIndirect(stream, ST6_CONSTANTS, r.offsetVec4, r.units, r.from);
        }
    }
    if (!ubos.empty()) {
        loadDirect(stream, ST6_UBO, 0, uint32_t(ubos.size() / 2), ubos.data(), uint32_t(ubos.size()));
    }
    if (nibo != 0) {
        loadIndirect(stream, ST6_IBO, 0, nibo, table + kTableIbosAt);
        reg64(stream, REG_A6XX_SP_CS_IBO, table + kTableIbosAt);
        reg(stream, REG_A6XX_SP_CS_IBO_COUNT, nibo);
    }
    if (numTex != 0 && o.texState) {
        // Gallium's CS_TEX group, in its order (fd6_texture.cc:593-687): the samplers, their pointer, the textures, their
        // pointer, the count. SP_CS_CONFIG's NTEX and NSAMP (above) stay ir3's num_samp, which counts images too and so may
        // exceed this count, as in every Gallium dispatch of a shader with an image.
        loadIndirect(stream, ST6_SHADER, 0, numSampDesc, table + kTableTexSampAt, SB6_CS_TEX);
        reg64(stream, REG_A6XX_SP_CS_TEX_SAMP, table + kTableTexSampAt);
        loadIndirect(stream, ST6_CONSTANTS, 0, numTex, table + kTableTexConstAt, SB6_CS_TEX);
        reg64(stream, REG_A6XX_SP_CS_TEX_CONST, table + kTableTexConstAt);
        reg(stream, REG_A6XX_SP_CS_TEX_COUNT, numTex);
    }
    if (indirect != 0) {
        // the counts into the table's two vec4s, as turnip does for an offset CP_LOAD_STATE6 cannot read from
        // (tu_cmd_buffer.cc:6714-6738): wait for what may still write them, copy, have the copies land and the caches
        // forget the old words, then load each vec4 over what the direct loads above put there. The fourth lanes are
        // the words those loads had: the slot's own fourth immediate, and the params' work_dim
        uint32_t* lanes = reinterpret_cast<uint32_t*>(tableCpu + kTableIndirectAt);
        lanes[3] = 0;
        if (k.numwgConstRegid != kNoConstRegid) {
            const size_t at = size_t(k.numwgConstRegid - k.immediatesOffsetVec4 * 4) + 3;
            lanes[3] = at < imm.size() ? imm[at] : 0;
        }
        lanes[7] = params[3];
        stream.waitForIdle();
        for (uint32_t v = 0; v < 2; v++) {
            for (uint32_t i = 0; i < 3; i++) {
                const uint64_t dst = table + kTableIndirectAt + 16 * v + 4 * i, src = indirect + 4 * i;
                uint32_t w[5] = {0, uint32_t(dst), uint32_t(dst >> 32), uint32_t(src), uint32_t(src >> 32)};
                stream.packet(CP_MEM_TO_MEM, w, 5);
            }
        }
        stream.packet(pm4::kWaitMemWrites, nullptr, 0);
        event(stream, CACHE_INVALIDATE);
        stream.waitForIdle();
        if (k.numwgConstRegid != kNoConstRegid) loadIndirect(stream, ST6_CONSTANTS, k.numwgConstRegid / 4, 1, table + kTableIndirectAt);
        if (k.driverParamsOffsetVec4 >= 0) loadIndirect(stream, ST6_CONSTANTS, uint32_t(k.driverParamsOffsetVec4), 1, table + kTableIndirectAt + 16);
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
    // A program longer than the instruction cache is fetched from memory as it runs, and that fetch is bounds-checked
    // against SP_FS_INSTRLEN of the other register context instead of SP_CS_INSTRLEN (Mesa issue 5892, MR 19023: "affects
    // all known gens"; Gallium's fd6_compute.cc and turnip's tu_dispatch work around it the same way). Measured here:
    // with SP_FS_INSTRLEN left as it was, a 134-unit chain finished with every word wrong and a 492-unit one hung. So the
    // FS length is set to the kernel's and a LABEL event rolls the context before the dispatch, and another after it, so
    // that the write and CP_EXEC_CS cannot race.
    const bool instrlenWorkaround = k.instrlen > kInstrCacheSize || (o.preloadCap != 0 && k.instrlen > o.preloadCap);
    if (instrlenWorkaround) {
        reg(stream, REG_A6XX_SP_FS_INSTRLEN, k.instrlen);
        event(stream, LABEL);
    }
    if (indirect != 0) {
        uint32_t w[4] = {0, uint32_t(indirect), uint32_t(indirect >> 32), execCsIndirect3(local[0], local[1], local[2])};
        stream.packet(CP_EXEC_CS_INDIRECT, w, 4);
    } else {
        uint32_t w[4] = {0, groups[0], groups[1], groups[2]};
        stream.packet(CP_EXEC_CS, w, 4);
    }
    if (instrlenWorkaround) {
        event(stream, LABEL);
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
}  // namespace

bool encode(pm4::Stream& stream, const Kernel& k, const uint32_t groups[3], const uint32_t local[3], const Bindings& bindings,
            const uint32_t* immediates, uint64_t program, uint64_t table, uint8_t* tableCpu, std::string* why, const Options& o) {
    return encodeAny(stream, k, groups, 0, local, bindings, immediates, program, table, tableCpu, why, o);
}

bool encodeIndirect(pm4::Stream& stream, const Kernel& k, uint64_t indirect, const uint32_t local[3], const Bindings& bindings,
                    const uint32_t* immediates, uint64_t program, uint64_t table, uint8_t* tableCpu, std::string* why, const Options& o) {
    if (indirect == 0 || indirect % 4 != 0) {
        return refuse(why, "an indirect dispatch's counts at " + n(indirect) + ": a buffer's address, a multiple of 4");
    }
    const uint32_t none[3] = {0, 0, 0};
    return encodeAny(stream, k, none, indirect, local, bindings, immediates, program, table, tableCpu, why, o);
}

}  // namespace compute

bool CommandBuffer::dispatch(const Kernel& k, const uint32_t groups[3], const uint32_t* local, const std::vector<Binding>& buffers,
                             const uint32_t* immediates, const compute::Options& options) {
    Bindings b;
    b.buffers = buffers;
    return dispatch(k, groups, local, b, immediates, options);
}

bool CommandBuffer::dispatch(const Kernel& k, const uint32_t groups[3], const uint32_t* local, const Bindings& bindings,
                             const uint32_t* immediates, const compute::Options& options) {
    return dispatchAny(k, groups, nullptr, 0, local, bindings, immediates, options);
}

bool CommandBuffer::dispatchIndirect(const Kernel& k, const Buffer& args, uint64_t offset, const uint32_t* local, const Bindings& bindings,
                                     const uint32_t* immediates, const compute::Options& options) {
    if (offset % 4 != 0 || offset + 12 > args.length()) {
        return fail("an indirect dispatch's counts at " + std::to_string(offset) + " of a " + std::to_string(args.length()) +
                    "-byte buffer: three words inside it, at a multiple of 4");
    }
    return dispatchAny(k, nullptr, &args, offset, local, bindings, immediates, options);
}

bool CommandBuffer::dispatchAny(const Kernel& k, const uint32_t* groups, const Buffer* args, uint64_t offset, const uint32_t* local,
                                const Bindings& bindings, const uint32_t* immediates, const compute::Options& options) {
    if (status_ > kEnqueued) {
        return fail("dispatch after commit");
    }
    std::string why;
    const uint64_t program = programAddress(k, &why);
    if (program == 0) {
        return fail(std::string("no memory for the program: ") + why);
    }
    Piece table;
    if (!piece(compute::kTableBytes, &table, &why)) {
        return fail(std::string("no memory for the descriptors: ") + why);
    }
    uint32_t loc[3] = {k.localSize[0], k.localSize[1], k.localSize[2]};
    if (local != nullptr) {
        for (int i = 0; i < 3; i++) loc[i] = local[i];
    }
    // private memory is the queue's, as for a draw: the kernel spills into it, so the submission names it for writing
    Bindings withScratch;
    const Bindings* b = &bindings;
    std::shared_ptr<Buffer> scratch;
    if (k.pvtmemSize != 0) {
        withScratch = bindings;
        scratch = queue_.privateMemory(k.pvtmemSize, k.pvtmemPerWave, &withScratch.privateMemory, &why);
        if (scratch == nullptr) {
            return fail(std::string("no private memory: ") + why);
        }
        b = &withScratch;
    }
    pm4::Stream added;
    const bool encoded = args != nullptr ? compute::encodeIndirect(added, k, args->gpuAddress() + offset, loc, *b, immediates, program, table.gpuAddress(),
                                                                   table.contents(), &why, options)
                                         : compute::encode(added, k, groups, loc, *b, immediates, program, table.gpuAddress(), table.contents(), &why, options);
    if (!encoded) {
        return refuseWork(why);
    }
    counts_.dispatches++;
    const std::vector<uint32_t>& w = added.words();
    closePass();   // a dispatch reads what the draws before it wrote: their pass's tail first
    stampStart(kPassDispatch, groups ? groups[0] : 0, groups ? groups[1] : 0);
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
    stampEnd();
    for (const Binding& bb : bindings.buffers) {
        if (bb.buffer != nullptr) {
            use(*bb.buffer, true);
        }
    }
    if (scratch != nullptr) use(*scratch, true);
    if (args != nullptr) use(*args, false);   // the CP reads the counts from it as it runs
    // every texture bound, the kernel's or not (an extra one is harmless to name): its memory is used for the flight and
    // kept alive until completion, written when the kernel stores through it
    for (size_t i = 0; i < bindings.textures.size(); i++) {
        const Texture* t = bindings.textures[i];
        if (t == nullptr) continue;
        bool writes = i < kMaxTextureSlots && k.textures[i].kind == kTexImage && k.textures[i].write;
        use(*t->buffer(), writes);
        keep(t->buffer());
    }
    return true;
}

}  // namespace hakios::metal
