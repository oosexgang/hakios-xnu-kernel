// The render path (render.hpp): ONE draw of a Program into ONE colour target in SYSMEM mode, as compute.cpp encodes a
// dispatch -- the refusals first, by name, then the table, then the stream group by group under render::Options'
// switches. The register sequence is turnip's sysmem pass and r3d path with fd6 as the second witness (render.hpp's
// file comment names the functions); every value here cites the line it was read at. Nothing of this has run on the
// phone: the fake GPU (fake_gpu.cpp) holds the stream together, hakios-metal-probe stage 9 is what measures it.
//
// SPDX-License-Identifier: MIT
#include "hakios-metal/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <tuple>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/texture.hpp"

namespace hakios::metal {

namespace render {

namespace {

using namespace a6xx;

std::string n(uint64_t v) { return std::to_string(v); }

bool refuse(std::string* why, const std::string& text) {
    if (why != nullptr) *why = text;
    return false;
}

const char* stageName(const Stage& s) { return s.k.name != nullptr ? s.k.name : s.kind == kStageVertex ? "the vertex stage" : "the fragment stage"; }

void reg(pm4::Stream& st, uint32_t r, uint32_t v) { st.registers(r, &v, 1); }

void reg64(pm4::Stream& st, uint32_t r, uint64_t a) {
    uint32_t v[2] = {uint32_t(a), uint32_t(a >> 32)};
    st.registers(r, v, 2);
}

// consecutive registers from R on, one type-4 (turnip's pkt4 of several)
void regs(pm4::Stream& st, uint32_t r, std::initializer_list<uint32_t> values) {
    uint32_t v[16];
    uint32_t count = 0;
    for (uint32_t x : values) v[count++] = x;
    st.registers(r, v, count);
}

// a type-7 packet of one word
void command1(pm4::Stream& st, uint32_t opcode, uint32_t word) { st.packet(opcode, &word, 1); }

void event(pm4::Stream& st, vgt_event_type e) {
    uint32_t w = eventWrite0(e, false);
    st.packet(CP_EVENT_WRITE, &w, 1);
}

// the 4-word event: VALUE written to ADDRESS when it has completed. turnip's form (tu_emit_raw_event_write,
// tu_cmd_buffer.cc:48-70) has no TIMESTAMP bit; with it the GPU writes a timestamp instead and the wait below never
// ends (28 October, every compute dispatch). The bit is here for the fake GPU's test of that alone.
void eventTimestamp(pm4::Stream& st, vgt_event_type e, uint64_t address, uint32_t value, bool timestampBit) {
    uint32_t w[4] = {eventWrite0(e, timestampBit), uint32_t(address), uint32_t(address >> 32), value};
    st.packet(CP_EVENT_WRITE, w, 4);
}

void waitForWord(pm4::Stream& st, uint64_t address, uint32_t value) {
    uint32_t w[6] = {waitRegMem0(WRITE_EQ, POLL_MEMORY), uint32_t(address), uint32_t(address >> 32), value, ~0u, kDelayLoopCycles};
    st.packet(CP_WAIT_REG_MEM, w, 6);
}

void waitForWordNot(pm4::Stream& st, uint64_t address, uint32_t value) {
    uint32_t w[6] = {waitRegMem0(WRITE_NE, POLL_MEMORY), uint32_t(address), uint32_t(address >> 32), value, ~0u, kDelayLoopCycles};
    st.packet(CP_WAIT_REG_MEM, w, 6);
}

void waitForWordAtLeast(pm4::Stream& st, uint64_t address, uint32_t value) {
    uint32_t w[4] = {0, uint32_t(address), uint32_t(address >> 32), value};
    st.packet(CP_WAIT_MEM_GTE, w, 4);
}

// CP_LOAD_STATE6_GEOM for the vertex stage's loads, _FRAG for the fragment stage's (tu6_stage2opcode, tu_shader.cc:1349;
// fd6_stage2opcode, fd6_emit.h:331-334): the program from GPU memory (ST6_SHADER, 128-byte units) or constants in
// the packet (ST6_CONSTANTS, vec4 units, the two address words zero as Mesa writes them)
uint32_t loadOpcode(const Stage& s) { return s.kind == kStageVertex ? uint32_t(CP_LOAD_STATE6_GEOM) : uint32_t(CP_LOAD_STATE6_FRAG); }
a6xx_state_block shaderBlock(const Stage& s) { return s.kind == kStageVertex ? SB6_VS_SHADER : SB6_FS_SHADER; }

void loadIndirect(pm4::Stream& st, const Stage& s, a6xx_state_type type, uint32_t dstOff, uint32_t units, uint64_t from) {
    uint32_t w[3] = {loadState6_0(dstOff, type, SS6_INDIRECT, shaderBlock(s), units), CP_LOAD_STATE6_1_EXT_SRC_ADDR(uint32_t(from)),
                     CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(uint32_t(from >> 32))};
    st.packet(loadOpcode(s), w, 3);
}

void loadDirect(pm4::Stream& st, const Stage& s, a6xx_state_type type, uint32_t dstOff, uint32_t units, const uint32_t* data, uint32_t dwords) {
    std::vector<uint32_t> w;
    w.reserve(3 + dwords);
    w.push_back(loadState6_0(dstOff, type, SS6_DIRECT, shaderBlock(s), units));
    w.push_back(0);
    w.push_back(0);
    w.insert(w.end(), data, data + dwords);
    st.packet(loadOpcode(s), w.data(), uint32_t(w.size()));
}

// The colour target as RB_MRT[0], SP_FS_MRT_REG[0] and the sRGB registers name it, off the Texture's own fields: the
// format's .rb column is its .tex one for every served format (fd6_format_table.c's VTC rows: RGBA8 :163, BGRA8 :174,
// R32Uint :132, R32Float :136, RGBA16F :249, RGBA32F :284, so FormatInfo::fmt6 and swap are the colour ones too), the
// pitch is the layout's (texturePitch), the layer size the buffer's length (layerBytes2d, 4096-aligned, fdl6_view_init's
// layer_size at fd6_view.c:192), the base the buffer's address (64-aligned by the layout), BASE_GMEM 0 in sysmem
// (r3d_dst, tu_clear_blit.cc:1400). COLOR_UINT/SINT for the pure-integer formats (fd6_view.c:415-418). (texture.hpp
// declares no method for this; texture_test holds the same fields through the same encoders to the 64x64 literals.)
struct MrtWords {
    uint32_t bufInfo, pitch, arrayPitch, baseLo, baseHi, baseGmem, spFsMrtReg;
    bool srgb;
};

// the format RB_MRT_BUF_INFO names (GRAS_LRZ_MRT_BUF_INFO_0 repeats attachment 0's)
a6xx_format mrtFormatOf(uint32_t bufInfo) {
    return a6xx_format((bufInfo & A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__MASK) >> A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__SHIFT);
}

// (at mip level LEVEL: its address and its pitch -- fdl6_view_init's base_miplevel, the RB told the level alone)
MrtWords mrtWords(const Texture& t, uint32_t level, uint32_t slice = 0) {
    const FormatInfo& f = t.info();
    const TextureLevel L = t.level(level);
    MrtWords m{};
    // (a layer, a face or a depth plane: the slices are one 2D layout each, sliceBytes apart -- fd6_layout.c's layer_size)
    const uint64_t base = t.levelAddress(level) + (slice != 0 ? uint64_t(slice) * t.sliceBytes() : 0);
    m.bufInfo = rbMrtBufInfo(f.fmt6, TILE6_LINEAR, f.swap);
    m.pitch = rbMrtPitch(uint32_t(L.pitch));
    m.arrayPitch = rbMrtArrayPitch(uint32_t(t.buffer()->length()));
    m.baseLo = uint32_t(base);
    m.baseHi = uint32_t(base >> 32);
    m.baseGmem = 0;
    m.spFsMrtReg = spFsMrtReg(f.fmt6, f.element == kElemInt, f.element == kElemUint);
    m.srgb = f.srgb;
    return m;
}

// what both stages share with a compute Kernel, refused as compute.cpp refuses it, plus what the first draw does not
// serve in either stage (render.hpp's list)
bool checkStage(const Stage& s, std::string* why) {
    const Kernel& k = s.k;
    const std::string name = stageName(s);
    if (k.instrlen == 0 || uint64_t(k.numWords) * 4 < uint64_t(k.instrlen) * 128) {
        return refuse(why, name + ": " + n(k.numWords) + " words do not hold its instrlen of " + n(k.instrlen) + " units");
    }
    if (k.numSamp > 31) {
        return refuse(why, name + ": numSamp " + n(k.numSamp) + " does not fit SP_xS_CONFIG.NSAMP's 5 bits (31 at most)");
    }
    // A `constant` buffer is a range ir3 pushes or an ldc through a UBO descriptor (uboDescriptors); a `device` one an
    // SSBO through the 3D pipeline's IBO table (gfxIbos), its slots the Metal buffer indices; images there are not served
    if (k.numIbos > k.numSsbos || k.numSsbos > kMaxVertexBuffers) {
        return refuse(why, name + " has " + n(k.numIbos) + " IBO slots for " + n(k.numSsbos) + " device buffer(s): images in a draw are not served");
    }
    if (k.numUbos > kMaxVertexBuffers) {
        return refuse(why, name + " indexes " + n(k.numUbos) + " constant buffers: Metal's buffer table has " + n(kMaxVertexBuffers));
    }
    for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
        if (k.textures[i].kind == kTexImage) {
            // an image is an IBO of the 3D pipe, the same table a device buffer needs (above)
            return refuse(why, name + " stores through texture " + n(i) + " as an image: images in a draw (the 3D IBO table) are not served");
        }
    }
    if (k.numTextureSlots > kMaxTextureSlots || (k.samplerMask >> kMaxTextureSlots) != 0) {
        return refuse(why, name + " names texture or sampler indices past " + n(kMaxTextureSlots - 1) + ": the tables hold " + n(kMaxTextureSlots));
    }
    for (uint32_t i = k.numTextureSlots; i < kMaxTextureSlots; i++) {
        if (k.textures[i].kind == kTexSampled) {
            return refuse(why, name + " samples texture " + n(i) + " past its numTextureSlots of " + n(k.numTextureSlots) + ": the stage's tables disagree");
        }
    }
    if (k.driverParamsOffsetVec4 >= 0 && s.kind != kStageVertex) {
        // an FS's driver params (subgroup size, the fragment density words) are not worked out here
        return refuse(why, name + " reads driver params at vec4 " + n(uint32_t(k.driverParamsOffsetVec4)) + ": only a vertex stage's are served");
    }
    if (k.driverParamsOffsetVec4 >= 0 && uint32_t(k.driverParamsOffsetVec4) + 1 > k.constlen) {
        return refuse(why, name + ": its driver params at vec4 " + n(uint32_t(k.driverParamsOffsetVec4)) + " run past its constlen of " + n(k.constlen));
    }
    if (k.constantDataSize != 0 && (k.constantDataUbo == 0 || uint64_t(k.constantDataOffset) + k.constantDataSize > 4ull * k.numWords)) {
        return refuse(why, name + " carries " + n(k.constantDataSize) + " bytes of constant data that no UBO of ir3's reads, or outside its words");
    }
    uint32_t immUnits = (k.numImmediates + 3) / 4;
    if (immUnits != 0 && k.immediatesOffsetVec4 + immUnits > k.constlen) {
        return refuse(why, name + ": " + n(immUnits) + " vec4 of constants at " + n(k.immediatesOffsetVec4) + " run past its constlen of " + n(k.constlen));
    }
    return true;
}

// A pushed constant range of a stage as the GPU reads it: DST_OFF, units and the address in the bound buffer -- or, when
// that address is not a multiple of 16, the words themselves. CP_LOAD_STATE6 reads an indirect source from the address
// rounded down to 16 bytes (probe 9o4: bound at 4, 8 or 120 the consts held the 4, 8 or 8 bytes before the colour;
// at 16 the colour), while Metal binds a constant buffer at any multiple of 4 and CoreAnimation binds its uniforms at
// multiples of 8. Such a range is loaded direct from the bytes as they are when the draw is encoded: a snapshot, which
// is what -setFragmentBytes: gives anyway, and what a caller that writes its uniforms before encoding cannot tell apart.
struct PushedRange {
    uint32_t offsetVec4, units;
    uint64_t from;
    std::vector<uint32_t> words;   // non-empty: load these direct instead of reading FROM
};

// The stage's pushed ranges against its own [[buffer(n)]] table, by compute's rules (compute.cpp: the range is ir3's,
// in whole vec4 and padded -- a 16-byte float4 is pushed as 64 bytes -- so the read must stay inside the bound object's
// pages, not inside the length bound; it must land inside the stage's constlen). False with *why naming the stage.
bool pushedRanges(const Stage& s, const Bindings& b, std::vector<PushedRange>* out, std::string* why) {
    const Kernel& k = s.k;
    const std::string name = stageName(s);
    for (uint32_t i = 0; i < k.numUboRanges; i++) {
        const KernelUboRange& r = k.uboRanges[i];
        // the constant data's range (a module's constant table, kernel.hpp: constantDataUbo): its words direct, from
        // the program's own, past their end zeros (ir3 pushes whole vec4, the table need not fill the last)
        if (k.constantDataUbo != 0 && r.block == k.constantDataUbo - 1) {
            if (r.offsetVec4 + r.sizeDwords / 4 > k.constlen) {
                return refuse(why, name + ": its constant data's range at vec4 " + n(r.offsetVec4) + " runs past its constlen of " + n(k.constlen));
            }
            PushedRange p{r.offsetVec4, r.sizeDwords / 4, 0, std::vector<uint32_t>(r.sizeDwords, 0u)};
            for (uint32_t d = 0; d < r.sizeDwords; d++) {
                const uint64_t at = uint64_t(r.startDword + d) * 4;
                if (at + 4 <= k.constantDataSize) p.words[d] = k.words[k.constantDataOffset / 4 + r.startDword + d];
            }
            out->push_back(std::move(p));
            continue;
        }
        if (r.block >= b.buffers.size() || b.buffers[r.block].buffer == nullptr) {
            return refuse(why, name + ": no buffer bound at index " + n(r.block) + " (its constant buffer)");
        }
        const Binding& x = b.buffers[r.block];
        if (x.offset % 4 != 0 || x.offset > x.buffer->length()) {
            return refuse(why, name + ": buffer " + n(r.block) + ": offset " + n(x.offset) + " is not a multiple of 4 inside it");
        }
        uint64_t bytes = x.buffer->length() - x.offset, mapped = x.buffer->bo().size() - x.offset;
        if (r.sizeDwords == 0 || r.sizeDwords % 4 != 0) {
            return refuse(why, name + ": a constant range of " + n(r.sizeDwords) + " dwords, not a whole number of vec4");
        }
        if (uint64_t(r.startDword) * 4 + uint64_t(r.sizeDwords) * 4 > mapped) {
            return refuse(why, name + " reads " + n(r.sizeDwords * 4) + " bytes at " + n(r.startDword * 4) + " of constant buffer " + n(r.block) + "; " +
                                   n(bytes) + " are bound and " + n(mapped) + " mapped behind the offset");
        }
        if (r.offsetVec4 + r.sizeDwords / 4 > k.constlen) {
            return refuse(why, name + ": constant buffer " + n(r.block) + "'s range at vec4 " + n(r.offsetVec4) + " runs past its constlen of " + n(k.constlen));
        }
        PushedRange p{r.offsetVec4, r.sizeDwords / 4, x.buffer->gpuAddress() + x.offset + 4ull * r.startDword, {}};
        if (p.from % 16 != 0) {
            if (x.buffer->contents() == nullptr) {
                return refuse(why, name + ": constant buffer " + n(r.block) + " is bound at an address the CP cannot load from (not a multiple of 16) and is mapped nowhere to copy from");
            }
            p.words.resize(r.sizeDwords);
            memcpy(p.words.data(), static_cast<const uint8_t*>(x.buffer->contents()) + x.offset + 4ull * r.startDword, 4ull * r.sizeDwords);
        }
        out->push_back(std::move(p));
    }
    return true;
}

// gl_vert_attrib's VERT_ATTRIB_GENERIC0 (Mesa shader_enums.h): a VS input at this slot + n is [[attribute(n)]]
constexpr uint32_t kVertAttribGeneric0 = 15;

// The served MTLVertexFormats as the fetch reads them: fd6_format_table.c's vertex column (VTC/VT_ rows), the bytes one
// element takes, and whether the decoded value is a float (VFD_DECODE_INSTR.FLOAT: set unless the format is a pure
// integer one, tu_pipeline.cc:2356)
struct VertexFormatInfo {
    VertexFormat format;
    a6xx_format fmt6;
    a3xx_color_swap swap;
    uint32_t bytes;
    bool isFloat;
    const char* name;
};
const VertexFormatInfo kVertexFormats[] = {
    {VertexFormat::kUChar2, FMT6_8_8_UINT, WZYX, 2, false, "UChar2"},   // fd6_format_table.c:97
    {VertexFormat::kUChar3, FMT6_8_8_8_UINT, WZYX, 3, false, "UChar3"},   // fd6_format_table.c:124
    {VertexFormat::kUChar4, FMT6_8_8_8_8_UINT, WZYX, 4, false, "UChar4"},   // fd6_format_table.c:169
    {VertexFormat::kChar2, FMT6_8_8_SINT, WZYX, 2, false, "Char2"},   // fd6_format_table.c:98
    {VertexFormat::kChar3, FMT6_8_8_8_SINT, WZYX, 3, false, "Char3"},   // fd6_format_table.c:125
    {VertexFormat::kChar4, FMT6_8_8_8_8_SINT, WZYX, 4, false, "Char4"},   // fd6_format_table.c:170
    {VertexFormat::kUChar2Normalized, FMT6_8_8_UNORM, WZYX, 2, true, "UChar2Normalized"},   // fd6_format_table.c:95
    {VertexFormat::kUChar3Normalized, FMT6_8_8_8_UNORM, WZYX, 3, true, "UChar3Normalized"},   // fd6_format_table.c:122
    {VertexFormat::kUChar4Normalized, FMT6_8_8_8_8_UNORM, WZYX, 4, true, "UChar4Normalized"},   // fd6_format_table.c:163
    {VertexFormat::kChar2Normalized, FMT6_8_8_SNORM, WZYX, 2, true, "Char2Normalized"},   // fd6_format_table.c:96
    {VertexFormat::kChar3Normalized, FMT6_8_8_8_SNORM, WZYX, 3, true, "Char3Normalized"},   // fd6_format_table.c:123
    {VertexFormat::kChar4Normalized, FMT6_8_8_8_8_SNORM, WZYX, 4, true, "Char4Normalized"},   // fd6_format_table.c:167
    {VertexFormat::kUShort2, FMT6_16_16_UINT, WZYX, 4, false, "UShort2"},   // fd6_format_table.c:151
    {VertexFormat::kUShort3, FMT6_16_16_16_UINT, WZYX, 6, false, "UShort3"},   // fd6_format_table.c:232
    {VertexFormat::kUShort4, FMT6_16_16_16_16_UINT, WZYX, 8, false, "UShort4"},   // fd6_format_table.c:243
    {VertexFormat::kShort2, FMT6_16_16_SINT, WZYX, 4, false, "Short2"},   // fd6_format_table.c:152
    {VertexFormat::kShort3, FMT6_16_16_16_SINT, WZYX, 6, false, "Short3"},   // fd6_format_table.c:233
    {VertexFormat::kShort4, FMT6_16_16_16_16_SINT, WZYX, 8, false, "Short4"},   // fd6_format_table.c:245
    {VertexFormat::kUShort2Normalized, FMT6_16_16_UNORM, WZYX, 4, true, "UShort2Normalized"},   // fd6_format_table.c:149
    {VertexFormat::kUShort3Normalized, FMT6_16_16_16_UNORM, WZYX, 6, true, "UShort3Normalized"},   // fd6_format_table.c:230
    {VertexFormat::kUShort4Normalized, FMT6_16_16_16_16_UNORM, WZYX, 8, true, "UShort4Normalized"},   // fd6_format_table.c:239
    {VertexFormat::kShort2Normalized, FMT6_16_16_SNORM, WZYX, 4, true, "Short2Normalized"},   // fd6_format_table.c:150
    {VertexFormat::kShort3Normalized, FMT6_16_16_16_SNORM, WZYX, 6, true, "Short3Normalized"},   // fd6_format_table.c:231
    {VertexFormat::kShort4Normalized, FMT6_16_16_16_16_SNORM, WZYX, 8, true, "Short4Normalized"},   // fd6_format_table.c:241
    {VertexFormat::kHalf2, FMT6_16_16_FLOAT, WZYX, 4, true, "Half2"},   // fd6_format_table.c:155
    {VertexFormat::kHalf3, FMT6_16_16_16_FLOAT, WZYX, 6, true, "Half3"},   // fd6_format_table.c:236
    {VertexFormat::kHalf4, FMT6_16_16_16_16_FLOAT, WZYX, 8, true, "Half4"},   // fd6_format_table.c:249
    {VertexFormat::kFloat, FMT6_32_FLOAT, WZYX, 4, true, "Float"},   // fd6_format_table.c:136
    {VertexFormat::kFloat2, FMT6_32_32_FLOAT, WZYX, 8, true, "Float2"},   // fd6_format_table.c:258
    {VertexFormat::kFloat3, FMT6_32_32_32_FLOAT, WZYX, 12, true, "Float3"},   // fd6_format_table.c:272
    {VertexFormat::kFloat4, FMT6_32_32_32_32_FLOAT, WZYX, 16, true, "Float4"},   // fd6_format_table.c:284
    {VertexFormat::kInt, FMT6_32_SINT, WZYX, 4, false, "Int"},   // fd6_format_table.c:133
    {VertexFormat::kInt2, FMT6_32_32_SINT, WZYX, 8, false, "Int2"},   // fd6_format_table.c:255
    {VertexFormat::kInt3, FMT6_32_32_32_SINT, WZYX, 12, false, "Int3"},   // fd6_format_table.c:269
    {VertexFormat::kInt4, FMT6_32_32_32_32_SINT, WZYX, 16, false, "Int4"},   // fd6_format_table.c:280
    {VertexFormat::kUInt, FMT6_32_UINT, WZYX, 4, false, "UInt"},   // fd6_format_table.c:132
    {VertexFormat::kUInt2, FMT6_32_32_UINT, WZYX, 8, false, "UInt2"},   // fd6_format_table.c:254
    {VertexFormat::kUInt3, FMT6_32_32_32_UINT, WZYX, 12, false, "UInt3"},   // fd6_format_table.c:268
    {VertexFormat::kUInt4, FMT6_32_32_32_32_UINT, WZYX, 16, false, "UInt4"},   // fd6_format_table.c:278
    {VertexFormat::kUChar4Normalized_BGRA, FMT6_8_8_8_8_UNORM, WXYZ, 4, true, "UChar4Normalized_BGRA"},   // fd6_format_table.c:174
    {VertexFormat::kUChar, FMT6_8_UINT, WZYX, 1, false, "UChar"},   // fd6_format_table.c:46
    {VertexFormat::kChar, FMT6_8_SINT, WZYX, 1, false, "Char"},   // fd6_format_table.c:47
    {VertexFormat::kUCharNormalized, FMT6_8_UNORM, WZYX, 1, true, "UCharNormalized"},   // fd6_format_table.c:44
    {VertexFormat::kCharNormalized, FMT6_8_SNORM, WZYX, 1, true, "CharNormalized"},   // fd6_format_table.c:45
    {VertexFormat::kUShort, FMT6_16_UINT, WZYX, 2, false, "UShort"},   // fd6_format_table.c:72
    {VertexFormat::kShort, FMT6_16_SINT, WZYX, 2, false, "Short"},   // fd6_format_table.c:73
    {VertexFormat::kUShortNormalized, FMT6_16_UNORM, WZYX, 2, true, "UShortNormalized"},   // fd6_format_table.c:70
    {VertexFormat::kShortNormalized, FMT6_16_SNORM, WZYX, 2, true, "ShortNormalized"},   // fd6_format_table.c:71
    {VertexFormat::kHalf, FMT6_16_FLOAT, WZYX, 2, true, "Half"},   // fd6_format_table.c:76
};
const VertexFormatInfo* vertexFormatInfo(VertexFormat f) {
    for (const VertexFormatInfo& i : kVertexFormats) {
        if (i.format == f) return &i;
    }
    return nullptr;
}

// What the vertex fetch registers get: decode entry n per [[attribute(n)]] up to the highest one the VS reads (holes
// zero, as turnip writes them), fetch entry b per Metal buffer index used.
struct VertexFetch {
    uint32_t count = 0;            // VFD_CONTROL_0.FETCH_CNT = DECODE_CNT (turnip writes both as the attribute count)
    uint32_t decode[kMaxVertexAttributes] = {};
    uint32_t stepRate[kMaxVertexAttributes] = {};
    uint32_t dest[kMaxVertexAttributes] = {};
    bool used[kMaxVertexBuffers] = {};
    uint64_t base[kMaxVertexBuffers] = {};
    uint32_t size[kMaxVertexBuffers] = {};
    uint32_t stride[kMaxVertexBuffers] = {};
};

// The VS's attributes against the descriptor and the VS's own [[buffer(n)]] table. Refused by name: an attribute the
// descriptor does not describe, a format not served, an offset or stride past the 12-bit fields (a6xx.xml:4620-4634),
// a step other than per vertex, a stride of 0, no buffer bound at the layout's index, and a fetch that reads past the
// bound length for the draw's last vertex (Metal requires the bytes to be there; the GPU would read whatever follows).
bool vertexFetch(const Stage& vs, const VertexDescriptor& vd, const Draw& draw, const Bindings& b, VertexFetch* f, std::string* why) {
    const std::string name = stageName(vs);
    for (uint32_t i = 0; i < vs.numInputs; i++) {
        const StageInput& in = vs.inputs[i];
        if (in.sysval || in.regid == kRegidNone) continue;
        if (in.slot < kVertAttribGeneric0 || in.slot - kVertAttribGeneric0 >= kMaxVertexAttributes) {
            return refuse(why, name + ": an input at slot " + n(in.slot) + " that is not a [[stage_in]] attribute");
        }
        const uint32_t loc = in.slot - kVertAttribGeneric0;
        const VertexAttribute& a = vd.attributes[loc];
        if (a.format == VertexFormat::kInvalid) {
            return refuse(why, name + " reads [[attribute(" + n(loc) + ")]], which the vertex descriptor does not describe");
        }
        const VertexFormatInfo* fi = vertexFormatInfo(a.format);
        if (fi == nullptr) {
            return refuse(why, name + ": [[attribute(" + n(loc) + ")]] is MTLVertexFormat " + n(uint32_t(a.format)) +
                                   ": the 1010102, RG11B10 and RGB9E5 formats are not served");
        }
        if (a.bufferIndex >= kMaxVertexBuffers) {
            return refuse(why, name + ": [[attribute(" + n(loc) + ")]] is fetched from buffer " + n(a.bufferIndex) + ": the layouts are 0 to 30");
        }
        if (a.offset > 0xfff) {
            return refuse(why, name + ": [[attribute(" + n(loc) + ")]] at offset " + n(a.offset) + ": VFD_DECODE_INSTR.OFFSET holds 12 bits");
        }
        const VertexLayout& l = vd.layouts[a.bufferIndex];
        const bool perInstance = l.step == VertexStep::kPerInstance;
        if (l.step != VertexStep::kPerVertex && !perInstance) {
            return refuse(why, name + ": buffer " + n(a.bufferIndex) + " steps " + (l.step == VertexStep::kConstant ? "constantly" : "in an unknown way") +
                                   ": per-vertex and per-instance fetch are served");
        }
        if (perInstance && l.stepRate == 0) {
            return refuse(why, name + ": buffer " + n(a.bufferIndex) + " steps per instance at a rate of 0: Metal's step rate is 1 or more");
        }
        if (l.stride == 0 || l.stride > 0xfff) {
            return refuse(why, name + ": buffer " + n(a.bufferIndex) + "'s stride of " + n(l.stride) + ": 1 to 4095 bytes (VFD_FETCH_STRIDE holds 12 bits)");
        }
        if (a.bufferIndex >= b.buffers.size() || b.buffers[a.bufferIndex].buffer == nullptr) {
            return refuse(why, name + ": no buffer bound at index " + n(a.bufferIndex) + " (the vertex buffer of [[attribute(" + n(loc) + ")]])");
        }
        const Binding& x = b.buffers[a.bufferIndex];
        if (x.offset > x.buffer->length()) {
            return refuse(why, name + ": buffer " + n(a.bufferIndex) + ": offset " + n(x.offset) + " lies past its end");
        }
        const uint64_t bytes = x.buffer->length() - x.offset;
        // the last element the draw fetches: for a per-instance attribute baseInstance + (instanceCount - 1) / stepRate
        // (VFD_INSTANCE_START_OFFSET is added after the divisor, as Vulkan's firstInstance is); for a per-vertex
        // one the last vertex, which an indexed draw names only in its index buffer -- there the check is the GPU's own
        // (VFD_FETCH_SIZE bounds the fetch: an index past it reads zeros, robust-buffer style, not what follows)
        // (an indirect draw's counts are the GPU's to read: every attribute is bounded by the fetch size alone, as above)
        if ((!perInstance && draw.indexBuffer != nullptr) || draw.indirect != nullptr) {
            f->decode[loc] = vfdDecodeInstr(a.bufferIndex, a.offset, perInstance, fi->fmt6, fi->swap, fi->isFloat);
            f->stepRate[loc] = perInstance ? l.stepRate : 0;
        } else {
            const uint64_t last = perInstance ? uint64_t(draw.baseInstance) + uint64_t(draw.instanceCount == 0 ? 0 : draw.instanceCount - 1) / l.stepRate
                                              : uint64_t(draw.vertexStart) + (draw.vertexCount == 0 ? 0 : draw.vertexCount - 1);
            const uint64_t need = last * l.stride + a.offset + fi->bytes;
            if (need > bytes) {
                return refuse(why, name + ": [[attribute(" + n(loc) + ")]] of " + (perInstance ? "instance step " : "vertex ") + n(uint32_t(last)) + " ends at byte " +
                                       n(need) + " of buffer " + n(a.bufferIndex) + "; " + n(bytes) + " are bound");
            }
            // per instance: INSTANCED and the divisor in VFD_DECODE_STEP_RATE (tu6_emit_vertex_input, tu_pipeline.cc:2345-2354);
            // per vertex: turnip's divisor 0, unread when not INSTANCED (fd6 writes 1, fd6_context.cc:97-98)
            f->decode[loc] = vfdDecodeInstr(a.bufferIndex, a.offset, perInstance, fi->fmt6, fi->swap, fi->isFloat);
            f->stepRate[loc] = perInstance ? l.stepRate : 0;
        }
        f->dest[loc] = vfdDestCntlInstr(in.compmask, in.regid);
        f->count = std::max(f->count, loc + 1);
        f->used[a.bufferIndex] = true;
        f->base[a.bufferIndex] = x.buffer->gpuAddress() + x.offset;
        f->size[a.bufferIndex] = uint32_t(std::min<uint64_t>(bytes, 0xffffffffu));
        f->stride[a.bufferIndex] = l.stride;
    }
    // the unused decode entries below the count: VFD_DEST_CNTL_INSTR writes no register (turnip :1606-1610)
    for (uint32_t loc = 0; loc < f->count; loc++) {
        if (f->decode[loc] == 0) f->dest[loc] = vfdDestCntlInstr(0, kRegidNone);
    }
    return true;
}

bool finite(double v) { return std::isfinite(v); }

// float to IEEE half, rounding to nearest even as _mesa_float_to_half does (r2d_clear_value's R2D_FLOAT16 branch)
uint32_t halfOf(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t absx = x & 0x7fffffffu;
    if (absx > 0x7f800000u) return sign | 0x7e00u;            // NaN: a quiet one
    if (absx >= 0x477ff000u) return sign | 0x7c00u;           // rounds to or past 65520: infinity
    if (absx < 0x38800000u) {                                 // below the smallest normal half: a subnormal or 0
        float a;
        memcpy(&a, &absx, 4);
        return sign | uint32_t(std::nearbyint(double(a) * 16777216.0));   // in units of 2^-24, round-half-even
    }
    const uint32_t mant = absx & 0x007fffffu;
    uint32_t h = ((absx >> 23) - 112u) << 10 | (mant >> 13);
    const uint32_t rest = mant & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) h++;
    return sign | h;
}

// util_format_linear_to_srgb_float (u_format_srgb.h), in float as Mesa computes it
float linearToSrgb(float c) {
    if (!(c > 0.0f)) return 0.0f;
    if (c < 0.0031308f) return 12.92f * c;
    if (c < 1.0f) return 1.055f * std::pow(c, 0.41666f) - 0.055f;
    return 1.0f;
}

// tu_pack_float32_for_unorm(v, 8): _mesa_lroundevenf(CLAMP(v, 0, 1) * 255)
uint32_t unorm8Of(float v) {
    float c = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
    return uint32_t(std::nearbyint(c * 255.0f));
}

// What a Clear writes: the 2D engine's intermediate format and RB_2D_SRC_SOLID_C0..3, the clear colour packed per
// channel as r2d_clear_value packs it (tu_clear_blit.cc:166-231). The channels a format does not have stay 0 (the
// swizzle > W skip), RGBA order whatever the memory order: RB_2D_DST_INFO's swap places them. Metal hands the colour
// as doubles; an integer target takes it as integers, which must be whole and in range to mean anything.
struct ClearWords {
    uint32_t ifmt;
    uint32_t value[4];
};

uint32_t channelsOf(const FormatInfo& f) {
    switch (f.fmt6) {
    case FMT6_8_UNORM: case FMT6_16_FLOAT: case FMT6_32_UINT: case FMT6_32_FLOAT: case FMT6_16_UNORM: case FMT6_16_UINT: case FMT6_16_SINT: return 1;
    case FMT6_8_8_UNORM: case FMT6_16_16_FLOAT: case FMT6_32_32_FLOAT: return 2;
    case FMT6_5_6_5_UNORM: return 3;
    default: return 4;
    }
}

bool clearWords(const FormatInfo& f, const double colour[4], ClearWords* out, std::string* why) {
    for (int i = 0; i < 4; i++) {
        if (!std::isfinite(colour[i])) {
            return refuse(why, std::string("a clear colour with a non-finite channel (") + "channel " + n(uint32_t(i)) + ") into " + f.name);
        }
    }
    *out = ClearWords{};
    const uint32_t channels = channelsOf(f);
    switch (f.fmt6) {
    case FMT6_A8_UNORM:
        // the one channel is alpha (A8's swizzle is 0, 0, 0, X: r2d_clear_value fills only the channels a swizzle names)
        out->ifmt = R2D_UNORM8;
        out->value[3] = unorm8Of(float(colour[3]));
        return true;
    // (5- and 6-bit channels go through the 2D engine's 8-bit unorm too: turnip's format_to_ifmt picks by the red
    // channel's size, 4, 5 and 8 alike, tu_clear_blit.cc:55-59)
    case FMT6_8_UNORM: case FMT6_8_8_UNORM: case FMT6_8_8_8_8_UNORM: case FMT6_5_6_5_UNORM: case FMT6_5_5_5_1_UNORM:
        out->ifmt = f.srgb ? R2D_UNORM8_SRGB : R2D_UNORM8;
        for (uint32_t i = 0; i < channels; i++) {
            const float linear = float(colour[i]);
            out->value[i] = unorm8Of(f.srgb && i < 3 ? linearToSrgb(linear) : linear);
        }
        return true;
    case FMT6_16_FLOAT: case FMT6_16_16_FLOAT: case FMT6_16_16_16_16_FLOAT:
        out->ifmt = R2D_FLOAT16;
        for (uint32_t i = 0; i < channels; i++) out->value[i] = halfOf(float(colour[i]));
        return true;
    case FMT6_32_FLOAT: case FMT6_32_32_FLOAT: case FMT6_32_32_32_32_FLOAT:
        out->ifmt = R2D_FLOAT32;
        for (uint32_t i = 0; i < channels; i++) out->value[i] = fui(float(colour[i]));
        return true;
    case FMT6_32_UINT: case FMT6_32_32_32_32_UINT:
        out->ifmt = R2D_INT32;
        for (uint32_t i = 0; i < channels; i++) {
            const double c = colour[i];
            if (c < 0.0 || c > 4294967295.0 || c != std::floor(c)) {
                return refuse(why, "a clear colour of " + std::to_string(c) + " in channel " + n(i) + " of " + f.name + ": an integer target takes whole numbers 0 to 4294967295");
            }
            out->value[i] = uint32_t(c);
        }
        return true;
    default:
        return refuse(why, std::string("a Clear of ") + f.name + ": the 2D engine's intermediate format for it is not worked out here");
    }
}

// The FS's texture and sampler tables, Gallium's FS_TEX group (fd6_texture.cc:575-687 with SB6_FS_TEX, SP_FS_TEX_*):
// the descriptors of the sampled textures at their Metal indices (64 bytes each; a slot the FS does not sample a zero
// descriptor, Gallium's dummy_view), and at least one sampler descriptor whenever a texture is loaded (16 bytes each, a
// slot without a sampler zeros, dummy_sampler) -- compute's rule (compute.cpp), the table in the pass's kTableFsAt.
// Every prefetch's slots must name a loaded texture and a bound sampler: the prefetch reads them before the FS's
// first instruction, and a slot past the count would be a stale descriptor.
constexpr uint32_t kFsTexConstAt = kTableFsAt;                       // kMaxTextureSlots * 64 = 1024 bytes, 64-aligned
constexpr uint32_t kFsTexSampAt = kTableFsAt + kMaxTextureSlots * 64;  // kMaxTextureSlots * 16 = 256 bytes
// The VS's, the same group into SB6_VS_TEX by CP_LOAD_STATE6_GEOM with SP_VS_TEX_* (fd6_texture.cc's VS_TEX): past the
// FS's, as the VS's own part of the table is the 3D IBO table's (kGfxIboAt) and full with it.
constexpr uint32_t kVsTexConstAt = kFsTexSampAt + kMaxTextureSlots * 16;
constexpr uint32_t kVsTexSampAt = kVsTexConstAt + kMaxTextureSlots * 64;
static_assert(kVsTexConstAt % 64 == 0 && kVsTexSampAt + kMaxTextureSlots * 16 <= kTableFsAt + 4096, "the two stages' tables fit the FS's part of the pass's table");

struct StageTextures {
    uint32_t numTex = 0, numSamp = 0;
    uint32_t texWords[kMaxTextureSlots][16] = {};
    uint32_t sampWords[kMaxTextureSlots][4] = {};
    std::vector<const Texture*> used;
};

const char* elementOf(uint8_t e) { return e == kElemFloat ? "float" : e == kElemUint ? "uint" : e == kElemInt ? "int" : "?"; }

// colour attachment I of a pass in one shape (attachment 0 is the pass's own fields, 1 to 7 its `more`)
struct Attachment {
    const Texture* texture;
    uint32_t level;
    LoadAction load;
    StoreAction store;
    const double* clearColor;
    uint32_t slice = 0;   // the layer, face or depth plane: its memory is that many slices on (Texture::sliceBytes)
};
Attachment attachmentOf(const RenderPass& p, uint32_t i);

// The pass's window: the smallest of its colour attachments at their levels, as Metal's render target size is when they
// differ -- QuartzCore's variable blur draws into a 576 x 80 target beside a 1152 x 160 one it does not write. Each
// attachment keeps its own pitch and base; the window scissor keeps every write inside the smallest.
std::pair<uint32_t, uint32_t> windowOf(const RenderPass& pass) {
    uint32_t w = ~0u, h = ~0u;
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        const Attachment a = attachmentOf(pass, i);
        if (a.texture == nullptr || a.level >= a.texture->levels()) continue;
        const TextureLevel L = a.texture->level(a.level);
        w = std::min(w, L.width);
        h = std::min(h, L.height);
    }
    // a pass of depth alone (a shadow map's) is the depth attachment's size; beside colour it is the same size (checkTarget)
    if (pass.depth != nullptr) {
        w = std::min(w, pass.depth->level(pass.depthLevel).width);
        h = std::min(h, pass.depth->level(pass.depthLevel).height);
    }
    return {w, h};
}

// the first attachment the pass has, colour before depth: what a draw's defaults (viewport, the LRZ's format) take
const Texture* firstAttachment(const RenderPass& pass) {
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        if (attachmentOf(pass, i).texture != nullptr) return attachmentOf(pass, i).texture;
    }
    return pass.depth;
}

Attachment attachmentOf(const RenderPass& p, uint32_t i) {
    if (i == 0) return Attachment{p.color0, p.level, p.load, p.store, p.clearColor, p.slice};
    const ColorAttachment& c = p.more[i - 1];
    return Attachment{c.texture, c.level, c.load, c.store, c.clearColor, c.slice};
}

bool stageTextures(const Stage& fs, const Bindings& b, const RenderPass& pass, StageTextures* out, std::string* why) {
    const Kernel& k = fs.k;
    const std::string name = stageName(fs);
    out->numTex = k.numTextureSlots;
    if (out->numTex == 0) {
        if (fs.numSamplerPrefetch != 0) {
            return refuse(why, name + " prefetches a texture and loads none: ir3's tables disagree");
        }
        return true;
    }
    for (uint32_t t = 0; t < out->numTex; t++) {
        const KernelTexture& kt = k.textures[t];
        if (kt.kind != kTexSampled) continue;
        if (kt.fbTarget != 0) {
            // a framebuffer fetch's slot (compiler.cpp: lowerFbRead): the attachment it reads, at the level the pass draws
            const Attachment a = attachmentOf(pass, kt.fbTarget - 1u);
            if (a.texture == nullptr) {
                return refuse(why, name + " reads [[color(" + n(kt.fbTarget - 1u) + ")]] through framebuffer fetch and the pass has no colour attachment " +
                                       n(kt.fbTarget - 1u));
            }
            if (a.slice != 0) {
                return refuse(why, name + " reads [[color(" + n(kt.fbTarget - 1u) + ")]] through framebuffer fetch from slice " + n(a.slice) +
                                       ": a fetch from a layer, face or plane past the first is not served");
            }
            const TexConst d = a.texture->levelDescriptor(a.level);
            memcpy(out->texWords[t], d.dword, sizeof d.dword);
            continue;
        }
        if (t >= b.textures.size() || b.textures[t] == nullptr) {
            return refuse(why, name + ": no texture bound at index " + n(t));
        }
        const Texture* tex = b.textures[t];
        if (kt.isNull || ((b.nullMask >> t) & 1) != 0) {
            // The null texture's slot, bound by the draw (CommandQueue::nullTexture), and sampled in whatever dimension
            // and class the shader asked for it in -- fixed_frag_lph_cph asks as 2D and as 3D through the one slot, and
            // the table has no 17th for a second descriptor. One 2D 1x1 descriptor serves both: on the phone a 3D sample
            // through it reads its one texel as the 2D sample does (probe 9s), so whatever is in that texel is what both read.
            TexConst d = tex->sampledDescriptor();
            memcpy(out->texWords[t], d.dword, sizeof d.dword);
            out->used.push_back(tex);
            continue;
        }
        if (!tex->isShaped(kt)) {
            return refuse(why, name + " samples texture " + n(t) + " as " +
                                   (kt.dim == kTexDim1D ? "1D" : kt.dim == kTexDim2D ? "2D" : kt.dim == kTexDim3D ? "3D" : kt.dim == kTexDimCube ? "a cube" : kt.dim == kTexDimMS ? "2D multisample" : "another kind") +
                                   (kt.isArray ? " array" : "") + " and texture " + n(t) + " is of type " + n(uint32_t(tex->type())) +
                                   ": 1D, 2D, 2D array, 2D multisample, cube and 3D, each as itself, are served in a draw");
        }
        if (kt.element != tex->info().element) {
            return refuse(why, "texture " + n(t) + " is " + tex->info().name + " (" + elementOf(tex->info().element) + "); " + name + " reads it as texture2d<" +
                                   elementOf(kt.element) + ">");
        }
        if ((tex->usage() & kTextureUsageRead) == 0) {
            return refuse(why, "texture " + n(t) + " was made without shader-read usage; " + name + " samples it");
        }
        TexConst d = tex->sampledDescriptor();
        memcpy(out->texWords[t], d.dword, sizeof d.dword);
        out->used.push_back(tex);
    }
    out->numSamp = 1;
    for (uint32_t m = 0; m < kMaxTextureSlots; m++) {
        if ((k.samplerMask >> m) & 1) out->numSamp = m + 1;
    }
    for (uint32_t m = 0; m < out->numSamp; m++) {
        if (((k.samplerMask >> m) & 1) == 0) continue;
        if (m >= b.samplers.size() || b.samplers[m] == nullptr) {
            // nothing bound: Metal leaves a sample through it undefined, and an uber shader declares samplers a draw does
            // not reach. All-zero words are a valid sampler (nearest, repeat), so what such a slot reads is defined here.
            memset(out->sampWords[m], 0, 16);
            continue;
        }
        memcpy(out->sampWords[m], b.samplers[m]->words, 16);
    }
    if (fs.numSamplerPrefetch > kMaxSamplerPrefetch) {
        return refuse(why, name + " has " + n(fs.numSamplerPrefetch) + " texture prefetches: ir3 makes at most " + n(kMaxSamplerPrefetch));
    }
    for (uint32_t i = 0; i < fs.numSamplerPrefetch; i++) {
        const SamplerPrefetch& pf = fs.prefetch[i];
        if (pf.bindless != 0) {
            return refuse(why, name + "'s prefetch " + n(i) + " is bindless: the textures here are SB6_FS_TEX tables");
        }
        if (pf.cmd != TEX_PREFETCH_SAM) {
            return refuse(why, name + "'s prefetch " + n(i) + " is not a plain sample (TEX_PREFETCH_SAM): no other prefetch is written here");
        }
        if (pf.texId >= out->numTex || k.textures[pf.texId].kind != kTexSampled || ((k.samplerMask >> pf.sampId) & 1) == 0) {
            return refuse(why, name + "'s prefetch " + n(i) + " samples texture " + n(pf.texId) + " with sampler " + n(pf.sampId) +
                                   ": not a texture and a sampler the FS declares");
        }
    }
    return true;
}

// The UBO descriptors a stage's ldc reads through: one per UBO ir3 counts (num_ubos, the highest constant buffer index
// it addresses plus one; the AIR front end numbers UBOs by Metal's [[buffer(n)]]), the bound buffer past its offset and
// its length in vec4, as fd6_emit_ubos writes them (fd6_const.cc:284-295). ir3 counts num_ubos as one past the highest
// block an ldc still reads (track_ubo_use, ir3_nir_analyze_ubo_ranges.c:252-263), so that one must be bound; an index
// below it may be one no ldc reads (QuartzCore's path_blit_vert_lph reads 1 and 2 and leaves 0 unbound), and an
// unbound one there gets a descriptor of size 0, which the ldc's bound check turns into zeros, rather than whatever
// the block held.
bool uboDescriptors(const Stage& s, const Bindings& b, uint64_t program, std::vector<uint32_t>* out, std::string* why) {
    const std::string name = stageName(s);
    out->clear();
    for (uint32_t u = 0; u < s.k.numUbos; u++) {
        if (s.k.constantDataUbo != 0 && u == s.k.constantDataUbo - 1) {
            // the constant data, where the program's words were uploaded (they carry it at constantDataOffset)
            uint32_t d[2];
            uboDescriptor(program + s.k.constantDataOffset, (s.k.constantDataSize + 15) / 16, d);
            out->push_back(d[0]);
            out->push_back(d[1]);
            continue;
        }
        if (u >= b.buffers.size() || b.buffers[u].buffer == nullptr) {
            if (u + 1 == s.k.numUbos) {
                return refuse(why, name + ": no buffer bound at index " + n(u) + " (a constant buffer it indexes)");
            }
            out->push_back(0);
            out->push_back(0);
            continue;
        }
        const Binding& x = b.buffers[u];
        // a multiple of 4, Metal's rule for a constant buffer's offset on iOS: probe 9o2 measured the ldc reading from
        // the descriptor's address as given at offsets 4, 8 and 120, not rounded to a vec4
        if (x.offset % 4 != 0 || x.offset > x.buffer->length()) {
            return refuse(why, name + ": buffer " + n(u) + ": offset " + n(x.offset) + " is not a multiple of 4 inside it");
        }
        const uint64_t vec4s = (x.buffer->length() - x.offset + 15) / 16;
        uint32_t d[2];
        uboDescriptor(x.buffer->gpuAddress() + x.offset, uint32_t(std::min<uint64_t>(vec4s, kUboMaxVec4)), d);
        out->push_back(d[0]);
        out->push_back(d[1]);
    }
    return true;
}

// The 3D pipeline's IBO table: ONE for every graphics stage (SP_IBO, loaded into SB6_IBO; turnip loads "one packet for
// all graphics stages", tu_pipeline.cc:180-185), so a device buffer the VS and the FS both address at a Metal index must
// be the same binding in both. Each slot below the stages' SSBO counts is a buffer view over the bound buffer past its
// offset (compute's bufferIbo, 32-bit elements), every one of them bound; the table sits in the pass's kTableVsAt part.
constexpr uint32_t kGfxIboAt = kTableVsAt;
constexpr uint32_t kMaxGfxIbos = (kTableFsAt - kTableVsAt) / 64;
static_assert(kMaxGfxIbos >= kMaxVertexBuffers, "the table holds an IBO per Metal buffer index");

struct GfxIbos {
    uint32_t count = 0;
    uint32_t words[kMaxVertexBuffers][16] = {};
    std::vector<const Buffer*> used;
};

bool gfxIbos(const Stage& vs, const Bindings& vsb, const Stage& fs, const Bindings& fsb, uint32_t fsBase, GfxIbos* out, std::string* why) {
    const Binding* chosen[kMaxVertexBuffers] = {};
    for (const auto& [s, b] : {std::pair<const Stage*, const Bindings*>{&vs, &vsb}, {&fs, &fsb}}) {
        // the FS was compiled with its [[buffer(n)]] at slot FS_BASE + n (Program::fsIboBase): the slots under it are
        // the VS's, and its own index is the slot less the base
        const uint32_t base = s == &fs ? fsBase : 0;
        for (uint32_t tableSlot = base; tableSlot < s->k.numSsbos; tableSlot++) {
            const uint32_t slot = tableSlot - base;
            if (slot >= b->buffers.size() || b->buffers[slot].buffer == nullptr) {
                // ir3 counts SSBOs by the highest one the function declares, not by the ones it reaches
                // (shader_info.num_ssbos), so an unbound slot may be one no instruction reads, as mask_color_fragment's
                // 1 is. The one that cannot be told from a read is the highest, refused as before; one below it gets
                // turnip's null descriptor (all zeros, tu_descriptor_set.cc's nullDescriptor), which a load reads as 0
                // and a store leaves alone, never another binding's memory.
                if (tableSlot + 1 == s->k.numSsbos) {
                    return refuse(why, std::string(stageName(*s)) + ": no buffer bound at index " + n(slot) + " (a device buffer it addresses)");
                }
                out->count = std::max(out->count, tableSlot + 1);
                continue;
            }
            const Binding& x = b->buffers[slot];
            if (x.offset % 4 != 0 || x.offset >= x.buffer->length()) {
                return refuse(why, std::string(stageName(*s)) + ": buffer " + n(slot) + ": offset " + n(x.offset) + " is not a multiple of 4 inside it");
            }
            if (chosen[tableSlot] != nullptr && (chosen[tableSlot]->buffer != x.buffer || chosen[tableSlot]->offset != x.offset)) {
                return refuse(why, "the vertex and fragment stages bind different device buffers at table slot " + n(tableSlot) +
                                       ": the 3D pipeline has one IBO table for both");
            }
            if (chosen[tableSlot] == nullptr) {
                chosen[tableSlot] = &x;
                const uint64_t elements = (x.buffer->length() - x.offset) / 4;
                if (elements >= (1ull << 30)) {
                    return refuse(why, "buffer " + n(slot) + " is longer than a descriptor counts");
                }
                TexConst d = bufferIbo(x.buffer->gpuAddress() + x.offset, uint32_t(elements));
                memcpy(out->words[tableSlot], d.dword, sizeof d.dword);
                out->used.push_back(x.buffer);
            }
            out->count = std::max(out->count, tableSlot + 1);
        }
    }
    return true;
}

// Colour attachment 0's blend as RB_MRT_CONTROL[0] and RB_MRT_BLEND_CONTROL[0] hold it (tu6_emit_blend,
// tu_pipeline.cc:2975-3063): MTLBlendOperation's numbers are VkBlendOp's, which turnip passes through as the opcode
// (tu6_blend_op, tu_util.h:288-291); MTLBlendFactor goes through the table below onto adreno_rb_blend_factor, the
// way tu6_blend_factor maps Vulkan's (tu_util.h:246-271); MTLColorWriteMask's bits run Red 8 .. Alpha 1, the
// component enable's Red 1 .. Alpha 8.
struct BlendWords {
    bool enabled;
    uint32_t mrtControl, mrtBlendControl;
};

bool blendWords(const ColorBlend& b, const FormatInfo& f, BlendWords* out, std::string* why) {
    static const adreno_rb_blend_factor kFactor[] = {
        FACTOR_ZERO, FACTOR_ONE, FACTOR_SRC_COLOR, FACTOR_ONE_MINUS_SRC_COLOR, FACTOR_SRC_ALPHA, FACTOR_ONE_MINUS_SRC_ALPHA,
        FACTOR_DST_COLOR, FACTOR_ONE_MINUS_DST_COLOR, FACTOR_DST_ALPHA, FACTOR_ONE_MINUS_DST_ALPHA, FACTOR_SRC_ALPHA_SATURATE,
        FACTOR_CONSTANT_COLOR, FACTOR_ONE_MINUS_CONSTANT_COLOR, FACTOR_CONSTANT_ALPHA, FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    };
    if (b.writeMask > 0xf) {
        return refuse(why, "a colour write mask of " + n(b.writeMask) + ": MTLColorWriteMask has the four bits Red 8, Green 4, Blue 2, Alpha 1");
    }
    uint32_t components = 0;
    for (uint32_t c = 0; c < 4; c++) {
        if (b.writeMask & (8u >> c)) components |= 1u << c;
    }
    *out = BlendWords{};
    out->enabled = b.enabled;
    out->mrtControl = rbMrtControl(components);
    if (!b.enabled) {
        return true;   // RB_MRT_BLEND_CONTROL 0 as turnip writes it for an attachment that does not blend
    }
    if (f.element != kElemFloat) {
        return refuse(why, std::string("blending into ") + f.name + ": an integer format has no blend (Metal refuses the pipeline too)");
    }
    const uint32_t factors[4] = {b.srcRgb, b.dstRgb, b.srcAlpha, b.dstAlpha};
    static const char* const kWhich[4] = {"source RGB", "destination RGB", "source alpha", "destination alpha"};
    for (int i = 0; i < 4; i++) {
        if (factors[i] >= 15 && factors[i] <= 18) {
            return refuse(why, std::string("the ") + kWhich[i] + " blend factor " + n(factors[i]) + " reads a second source colour: dual-source blending is not served");
        }
        if (factors[i] > 18) {
            return refuse(why, std::string("the ") + kWhich[i] + " blend factor " + n(factors[i]) + ": MTLBlendFactor runs 0 to 18");
        }
    }
    if (b.rgbOp > 4 || b.alphaOp > 4) {
        return refuse(why, "blend operation " + n(b.rgbOp > 4 ? b.rgbOp : b.alphaOp) + ": MTLBlendOperation runs Add 0 to Max 4");
    }
    out->mrtControl |= A6XX_RB_MRT_CONTROL_BLEND | A6XX_RB_MRT_CONTROL_BLEND2;
    out->mrtBlendControl = A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(kFactor[b.srcRgb]) |
                           A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE(a3xx_rb_blend_opcode(b.rgbOp)) |
                           A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR(kFactor[b.dstRgb]) |
                           A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR(kFactor[b.srcAlpha]) |
                           A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE(a3xx_rb_blend_opcode(b.alphaOp)) |
                           A6XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR(kFactor[b.dstAlpha]);
    return true;
}

// The fill itself, between the pass begin and the draw state (clear_sysmem_attachment, tu_clear_blit.cc:4402-4444,
// with r2d_setup_common :455-521, r2d_coords :145-163, r2d_clear_value, r2d_dst :396-413, r2d_run :551-570): the
// render area is the whole target, as a Metal Clear covers the attachment whatever the scissor. Then what
// tu_clear_sysmem_attachment does after it (:4480-4489): the colour CCU cleaned (PC_CCU_FLUSH_COLOR_TS in the
// 4-word form, waited for, as the tail does it) and invalidated, a WFI -- so that the draw's colour writes, which go
// through the same CCU, start after the fill landed.
void fillStream(pm4::Stream& stream, const Texture& target, const ClearWords& c, a6xx_tile_mode tile, uint32_t level = 0, uint32_t slice = 0) {
    const FormatInfo& f = target.info();
    const TextureLevel L = target.level(level);
    // (a multisample target is filled as the wider texture its memory is: every sample of every pixel the colour. A tiled
    // one -- a multisample depth -- whole rows of its pitch and its rows to 16: the fill is one value, so covering every
    // byte of the layout is right however its tiles order the samples)
    const bool wholeLayout = tile != TILE6_LINEAR && target.samples() > 1;
    const uint32_t W = wholeLayout ? uint32_t(L.pitch / f.bytesPerPixel) : L.width * target.samples();
    const uint32_t H = wholeLayout ? (L.height + 15) / 16 * 16 : L.height;
    const uint64_t at = target.levelAddress(level) + (slice != 0 ? uint64_t(slice) * target.sliceBytes() : 0);
    const uint32_t blit = rb2dBlitCntlClear(f.fmt6, c.ifmt);
    reg(stream, REG_A6XX_RB_2D_UNKNOWN_8C01, 0);
    reg(stream, REG_A6XX_RB_2D_BLIT_CNTL, blit);
    reg(stream, REG_A6XX_GRAS_2D_BLIT_CNTL, blit);
    reg(stream, REG_A6XX_SP_2D_DST_FORMAT, sp2dDstFormat(f.fmt6, f.element == kElemInt, f.element == kElemUint, f.srgb));
    regs(stream, REG_A6XX_GRAS_2D_DST_TL, {regXY(0, 0), regXY(W - 1, H - 1)});
    regs(stream, REG_A6XX_RB_2D_SRC_SOLID_C0, {c.value[0], c.value[1], c.value[2], c.value[3]});
    regs(stream, REG_A6XX_RB_2D_DST_INFO, {rb2dDstInfo(f.fmt6, tile, f.swap, f.srgb), uint32_t(at), uint32_t(at >> 32), A6XX_RB_2D_DST_PITCH(uint32_t(L.pitch))});
    regs(stream, REG_A6XX_RB_2D_DST_FLAGS, {0, 0, 0});
    command1(stream, CP_BLIT, cpBlit0(BLIT_OP_SCALE));
}

void clearStream(pm4::Stream& stream, const Texture& target, const ClearWords& c, uint64_t ts, uint32_t level = 0, uint32_t slice = 0) {
    fillStream(stream, target, c, TILE6_LINEAR, level, slice);
    eventTimestamp(stream, PC_CCU_FLUSH_COLOR_TS, ts, kClearFlushValue, false);
    waitForWord(stream, ts, kClearFlushValue);
    event(stream, PC_CCU_INVALIDATE_COLOR);
    stream.waitForIdle();
}

// A depth Clear: the same fill into the depth texture as Z32_FLOAT is cleared (r2d_clear_value's R2D_FLOAT32 branch:
// the depth's float bits in SOLID_C0; the 2D engine sees FMT6_32_FLOAT, TILE6_3 as the image is laid out), then what
// tu_clear_sysmem_attachment does after a depth clear (tu_clear_blit.cc:4480-4483): the colour CCU cleaned -- the 2D
// engine writes through it -- and the depth CCU cleaned and invalidated, so the draw's depth test reads the fill
void depthClearStream(pm4::Stream& stream, const Texture& depth, double clearDepth, uint64_t table, uint32_t level = 0) {
    ClearWords c{};
    c.ifmt = R2D_FLOAT32;
    c.value[0] = fui(float(clearDepth));
    fillStream(stream, depth, c, TILE6_3, level);
    eventTimestamp(stream, PC_CCU_FLUSH_COLOR_TS, table + kTableDepthClearFlushAt, kDepthClearFlushValue, false);
    waitForWord(stream, table + kTableDepthClearFlushAt, kDepthClearFlushValue);
    eventTimestamp(stream, PC_CCU_FLUSH_DEPTH_TS, table + kTableDepthClearDepthFlushAt, kDepthClearDepthFlushValue, false);
    waitForWord(stream, table + kTableDepthClearDepthFlushAt, kDepthClearDepthFlushValue);
    event(stream, PC_CCU_INVALIDATE_DEPTH);
    stream.waitForIdle();
}

// A stencil Clear: the S8 plane filled as an 8-bit integer image (r2d_clear_value's S8_UINT branch: the value in
// SOLID_C0, FMT6_8_UINT through R2D_INT8, TILE6_3 as the plane is laid out), then the colour CCU and the depth CCU
// cleaned -- the depth CCU holds stencil -- and the depth one invalidated, as depthClearStream does
void stencilClearStream(pm4::Stream& stream, const Texture& t, uint32_t value, uint64_t table) {
    const uint32_t W = t.width(), H = t.height();
    const uint32_t blit = rb2dBlitCntlClear(FMT6_8_UINT, R2D_INT8);
    const uint64_t at = t.gpuAddress() + t.stencilOffset();
    reg(stream, REG_A6XX_RB_2D_UNKNOWN_8C01, 0);
    reg(stream, REG_A6XX_RB_2D_BLIT_CNTL, blit);
    reg(stream, REG_A6XX_GRAS_2D_BLIT_CNTL, blit);
    reg(stream, REG_A6XX_SP_2D_DST_FORMAT, sp2dDstFormat(FMT6_8_UINT, false, true, false));
    regs(stream, REG_A6XX_GRAS_2D_DST_TL, {regXY(0, 0), regXY(W - 1, H - 1)});
    regs(stream, REG_A6XX_RB_2D_SRC_SOLID_C0, {value & 0xffu, 0, 0, 0});
    regs(stream, REG_A6XX_RB_2D_DST_INFO, {rb2dDstInfo(FMT6_8_UINT, TILE6_3, WZYX, false), uint32_t(at), uint32_t(at >> 32), A6XX_RB_2D_DST_PITCH(uint32_t(t.stencilPitch()))});
    regs(stream, REG_A6XX_RB_2D_DST_FLAGS, {0, 0, 0});
    command1(stream, CP_BLIT, cpBlit0(BLIT_OP_SCALE));
    eventTimestamp(stream, PC_CCU_FLUSH_COLOR_TS, table + kTableStencilClearFlushAt, kStencilClearFlushValue, false);
    waitForWord(stream, table + kTableStencilClearFlushAt, kStencilClearFlushValue);
    eventTimestamp(stream, PC_CCU_FLUSH_DEPTH_TS, table + kTableStencilClearDepthFlushAt, kStencilClearDepthFlushValue, false);
    waitForWord(stream, table + kTableStencilClearDepthFlushAt, kStencilClearDepthFlushValue);
    event(stream, PC_CCU_INVALIDATE_DEPTH);
    stream.waitForIdle();
}

// the depth attachment's Clear and the stencil attachment's, each when asked for (tu_clear_sysmem_attachment, each aspect in turn)
void zsClears(pm4::Stream& stream, const RenderPass& pass, uint64_t table) {
    if (pass.depth != nullptr && pass.depthLoad == LoadAction::kClear) {
        depthClearStream(stream, *pass.depth, pass.clearDepth, table, pass.depthLevel);
    }
    if (pass.stencil != nullptr && pass.stencilLoad == LoadAction::kClear) {
        stencilClearStream(stream, *pass.stencil, pass.clearStencil, table);
    }
}

// gl_frag_result / gl_varying_slot numbers the Stage tables carry (Mesa shader_enums.h:987): FRAG_RESULT_DATA0 is 4
constexpr uint8_t kFragResultData0 = 4;


// the table word attachment I's Clear waits on (render.hpp: kTableMoreClearFlushAt says why each has its own)
uint64_t clearFlushAt(uint64_t table, uint32_t i) { return table + (i == 0 ? kTableClearFlushAt : kTableMoreClearFlushAt + 4 * (i - 1)); }

// The colour targets and the pass's actions, for a draw and for a pass with no draw alike; the colour of a Clear is
// packed apart (clearWords), since only a Clear has one. Every attachment set is held to the same rules, and those past
// 0 to attachment 0's size: the window is one rectangle for all of them.
bool checkTarget(const RenderPass& pass, std::string* why) {
    if (firstAttachment(pass) == nullptr) {
        return refuse(why, "a render pass with no attachment at all: nothing to draw into");
    }
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        const Attachment a = attachmentOf(pass, i);
        if (a.texture == nullptr) continue;
        const Texture& t = *a.texture;
        const FormatInfo& f = t.info();
        // (attachment 0's sentences are the ones a single target always had; a later one's name its index)
        const std::string which = "colour attachment " + n(i), of = i == 0 ? std::string() : " of " + which, at = i == 0 ? std::string() : which + ": ";
        if ((t.usage() & kTextureUsageRenderTarget) == 0) {
            return refuse(why, "the " + n(t.width()) + " x " + n(t.height()) + " " + f.name + " texture" + of +
                                   " was made without render-target usage (MTLTextureUsageRenderTarget, 4); a draw needs it");
        }
        if (t.type() != TextureType::k2D && t.type() != TextureType::k2DMultisample && t.type() != TextureType::k2DArray && t.type() != TextureType::kCube &&
            t.type() != TextureType::k3D && t.type() != TextureType::kCubeArray) {
            return refuse(why, at + "a colour attachment of texture type " + n(uint32_t(t.type())) +
                                   ": 2D (2), 2D multisample (4), a 2D array's layer (3), a cube's or cube array's face (5, 6) and a 3D texture's plane (7) are served");
        }
        // a slice of a 2D array, a cube (array) or a 3D texture (Texture::depth() counts them all: layers, faces, planes)
        const uint32_t slices = (t.type() == TextureType::k2DArray || t.type() == TextureType::kCube || t.type() == TextureType::kCubeArray ||
                                 t.type() == TextureType::k3D) ? t.depth() : 1;
        if (a.slice >= slices) {
            return refuse(why, at + "slice " + n(a.slice) + " of a texture of " + n(slices));
        }
        if (!rbMrtPitchValid(uint32_t(t.bytesPerRow())) || t.buffer()->length() >= (1ull << 32)) {
            return refuse(why, at + "the target's pitch of " + n(t.bytesPerRow()) + " bytes or its " + n(t.buffer()->length()) +
                                   " bytes do not fit RB_MRT_PITCH / RB_MRT_ARRAY_PITCH");
        }
        if (a.load != LoadAction::kLoad && a.load != LoadAction::kDontCare && a.load != LoadAction::kClear) {
            return refuse(why, at + "load action " + n(uint32_t(a.load)) + ": MTLLoadAction is DontCare (0), Load (1) or Clear (2)");
        }
        // DontCare is served as a Store: in sysmem the RB writes the target's memory as it draws, and what DontCare
        // allows -- the contents undefined after the pass -- includes keeping them
        if (a.store != StoreAction::kStore && a.store != StoreAction::kDontCare) {
            return refuse(why, at + "store action " + n(uint32_t(a.store)) + ": Store (1) and DontCare (0) are served");
        }
        if (f.depth) {
            return refuse(why, std::string("a ") + f.name + " texture as " + which + ": a depth format is the depth attachment's");
        }
        if (a.level >= t.levels()) {
            return refuse(why, which + " names mip level " + n(a.level) + " of a texture of " + n(t.levels()));
        }
        if (t.samples() != firstAttachment(pass)->samples()) {
            return refuse(why, which + " has " + n(t.samples()) + " sample(s) beside another attachment's " + n(firstAttachment(pass)->samples()) +
                                   ": a pass's attachments share one sample count");
        }
        // (attachments of different sizes are Metal's to allow: the window is the smallest of them, windowOf below)
    }
    if (pass.depth != nullptr) {
        const Texture& d = *pass.depth;
        if (!d.info().depth) {
            return refuse(why, std::string("a ") + d.info().name + " texture as the depth attachment: Depth32Float (252) is served");
        }
        if (d.samples() != firstAttachment(pass)->samples()) {
            return refuse(why, "a depth attachment of " + n(d.samples()) + " sample(s) beside colour of " + n(firstAttachment(pass)->samples()) +
                                   ": a pass's attachments share one sample count");
        }
        if ((d.usage() & kTextureUsageRenderTarget) == 0) {
            return refuse(why, "the depth texture was made without render-target usage (MTLTextureUsageRenderTarget, 4)");
        }
        const Attachment c = attachmentOf(pass, 0).texture != nullptr ? attachmentOf(pass, 0) : Attachment{nullptr, 0, LoadAction::kLoad, StoreAction::kStore, nullptr, 0};
        if (pass.depthLevel >= d.levels()) {
            return refuse(why, "the depth attachment names mip level " + n(pass.depthLevel) + " of a texture of " + n(d.levels()));
        }
        const TextureLevel DL = d.level(pass.depthLevel);
        // a depth larger than the colour is drawn in the colour's window (windowOf: the smallest attachment): Metal's
        // validation asks for one size, and sokol hands Apple's Metal a level-0 depth beside a smaller colour level
        // (its depth attachment never names a level), which Apple's GPUs draw. A smaller depth would leave pixels of
        // the window without one.
        if (c.texture != nullptr && (DL.width < c.texture->level(c.level).width || DL.height < c.texture->level(c.level).height)) {
            return refuse(why, "a " + n(DL.width) + " x " + n(DL.height) + " depth attachment (level " + n(pass.depthLevel) + ") beside a " +
                                   n(c.texture->level(c.level).width) + " x " + n(c.texture->level(c.level).height) + " colour attachment (level " + n(c.level) +
                                   "): a depth attachment smaller than the colour leaves pixels without one");
        }
        if ((d.bytesPerRow() >> A6XX_RB_DEPTH_BUFFER_PITCH__SCALE) > A6XX_RB_DEPTH_BUFFER_PITCH__MASK || d.buffer()->length() >= (1ull << 32)) {
            return refuse(why, "the depth texture's pitch of " + n(d.bytesPerRow()) + " bytes or its " + n(d.buffer()->length()) +
                                   " bytes do not fit RB_DEPTH_BUFFER_PITCH / ARRAY_PITCH");
        }
        if (pass.depthLoad != LoadAction::kLoad && pass.depthLoad != LoadAction::kDontCare && pass.depthLoad != LoadAction::kClear) {
            return refuse(why, "depth load action " + n(uint32_t(pass.depthLoad)) + ": MTLLoadAction is DontCare (0), Load (1) or Clear (2)");
        }
        if (pass.depthStore != StoreAction::kStore && pass.depthStore != StoreAction::kDontCare) {
            return refuse(why, "depth store action " + n(uint32_t(pass.depthStore)) + ": DontCare (0) and Store (1) are served");
        }
        if (pass.depthLoad == LoadAction::kClear && !(pass.clearDepth >= 0.0 && pass.clearDepth <= 1.0)) {
            return refuse(why, "a clear depth of " + std::to_string(pass.clearDepth) + ": Metal's clearDepth is 0 to 1");
        }
    }
    if (pass.stencil != nullptr) {
        if (pass.stencil != pass.depth || !pass.stencil->info().stencil) {
            return refuse(why, "a stencil attachment that is not the depth attachment's own Depth32Float_Stencil8 texture: the S8 plane of that one texture is what is served");
        }
        if (pass.stencilLoad != LoadAction::kLoad && pass.stencilLoad != LoadAction::kDontCare && pass.stencilLoad != LoadAction::kClear) {
            return refuse(why, "stencil load action " + n(uint32_t(pass.stencilLoad)) + ": MTLLoadAction is DontCare (0), Load (1) or Clear (2)");
        }
        if (pass.stencilStore != StoreAction::kStore && pass.stencilStore != StoreAction::kDontCare) {
            return refuse(why, "stencil store action " + n(uint32_t(pass.stencilStore)) + ": DontCare (0) and Store (1) are served");
        }
    }
    return true;
}

// the Clear words of every set attachment that asks for a Clear (the rest left zero)
bool clearsOf(const RenderPass& pass, ClearWords out[kMaxColorOutputs], std::string* why) {
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        const Attachment a = attachmentOf(pass, i);
        out[i] = ClearWords{};
        if (a.texture != nullptr && a.load == LoadAction::kClear && !clearWords(a.texture->info(), a.clearColor, &out[i], why)) {
            return i == 0 ? false : refuse(why, "colour attachment " + n(i) + ": " + *why);
        }
    }
    return true;
}

bool anyColorClear(const RenderPass& pass) {
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        const Attachment a = attachmentOf(pass, i);
        if (a.texture != nullptr && a.load == LoadAction::kClear) return true;
    }
    return false;
}

// the fills, attachment by attachment as turnip clears them (tu_clear_sysmem_attachment per attachment), each waited for
// on its own word
void clearAll(pm4::Stream& stream, const RenderPass& pass, const ClearWords c[kMaxColorOutputs], uint64_t table) {
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        const Attachment a = attachmentOf(pass, i);
        if (a.texture != nullptr && a.load == LoadAction::kClear) clearStream(stream, *a.texture, c[i], clearFlushAt(table, i), a.level, a.slice);
    }
}

// the table's words the stream addresses, zeroed before the GPU sees them
void tableStart(uint8_t* tableCpu) {
    memset(tableCpu + kTableTimestampsAt, 0, kTableZeroedBytes);   // the tail's and the clears' values: 0 until the events write them
    writeBorderColors(tableCpu + kTableBorderColorAt);     // the entries a sampler's BCOLOR names (the batch start points the TP here)
    memset(tableCpu + kTableHoleAt, 0, 64);
}

// steps 1 to 3 of a pass: the batch start, the CCU set to sysmem, the pass begin -- what a draw and a Clear without a
// draw both need before the 2D engine or the 3D pipe writes the target
void passStart(pm4::Stream& stream, uint32_t W, uint32_t H, uint64_t table, const Options& o) {
    // 1. batch start: fd6_emit_restore (fd6_emit.cc:1090-1160) with turnip's tu6_init_hw as the second witness
    // (tu_cmd_buffer.cc:1584-1660): CP_SET_MODE 0; the three invalidates (fd6_cache_inv, fd6_emit.h:271-276; turnip's A6XX
    // path has CACHE_INVALIDATE alone, the kernel already emits both CCU invalidates per submit); HLSQ_INVALIDATE_CMD;
    // a WFI; the static tables a6xx.hpp documents, in Gallium's order; CP_SET_DRAW_STATE disable-all (no state
    // group is left pending by a previous owner of the GPU, and r3d needs none active: tu_clear_blit.cc:3960-3967); the
    // 32 VFD_FETCH sizes zeroed so a stale BASE is never fetched; the two border colour bases at the table's zero entry.
    // Where the two witnesses' tables differ the table keeps fd6's (SP_FLOAT_CNTL F16_NO_INF, GRAS_UNKNOWN_8110 2);
    // both are --bisect items the lead must run.
    if (o.batchStart) {
        command1(stream, CP_SET_MODE, 0);
        event(stream, PC_CCU_INVALIDATE_COLOR);
        event(stream, PC_CCU_INVALIDATE_DEPTH);
        event(stream, CACHE_INVALIDATE);
        reg(stream, REG_A6XX_HLSQ_INVALIDATE_CMD, hlsqInvalidateCmdAll());
        stream.waitForIdle();
        for (const RegWrite& w : kStaticState) reg(stream, w.reg, w.value);
        {
            uint32_t w[3] = {setDrawState0(0, CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS, 0), 0, 0};
            stream.packet(CP_SET_DRAW_STATE, w, 3);
        }
        for (const RegWrite& w : kStaticStateAfterDrawState) reg(stream, w.reg, w.value);
        for (uint32_t i = 0; i < kVfdFetchCount; i++) reg(stream, vfdFetchSize(i), 0);
        reg64(stream, REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR, table + kTableBorderColorAt);
        reg64(stream, REG_A6XX_SP_PS_TP_BORDER_COLOR_BASE_ADDR, table + kTableBorderColorAt);
        for (const RegWrite& w : kStaticStateTail) reg(stream, w.reg, w.value);
    }
    // 2. RB_CCU_CNTL for sysmem after a WFI ("isn't pipelined", tu_cmd_buffer.cc:373-377; fd6_emit.cc:828 "early a6xx
    // needed this"): 0x10000000 in both witnesses (rbCcuCntlSysmem); bit 2, CONCURRENT_RESOLVE, is what hakios's kernel
    // wrote at boot (hakios.adreno.c:2904) and Mesa never sets for this chip -- the switch is the bisect's
    if (o.ccuCntl) {
        stream.waitForIdle();
        reg(stream, REG_A6XX_RB_CCU_CNTL, rbCcuCntlSysmem() | (o.ccuConcurrentResolve ? A6XX_RB_CCU_CNTL_CONCURRENT_RESOLVE : 0u));
    }
    // fd6's LRZ_FLUSH at the start of a sysmem pass (fd6_gmem.cc:2001); turnip has none here. Under lrzOff with the LRZ
    // registers below, since it is the LRZ it flushes
    if (o.lrzOff) {
        event(stream, LRZ_FLUSH);
    }
    // 3. pass begin (tu6_sysmem_render_begin, tu_cmd_buffer.cc:2093-2142; fd6_emit_sysmem_prep, fd6_gmem.cc:2020-2056):
    // the window scissor is the whole target (BR inclusive: tu6_emit_window_scissor :736-747), GRAS_2D_RESOLVE_CNTL_1/2
    // beside it (:745-746), the four A6XX window offsets 0 (set_window_offset, fd6_gmem.cc:1223-1235; turnip's
    // A7XX_SP_PS_2D_WINDOW_OFFSET is not copied), GRAS/RB_BIN_CONTROL with BUFFERS_IN_SYSMEM and no bins (0x00c00000 as
    // fd6 and r3d; 0x03c00000 with turnip's LRZ feedback mask under binControlLrzFeedback), RB_BIN_CONTROL2 0, the
    // RM6_DIRECT_RENDER marker, CP_SKIP_IB2_ENABLE_GLOBAL 0, CP_SET_VISIBILITY_OVERRIDE 1 (what makes a draw without a
    // visibility stream legal), CP_SET_MODE 0. VPC_SO_DISABLE stays at the static table's 1 (turnip's; fd6 writes 0
    // here, fd6_gmem.cc:2053): not switchable in render::Options, named for the lead.
    if (o.passBegin) {
        regs(stream, REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL, {regXY(0, 0), regXY(W - 1, H - 1)});
        regs(stream, REG_A6XX_GRAS_2D_RESOLVE_CNTL_1, {regXY(0, 0), regXY(W - 1, H - 1)});
        reg(stream, REG_A6XX_RB_WINDOW_OFFSET, regXY(0, 0));
        reg(stream, REG_A6XX_RB_WINDOW_OFFSET2, regXY(0, 0));
        reg(stream, REG_A6XX_SP_WINDOW_OFFSET, regXY(0, 0));
        reg(stream, REG_A6XX_SP_TP_WINDOW_OFFSET, regXY(0, 0));
        reg(stream, REG_A6XX_GRAS_BIN_CONTROL, binControlSysmem(o.binControlLrzFeedback));
        reg(stream, REG_A6XX_RB_BIN_CONTROL, binControlSysmem(o.binControlLrzFeedback));
        reg(stream, REG_A6XX_RB_BIN_CONTROL2, 0);
    }
    if (o.marker) {
        command1(stream, CP_SET_MARKER, setMarker0(RM6_DIRECT_RENDER));
    }
    if (o.passBegin) {
        command1(stream, CP_SKIP_IB2_ENABLE_GLOBAL, 0);
        command1(stream, CP_SET_VISIBILITY_OVERRIDE, 1);
        command1(stream, CP_SET_MODE, 0);
    }
}

void passTail(pm4::Stream& stream, uint64_t table, const Options& o, bool depth = false) {
    // 19. the tail: a WFI (as compute's), then what puts the colour in memory -- PC_CCU_FLUSH_COLOR_TS as a 4-word event
    // writing 1 (turnip's form, tu_cmd_buffer.cc:48-70, with a value where turnip writes a dummy) and a wait for it
    // (neither witness waits: both let the kernel's fence follow; the kernel's own fence here has no CCU flush,
    // hakios.adreno.c:3496-3516, so the wait is cheap insurance); LRZ_FLUSH as both witnesses end a pass (tu_lrz.cc:465,
    // fd6_gmem.cc:2114); RB_DONE_TS only under tailRbDone (in neither witness's render end); CACHE_FLUSH_TS writing 2 and
    // CP_WAIT_MEM_GTE for it, compute's measured tail
    stream.waitForIdle();
    {
        const uint64_t ts = table + kTableTimestampsAt;
        if (o.tailCcuFlush) {
            eventTimestamp(stream, PC_CCU_FLUSH_COLOR_TS, ts, 1, o.tailTimestampBit);
            waitForWord(stream, ts, 1);
        }
        // with a depth attachment its CCU too (tu6_emit_flushes' FD_CCU_CLEAN_DEPTH, the 4-word PC_CCU_FLUSH_DEPTH_TS on
        // A6XX): every draw is a pass of its own here, so the next draw's depth test must find this one's depth in memory
        if (depth && o.tailDepthFlush) {
            eventTimestamp(stream, PC_CCU_FLUSH_DEPTH_TS, table + kTableDepthFlushAt, kDepthFlushValue, false);
            waitForWord(stream, table + kTableDepthFlushAt, kDepthFlushValue);
        }
        if (o.tailLrzFlush) {
            event(stream, LRZ_FLUSH);
        }
        if (o.tailRbDone) {
            eventTimestamp(stream, RB_DONE_TS, ts + 8, 3, o.tailTimestampBit);
            waitForWord(stream, ts + 8, 3);
        }
        if (o.tailCacheFlush) {
            eventTimestamp(stream, CACHE_FLUSH_TS, ts + 4, 2, o.tailTimestampBit);
            waitForWordAtLeast(stream, ts + 4, 2);
        }
    }
}

}  // namespace

namespace {
// A draw, either WHOLE (its own pass: the batch start, the pass begin, the clears, the draw, the tail) or inside a pass
// encodeBegin opened (the draw's state and the draw alone: TABLE is then the draw's own, holding only its descriptors;
// the pass's table, which the batch start pointed the border colours at and the tail writes, is the pass's)
bool encodeDraw(pm4::Stream& stream, const Program& program, const RenderPass& pass, const Draw& draw, const Bindings& vsBindings,
                const Bindings& fsBindings, uint64_t vsProgram, uint64_t fsProgram, uint64_t table, uint8_t* tableCpu, std::string* why,
                const Options& o, bool whole) {
    const Stage& vs = program.vs;
    // A [[color(n)]] the pass has no attachment n for is written nowhere: Metal leaves that pipeline slot's format Invalid
    // and discards the output (CoreAnimation's fixed_frag_lph_cpf writes three into passes of one). The stage is drawn
    // as if it did not write it -- no register for the slot, RB_MRT_CONTROL 0 -- which is the slot nothing writes.
    Stage fsDrawn = program.fs;
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        if (fsDrawn.colorRegid[i] != kRegidNone && attachmentOf(pass, i).texture == nullptr) fsDrawn.colorRegid[i] = kRegidNone;
    }
    const Stage& fs = fsDrawn;
    // --- the two stages ---
    if (vs.kind != kStageVertex) {
        return refuse(why, std::string(stageName(vs)) + " is not a vertex stage");
    }
    if (fs.kind != kStageFragment) {
        return refuse(why, std::string(stageName(fs)) + " is not a fragment stage");
    }
    if (!checkStage(vs, why) || !checkStage(fs, why)) {
        return false;
    }
    // each stage's pushed constant ranges out of its own [[buffer(n)]] table (Metal's setVertexBuffer / setFragmentBuffer)
    std::vector<PushedRange> vsRanges, fsRanges;
    if (!pushedRanges(vs, vsBindings, &vsRanges, why) || !pushedRanges(fs, fsBindings, &fsRanges, why)) {
        return false;
    }
    if (vs.k.constlen + fs.k.constlen > kMaxConstPipeline) {
        // ir3_compiler.c:185-188 max_const_pipeline = 512 vec4 for gen 6; ir3_trim_constlen (tu_pipeline.cc:2207) trims
        // a pipeline over it, here the sum is refused instead
        return refuse(why, "the two stages need " + n(vs.k.constlen + fs.k.constlen) + " vec4 of constants together (" + n(vs.k.constlen) + " + " +
                               n(fs.k.constlen) + "): the pipeline has " + n(kMaxConstPipeline) + " (ir3_compiler.c max_const_pipeline)");
    }
    // the VS's [[stage_in]] attributes through the pipeline's vertex descriptor and the VS's own [[buffer(n)]] table
    VertexFetch fetch;
    if (!vertexFetch(vs, program.vertexDescriptor, draw, vsBindings, &fetch, why)) {
        return false;
    }

    if (vs.clipMask != 0 || vs.cullMask != 0) {
        return refuse(why, std::string(stageName(vs)) + " writes clip or cull distances: not served");
    }
    if (vs.writesLayer || vs.writesViewport) {
        return refuse(why, std::string(stageName(vs)) + " writes a layer or viewport index: not served");
    }
    // Up to eight colour outputs, with gaps: CoreAnimation's fixed_frag_lph_cph writes [[color(fc_dest_index)]] alone --
    // [[color(1)]] beside an attachment 0 it reads through framebuffer fetch. A slot below the highest that the FS does
    // not write is an RB_MRT entry with nothing to write (no register, no components, no write mask), as turnip leaves
    // an attachment a subpass does not write; each slot it writes goes into an attachment of its own.
    // A stage that writes no colour at all is CoreAnimation's path_edges_fragment, run for the stencil it leaves: it is
    // drawn as one slot nothing is written to, the same entry a gap gets, so attachment 0 keeps what it holds.
    if (fs.numColorOutputs > kMaxColorOutputs) {
        return refuse(why, std::string(stageName(fs)) + " writes " + n(fs.numColorOutputs) + " colour output(s): 8 at most are served");
    }
    if (fs.depthRegid != kRegidNone || fs.writesPos) {
        return refuse(why, std::string(stageName(fs)) + " writes depth: no depth attachment is served");
    }
    if (fs.stencilRefRegid != kRegidNone || fs.writesStencilref) {
        return refuse(why, std::string(stageName(fs)) + " writes a stencil reference: no stencil attachment is served");
    }
    if (fs.sampleMaskRegid != kRegidNone || fs.writesSmask) {
        return refuse(why, std::string(stageName(fs)) + " writes a sample mask: no multisampling is served");
    }
    if (fs.sampleIdRegid != kRegidNone || fs.sampleMaskInRegid != kRegidNone) {
        return refuse(why, std::string(stageName(fs)) + " reads the sample id or the sample mask: no multisampling is served");
    }
    if (validReg(fs.ijRegid[IJ_PERSP_SAMPLE]) || validReg(fs.ijRegid[IJ_PERSP_CENTROID]) || validReg(fs.ijRegid[IJ_LINEAR_SAMPLE]) ||
        validReg(fs.ijRegid[IJ_LINEAR_CENTROID])) {
        return refuse(why, std::string(stageName(fs)) + " reads centroid or per-sample barycentrics: no multisampling is served");
    }
    if (fs.perSamp) {
        return refuse(why, std::string(stageName(fs)) + " runs per sample: no multisampling is served");
    }
    if (fs.dualSrcBlend) {
        return refuse(why, std::string(stageName(fs)) + " writes a dual-source blend output: blending is off in the first draw");
    }
    if (fs.readsPrimid) {
        return refuse(why, std::string(stageName(fs)) + " reads the primitive id: not served");
    }
    // private memory: a stage that wants it gets the queue's, laid out at least as large and in the same layout
    for (const auto& [s, b] : {std::pair<const Stage*, const Bindings*>{&vs, &vsBindings}, {&fs, &fsBindings}}) {
        if (s->k.pvtmemSize == 0) continue;
        const PrivateMemory& pm = b->privateMemory;
        if (pm.address == 0) {
            return refuse(why, std::string(stageName(*s)) + " wants " + n(s->k.pvtmemSize) + " bytes of private memory a fiber and none is bound");
        }
        if (pm.perFiber < s->k.pvtmemSize || pm.perWave != s->k.pvtmemPerWave || pm.address % 32 != 0) {
            return refuse(why, std::string(stageName(*s)) + " wants " + n(s->k.pvtmemSize) + " bytes a fiber" + (s->k.pvtmemPerWave ? " laid out a wave" : "") +
                                   ": the private memory bound has " + n(pm.perFiber) + (pm.perWave ? " laid out a wave" : "") + " at " + n(pm.address));
        }
    }
    // --- the target ---
    if (!checkTarget(pass, why)) {
        return false;
    }
    const uint32_t samples = firstAttachment(pass)->samples();
    if (program.samples != samples) {
        return refuse(why, "a pipeline of rasterSampleCount " + n(program.samples) + " into attachments of " + n(samples) + " sample(s): Metal asks for the same count");
    }
    const uint32_t numRt = std::max<uint32_t>(fs.numColorOutputs, 1);
    for (uint32_t i = 0; i < numRt; i++) {
        const Texture* t = attachmentOf(pass, i).texture;
        if (fs.colorRegid[i] == kRegidNone) continue;   // (a slot nothing is written to: no attachment needed)
        if (t == nullptr) {
            return refuse(why, std::string(stageName(fs)) + " writes [[color(" + n(i) + ")]] and the pass has no colour attachment " + n(i));
        }
        // the FS's colour class against the format's: floats into a unorm/float format, integers into a Uint one (the
        // hardware converts the raw register as the format says, so a mismatch is garbage on the phone, not an error).
        // The class rides in StageOutput::pad of each FRAG_RESULT_DATA0 + i entry, as compiler.cpp's fillStage and the
        // fixture writer put it there (linkProgram reads attachment 0's the same way)
        uint8_t fsClass = kElemFloat;
        for (uint32_t k = 0; k < fs.numOutputs && k < kMaxStageOutputs; k++) {
            if (fs.outputs[k].slot == kFragResultData0 + i) fsClass = fs.outputs[k].pad;
        }
        const FormatInfo& f = t->info();
        uint8_t formatClass = f.element == kElemFloat ? kElemFloat : kElemUint;
        if ((fsClass == kElemFloat) != (formatClass == kElemFloat)) {
            return refuse(why, std::string(stageName(fs)) + " returns " + (fsClass == kElemFloat ? "floats" : "integers") + " into " + f.name +
                                   (i != 0 ? " (colour attachment " + n(i) + ")" : std::string()) + ", which holds " +
                                   (formatClass == kElemFloat ? "floats" : "integers"));
        }
    }
    ClearWords clears[kMaxColorOutputs];
    if (whole && !clearsOf(pass, clears, why)) {
        return false;
    }
    BlendWords blends[kMaxColorOutputs] = {};
    uint32_t blendMask = 0;
    for (uint32_t i = 0; i < numRt; i++) {
        if (fs.colorRegid[i] == kRegidNone) continue;   // RB_MRT_CONTROL 0: no components written, no blend
        if (!blendWords(i == 0 ? program.blend : program.moreBlend[i - 1], attachmentOf(pass, i).texture->info(), &blends[i], why)) {
            return i == 0 ? false : refuse(why, "colour attachment " + n(i) + ": " + *why);
        }
        if (blends[i].enabled) blendMask |= 1u << i;
    }
    StageTextures vsTex, fsTex;
    if (!stageTextures(vs, vsBindings, pass, &vsTex, why) || !stageTextures(fs, fsBindings, pass, &fsTex, why)) {
        return false;
    }
    std::vector<uint32_t> vsUbos, fsUbos;
    if (!uboDescriptors(vs, vsBindings, vsProgram, &vsUbos, why) || !uboDescriptors(fs, fsBindings, fsProgram, &fsUbos, why)) {
        return false;
    }
    GfxIbos ibos;
    if (!gfxIbos(vs, vsBindings, fs, fsBindings, program.fsIboBase, &ibos, why)) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!std::isfinite(draw.blendColor[i])) {
            return refuse(why, "a blend colour with a non-finite channel (channel " + n(uint32_t(i)) + ")");
        }
    }
    // the depth test is on when it can change anything: a compare other than Always, or a write (turnip's test_enable)
    if (draw.depth.compare > 7) {
        return refuse(why, "depth compare function " + n(draw.depth.compare) + ": MTLCompareFunction runs Never 0 to Always 7");
    }
    const bool depthTest = draw.depth.compare != 7 || draw.depth.write;
    const StencilState& st = draw.stencil;
    if (st.enabled && pass.stencil == nullptr) {
        return refuse(why, "a stencil test with no stencil attachment");
    }
    for (const StencilFace* f : {&st.front, &st.back}) {
        if (st.enabled && (f->compare > 7 || f->stencilFail > 7 || f->depthFail > 7 || f->pass > 7)) {
            return refuse(why, "a stencil face of compare " + n(f->compare) + " and operations " + n(f->stencilFail) + " / " + n(f->depthFail) + " / " + n(f->pass) +
                                   ": MTLCompareFunction and MTLStencilOperation run 0 to 7");
        }
    }
    if (depthTest && pass.depth == nullptr) {
        return refuse(why, "a depth test (compare " + n(draw.depth.compare) + (draw.depth.write ? ", write" : "") + ") with no depth attachment");
    }
    // --- the draw ---
    pc_di_primtype prim = DI_PT_TRILIST;
    switch (draw.primitive) {
    case Primitive::kTriangle: prim = DI_PT_TRILIST; break;
    case Primitive::kTriangleStrip: prim = DI_PT_TRISTRIP; break;
    // lines are one pixel wide in Metal: GRAS_SU_CNTL's LINEHALFWIDTH is already 2 quarters (a6xx.hpp: grasSuCntl) and
    // its LINE_MODE RECTANGULAR, turnip's for Vulkan's default lines (tu6_emit_rast)
    case Primitive::kLine: prim = DI_PT_LINELIST; break;
    case Primitive::kLineStrip: prim = DI_PT_LINESTRIP; break;
    // a point is as large as the VS's [[point_size]] (linked after the position, PC_VS_OUT_CNTL.PSIZE), else the
    // GRAS_SU_POINT_SIZE of 1 the rasterizer state writes; Metal leaves a point with no size undefined
    case Primitive::kPoint: prim = DI_PT_POINTLIST; break;
    default:
        return refuse(why, "primitive " + n(uint32_t(draw.primitive)) + ": MTLPrimitiveType is Point (0), Line (1), LineStrip (2), Triangle (3) or TriangleStrip (4)");
    }
    const bool indirect = draw.indirect != nullptr;
    if (indirect) {
        const uint64_t words = draw.indexBuffer != nullptr ? 5 : 4;
        if (draw.indirectOffset % 4 != 0 || draw.indirectOffset + 4 * words > draw.indirect->length()) {
            return refuse(why, "an indirect draw's arguments at " + n(draw.indirectOffset) + " of a " + n(draw.indirect->length()) + "-byte buffer: " + n(words) +
                                   " words inside it, at a multiple of 4");
        }
        // CP_DRAW_INDIRECT_MULTI writes the draw's own params (draw id, vertex offset, first instance) at DST_OFF, and a
        // DST_OFF of 0 is its "none" (tu_cmd_buffer.cc:6119): a VS reading them from vec4 0 cannot be given them
        if (vs.k.driverParamsOffsetVec4 == 0) {
            return refuse(why, std::string(stageName(vs)) + " reads its base vertex and instance from c0, which an indirect draw cannot write (DST_OFF 0 is none)");
        }
    } else if (draw.vertexCount == 0) {
        return refuse(why, "a draw of 0 vertices");
    } else if (draw.instanceCount == 0) {
        return refuse(why, "a draw of 0 instances");
    }
    if (draw.visibilityMode > 2) {
        return refuse(why, "visibility result mode " + n(draw.visibilityMode) + ": MTLVisibilityResultMode is Disabled (0), Boolean (1) or Counting (2)");
    }
    if (draw.visibilityMode != 0) {
        if (draw.visibility == nullptr) {
            return refuse(why, "a visibility result mode and no visibility result buffer in the pass");
        }
        if (draw.visibilityOffset % 8 != 0 || draw.visibilityOffset + 8 > draw.visibility->length()) {
            return refuse(why, "a visibility result at offset " + n(draw.visibilityOffset) + ": a multiple of 8 with its 8 bytes inside the " +
                                   n(draw.visibility->length()) + "-byte buffer");
        }
    }
    uint32_t indexSize = 0;
    uint64_t maxIndices = 0;
    if (draw.indexBuffer != nullptr) {
        if (draw.indexType > 1) {
            return refuse(why, "index type " + n(draw.indexType) + ": MTLIndexType is UInt16 (0) or UInt32 (1)");
        }
        indexSize = draw.indexType == 0 ? 2 : 4;
        if (draw.indexOffset % indexSize != 0 || draw.indexOffset > draw.indexBuffer->length()) {
            return refuse(why, "an index buffer offset of " + n(draw.indexOffset) + ": a multiple of the index size (" + n(indexSize) + ") inside the buffer");
        }
        maxIndices = (draw.indexBuffer->length() - draw.indexOffset) / indexSize;
        if (!indirect && draw.vertexCount > maxIndices) {
            return refuse(why, "a draw of " + n(draw.vertexCount) + " indices from an index buffer holding " + n(maxIndices) + " past its offset");
        }
        if (draw.baseVertex < 0) {
            return refuse(why, "a base vertex of " + std::to_string(draw.baseVertex) + ": a negative one is not served (VFD_INDEX_OFFSET is added unsigned)");
        }
        if (maxIndices > 0xffffffffull) maxIndices = 0xffffffffull;
    }
    if (draw.cullMode > 2 || draw.winding > 1) {
        return refuse(why, "cull mode " + n(draw.cullMode) + ", winding " + n(draw.winding) + ": MTLCullMode is 0 to 2, MTLWinding 0 or 1");
    }
    const auto [W, H] = windowOf(pass);
    const Viewport& vp = draw.viewport;
    if (!finite(vp.x) || !finite(vp.y) || !finite(vp.width) || !finite(vp.height) || !finite(vp.znear) || !finite(vp.zfar)) {
        return refuse(why, "a viewport with a non-finite value");
    }
    if (!(vp.width > 0.0) || !(vp.height > 0.0)) {
        return refuse(why, "a viewport of " + n(uint64_t(std::max(vp.width, 0.0))) + " x " + n(uint64_t(std::max(vp.height, 0.0))) + " pixels: both sides must be positive");
    }
    // A viewport may reach past the target, as Metal's may (sokol's sgl sample draws through a 406 x 406 one into a 375 x
    // 812 drawable): what falls outside is clipped by the viewport scissor, which step 16 clamps to the window. One that
    // covers no pixel of the window at all would need an empty scissor, which inclusive corners cannot say: refused.
    if (vp.x >= double(W) || vp.y >= double(H) || vp.x + vp.width <= 0.0 || vp.y + vp.height <= 0.0) {
        return refuse(why, "viewport (" + std::to_string(vp.x) + ", " + std::to_string(vp.y) + ", " + std::to_string(vp.width) + ", " + std::to_string(vp.height) +
                               ") covers no pixel of the " + n(W) + " x " + n(H) + " target");
    }
    Scissor sc = draw.scissor;
    if (sc.width == 0) {
        sc = Scissor{0, 0, W, H};   // render.hpp: width 0 is the whole target
    } else if (sc.height == 0) {
        return refuse(why, "a scissor of " + n(sc.width) + " x 0 pixels: an empty scissor draws nothing (width 0 means the whole target)");
    } else if (uint64_t(sc.x) + sc.width > W || uint64_t(sc.y) + sc.height > H) {
        return refuse(why, "scissor (" + n(sc.x) + ", " + n(sc.y) + ", " + n(sc.width) + ", " + n(sc.height) + ") lies outside the " + n(W) + " x " + n(H) + " target");
    }
    // --- the link: ir3_link_shaders over the two stages' tables (src/link.cpp), recomputed here so that a hand-built
    // Program needs none and the one linkProgram stored cannot disagree with the registers
    Linkage link{};
    if (!render::link(vs, fs, &link, why)) {
        return false;
    }
    if (link.cnt == 0 || link.cnt > kMaxStageOutputs || link.positionLoc == kLocNone || link.maxLoc == 0) {
        return refuse(why, "the link of " + std::string(stageName(vs)) + " and " + stageName(fs) + " has no position entry");
    }

    // --- nothing is refused from here on: the table, then the stream ---
    if (whole) {
        tableStart(tableCpu);
    }
    memset(tableCpu + kTableFbReadFlushAt, 0, 4);   // (the draw's own table: zero before the flush below writes it)
    memcpy(tableCpu + kGfxIboAt, ibos.words, size_t(ibos.count) * 64);
    memset(tableCpu + kFsTexConstAt, 0, kMaxTextureSlots * 64);
    memset(tableCpu + kFsTexSampAt, 0, kMaxTextureSlots * 16);
    memcpy(tableCpu + kFsTexConstAt, fsTex.texWords, sizeof fsTex.texWords);
    memcpy(tableCpu + kFsTexSampAt, fsTex.sampWords, sizeof fsTex.sampWords);
    memcpy(tableCpu + kVsTexConstAt, vsTex.texWords, sizeof vsTex.texWords);
    memcpy(tableCpu + kVsTexSampAt, vsTex.sampWords, sizeof vsTex.sampWords);
    MrtWords mrts[kMaxColorOutputs] = {};
    uint32_t srgbMask = 0;
    for (uint32_t i = 0; i < numRt; i++) {
        // (an unwritten slot with no attachment of its own names another attachment's memory: valid, and never written.
        // In a pass of depth alone it names the depth texture's, as RGBA8 -- 4 bytes a pixel as Depth32Float's plane is,
        // so its rows are the depth's rows -- which RB_MRT_CONTROL 0 never lets it write; memory the pass owns rather
        // than address 0, which nothing maps)
        const Attachment a = attachmentOf(pass, i);
        const Texture* named = a.texture != nullptr ? a.texture : firstAttachment(pass) != pass.depth ? firstAttachment(pass) : nullptr;
        if (named == nullptr) {
            const FormatInfo& rgba = *formatInfo(PixelFormat::kRGBA8Unorm);
            const Texture& d = *pass.depth;
            mrts[i] = MrtWords{rbMrtBufInfo(rgba.fmt6, TILE6_LINEAR, rgba.swap), rbMrtPitch(uint32_t(d.bytesPerRow())), rbMrtArrayPitch(uint32_t(d.buffer()->length())),
                               uint32_t(d.gpuAddress()), uint32_t(d.gpuAddress() >> 32), 0, spFsMrtReg(rgba.fmt6, false, false), false};
            continue;
        }
        mrts[i] = a.texture != nullptr ? mrtWords(*named, a.level, a.slice)
                                       : mrtWords(*named, named == attachmentOf(pass, 0).texture ? attachmentOf(pass, 0).level : 0,
                                                  named == attachmentOf(pass, 0).texture ? attachmentOf(pass, 0).slice : 0);
        if (mrts[i].srgb && fs.colorRegid[i] != kRegidNone) srgbMask |= 1u << i;
    }
    const a6xx_threadsize fsThreads = fs.k.doubleThreadsize ? THREAD128 : THREAD64;
    const uint32_t* ij = fs.ijRegid;

    if (whole) {
        passStart(stream, W, H, table, o);
        // a Clear: the 2D engine fills the target before any draw state is written, as turnip's subpass begin does it;
        // the depth attachment's Clear after it (tu_clear_sysmem_attachment, each attachment in turn)
        clearAll(stream, pass, clears, table);
        zsClears(stream, pass, table);
    }
    // A framebuffer fetch reads the target through the texture unit while the RB writes it through the colour CCU: what
    // the draws before this one (or the pass's Clear) wrote is put in memory and the texture caches dropped before it
    // runs -- turnip's barrier for a sysmem feedback loop (FD_CCU_CLEAN_COLOR | FD_CACHE_INVALIDATE), the CCU flush
    // waited for as the tail waits for its own. Within ONE draw nothing orders a pixel's read after an earlier
    // primitive's write of it: Metal's raster order is not kept between overlapping primitives of one draw.
    if (fs.fbRead && o.fbReadBarrier) {
        stream.waitForIdle();
        eventTimestamp(stream, PC_CCU_FLUSH_COLOR_TS, table + kTableFbReadFlushAt, kFbReadFlushValue, false);
        waitForWord(stream, table + kTableFbReadFlushAt, kFbReadFlushValue);
        event(stream, CACHE_INVALIDATE);
        stream.waitForIdle();
    }
    // 4. no depth attachment (tu6_emit_zs :415-433; fd6 emit_zs(NULL), fd6_gmem.cc:231-247): RB_DEPTH_BUFFER_INFO
    // DEPTH6_NONE with its pitches and bases 0, GRAS_SU_DEPTH_BUFFER_INFO 0, RB_STENCIL_INFO 0
    // With one (tu6_emit_zs :437-475; fd6 emit_zs, fd6_gmem.cc:180-229): DEPTH6_32, the texture's pitch and layer size,
    // its base, BASE_GMEM 0 in sysmem, the same format in GRAS_SU_DEPTH_BUFFER_INFO, the flag buffer 0 (no UBWC), no
    // stencil. A6XX's RB_DEPTH_BUFFER_INFO has no tile field: the RB reads and writes depth TILE6_3, the only layout the
    // hardware has for it (fd6_layout.c:157-164), which is how texture.cpp lays a Depth32Float texture out.
    if (o.zsNone && pass.depth == nullptr) {
        regs(stream, REG_A6XX_RB_DEPTH_BUFFER_INFO, {A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_NONE), 0, 0, 0, 0, 0});
        reg(stream, REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO, A6XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_NONE));
        reg(stream, REG_A6XX_RB_STENCIL_INFO, 0);
    }
    if (pass.depth != nullptr) {
        const Texture& d = *pass.depth;
        regs(stream, REG_A6XX_RB_DEPTH_BUFFER_INFO,
             {A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_32), A6XX_RB_DEPTH_BUFFER_PITCH(uint32_t(d.level(pass.depthLevel).pitch)),
              A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH(uint32_t(d.depthPlaneBytes())), uint32_t(d.levelAddress(pass.depthLevel)),
              uint32_t(d.levelAddress(pass.depthLevel) >> 32), 0});
        reg(stream, REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO, A6XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_32));
        regs(stream, REG_A6XX_RB_DEPTH_FLAG_BUFFER_BASE, {0, 0, 0});
        if (pass.stencil != nullptr) {
            // the S8 plane as a separate stencil buffer (tu6_emit_zs's D32_SFLOAT_S8_UINT branch): SEPARATE_STENCIL, its
            // pitch and layer, its base, BASE_GMEM 0; A6XX's RB_STENCIL_INFO has no tile field -- stencil is TILE6_3, as laid out
            const uint64_t sb = d.gpuAddress() + d.stencilOffset();
            regs(stream, REG_A6XX_RB_STENCIL_INFO, {A6XX_RB_STENCIL_INFO_SEPARATE_STENCIL, uint32_t(d.stencilPitch() >> 6), uint32_t(d.stencilPlaneBytes() >> 6),
                                                    uint32_t(sb), uint32_t(sb >> 32), 0});
        } else {
            reg(stream, REG_A6XX_RB_STENCIL_INFO, 0);
        }
    }
    // 5. the colour target (tu6_emit_mrt, tu_cmd_buffer.cc:482-576; r3d_dst, tu_clear_blit.cc:1380-1408; fd6 emit_mrt,
    // fd6_gmem.cc:60-147): RB_MRT[0]'s six registers as one pkt4 (BUF_INFO, PITCH, ARRAY_PITCH, BASE lo/hi, BASE_GMEM;
    // a seventh would land in RB_MRT_CONTROL[1]), SP_FS_MRT_REG[0], the flag buffer's three words 0 (no UBWC; inert
    // while RB_RENDER_CNTL.FLAG_MRTS is clear), GRAS_LRZ_MRT_BUF_INFO_0 the format, dither off, the sRGB bit in both
    // copies, GRAS_MAX_LAYER_INDEX 0
    // ...and RB_MRT[i] / SP_FS_MRT_REG[i] / the flag buffer for each further target the FS writes, the sRGB bit of each
    // in both copies (SRGB_MRTn is bit n in both registers)
    if (o.mrt) {
        for (uint32_t i = 0; i < numRt; i++) {
            const MrtWords& m = mrts[i];
            regs(stream, regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, i), {m.bufInfo, m.pitch, m.arrayPitch, m.baseLo, m.baseHi, m.baseGmem});
            reg(stream, regSpFsMrtReg(i), m.spFsMrtReg);
            regs(stream, regRbMrtFlagBuffer(REG_A6XX_RB_MRT_FLAG_BUFFER_ADDR, i), {0, 0, 0});
        }
        reg(stream, REG_A6XX_GRAS_LRZ_MRT_BUF_INFO_0, A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT(mrtFormatOf(mrts[0].bufInfo)));
        reg(stream, REG_A6XX_RB_DITHER_CNTL, 0);
        reg(stream, REG_A6XX_RB_SRGB_CNTL, srgbMask);
        reg(stream, REG_A6XX_SP_SRGB_CNTL, srgbMask);
        reg(stream, REG_A6XX_GRAS_MAX_LAYER_INDEX, 0);
    }
    // 6. one sample (tu6_emit_msaa, tu_cmd_buffer.cc:892-910; fd6 emit_msaa, fd6_gmem.cc:1037-1061): RAS_MSAA_CNTL 0 and
    // DEST_MSAA_CNTL MSAA_DISABLE in the three blocks, the sample-location configs 0 (fd6_context.cc:300-306),
    // RB_BLIT_GMEM_MSAA_CNTL 0 (fd6's)
    // ...or a multisample pass's count in all three blocks without MSAA_DISABLE, as tu6_emit_msaa writes it: the GRAS
    // covers samples, the RB writes each covered one, the FS still runs once a pixel (no sample shading). The sample
    // configs stay 0, the standard locations (tu6_emit_sample_locations without custom ones).
    if (o.msaaOff) {
        const a3xx_msaa_samples m = samples == 4 ? MSAA_FOUR : samples == 2 ? MSAA_TWO : MSAA_ONE;
        regs(stream, REG_A6XX_SP_TP_RAS_MSAA_CNTL, {A6XX_SP_TP_RAS_MSAA_CNTL_SAMPLES(m),
                                                    m == MSAA_ONE ? A6XX_SP_TP_DEST_MSAA_CNTL_MSAA_DISABLE : A6XX_SP_TP_DEST_MSAA_CNTL_SAMPLES(m)});
        reg(stream, REG_A6XX_SP_TP_SAMPLE_CONFIG, 0);
        regs(stream, REG_A6XX_GRAS_RAS_MSAA_CNTL, {A6XX_GRAS_RAS_MSAA_CNTL_SAMPLES(m),
                                                   m == MSAA_ONE ? A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE : A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES(m), 0});   // ..., GRAS_SAMPLE_CONFIG
        regs(stream, REG_A6XX_RB_RAS_MSAA_CNTL, {A6XX_RB_RAS_MSAA_CNTL_SAMPLES(m),
                                                 m == MSAA_ONE ? A6XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE : A6XX_RB_DEST_MSAA_CNTL_SAMPLES(m), 0});       // ..., RB_SAMPLE_CONFIG
        reg(stream, REG_A6XX_RB_BLIT_GMEM_MSAA_CNTL, 0);
    }
    // 7. RB_RENDER_CNTL: CCUSINGLECACHELINESIZE 2 in both pipelines (tu_cmd_buffer.cc:633-693; fd6_gmem.cc:531-552), 0 in
    // r3d_dst (tu_clear_blit.cc:1408) -- renderCntlZero for the bisect. A plain type-4: has_cp_reg_write is false here.
    if (o.renderCntl) {
        reg(stream, REG_A6XX_RB_RENDER_CNTL, o.renderCntlZero ? 0u : rbRenderCntlSysmem());
    }
    // 8. LRZ off (tu_lrz.cc:936-944 via a zero state; r3d :1622-1623; fd6 build_lrz with no zsbuf, fd6_emit.cc:262-270)
    // and fd6's zeroed LRZ buffer registers (emit_lrz, fd6_gmem.cc:250-260): BASE (2), PITCH, FAST_CLEAR_BUFFER_BASE (2)
    if (o.lrzOff) {
        reg(stream, REG_A6XX_GRAS_LRZ_CNTL, 0);
        reg(stream, REG_A6XX_RB_LRZ_CNTL, 0);
        regs(stream, REG_A6XX_GRAS_LRZ_BUFFER_BASE, {0, 0, 0, 0, 0});
    }
    // 9. program config (tu6_emit_program_config, tu_pipeline.cc:1272-1300; fd6 setup_config_stateobj, fd6_program.cc:
    // 400-458): HLSQ_INVALIDATE_CMD for the five graphics stages and the 3D IBOs, then SP_xS_CONFIG / HLSQ_xS_CNTL per
    // stage with HS, DS and GS off (NTEX = NSAMP = num_samp = 0; CONSTLEN in the field's units of 4 vec4, the FS's 0 is
    // ENABLED alone), SP_IBO_COUNT 0 (fd6's nibo(fs))
    if (o.programConfig) {
        reg(stream, REG_A6XX_HLSQ_INVALIDATE_CMD,
            A6XX_HLSQ_INVALIDATE_CMD_VS_STATE | A6XX_HLSQ_INVALIDATE_CMD_HS_STATE | A6XX_HLSQ_INVALIDATE_CMD_DS_STATE |
                A6XX_HLSQ_INVALIDATE_CMD_GS_STATE | A6XX_HLSQ_INVALIDATE_CMD_FS_STATE | A6XX_HLSQ_INVALIDATE_CMD_GFX_IBO);
        reg(stream, REG_A6XX_SP_VS_CONFIG, spXsConfig(vs.k.numSamp, vs.k.numSamp, vs.k.numSsbos));
        reg(stream, REG_A6XX_HLSQ_VS_CNTL, hlsqVsCntl(vs.k.constlen));
        reg(stream, REG_A6XX_SP_HS_CONFIG, 0);
        reg(stream, REG_A6XX_HLSQ_HS_CNTL, 0);
        reg(stream, REG_A6XX_SP_DS_CONFIG, 0);
        reg(stream, REG_A6XX_HLSQ_DS_CNTL, 0);
        reg(stream, REG_A6XX_SP_GS_CONFIG, 0);
        reg(stream, REG_A6XX_HLSQ_GS_CNTL, 0);
        reg(stream, REG_A6XX_SP_FS_CONFIG, spXsConfig(fs.k.numSamp, fs.k.numSamp, fs.k.numSsbos));
        reg(stream, REG_A6XX_HLSQ_FS_CNTL, hlsqFsCntl(fs.k.constlen));
        // the 3D IBO table's count (fd6's nibo(fs); here the larger of the two stages', the table being one), and the table
        // itself: CP_LOAD_STATE6 (the plain opcode) of ST6_SHADER into SB6_IBO, as both witnesses load it (tu_pipeline.cc:
        // 183, fd6_image.cc:308-335 -- bindless there, from the table here, the SP_CS_IBO way), then SP_IBO at it
        reg(stream, REG_A6XX_SP_IBO_COUNT, ibos.count);
        if (ibos.count != 0) {
            uint32_t w[3] = {loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_IBO, ibos.count), CP_LOAD_STATE6_1_EXT_SRC_ADDR(uint32_t(table + kGfxIboAt)),
                             CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(uint32_t((table + kGfxIboAt) >> 32))};
            stream.packet(CP_LOAD_STATE6, w, 3);
            reg64(stream, REG_A6XX_SP_IBO, table + kGfxIboAt);
        }
    }
    // 10. the two programs (tu6_emit_xs, tu_shader.cc:1238-1356; fd6_emit_shader, fd6_program.cc:100-245): CTRL_REG0 (the
    // VS's MERGEDREGS is bit 20 and it has no THREADSIZE; the FS's has THREADSIZE, VARYING, UNK24 always, MERGEDREGS bit
    // 31), INSTRLEN, the 7-register OBJ_START block with the private memory's address and layout (zeros for a stage with
    // none), HW_STACK_OFFSET the per-SP size as turnip writes it (tu_shader.cc:1328-1338), the ST6_SHADER
    // preload of min(instrlen, 127) units from the 128-byte aligned program, the immediates as compute uploads them
    // (ST6_CONSTANTS, direct, at immediatesOffsetVec4)
    // ... then the pushed constant ranges, indirect, as compute uploads them (fd6_emit_user_consts / tu6_emit_xs's ranges,
    // tu_shader.cc:1397-1421) into the stage's own block
    auto programOf = [&](const Stage& s, const Bindings& b, const std::vector<PushedRange>& ranges, const std::vector<uint32_t>& ubos, uint64_t address,
                         uint32_t ctrlReg0, uint32_t regCtrl0, uint32_t regInstrlen, uint32_t regObjFirstExec, uint32_t regHwStackOffset) {
        reg(stream, regCtrl0, ctrlReg0);
        reg(stream, regInstrlen, s.k.instrlen);
        // (the CS's field helpers: the VS's and the FS's registers lay their fields out the same, and tu6_emit_xs writes
        // every stage's through the VS's)
        const PrivateMemory pm = s.k.pvtmemSize != 0 ? b.privateMemory : PrivateMemory{};
        regs(stream, regObjFirstExec,
             {0, uint32_t(address), uint32_t(address >> 32), A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM(pm.perFiber), uint32_t(pm.address),
              uint32_t(pm.address >> 32), A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(pm.perSp) | (pm.perWave ? A6XX_SP_CS_PVT_MEM_SIZE_PERWAVEMEMLAYOUT : 0u)});
        reg(stream, regHwStackOffset, A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET(pm.perSp));
        loadIndirect(stream, s, ST6_SHADER, 0, std::min(s.k.instrlen, kInstrCacheSize), address);
        if (s.k.numImmediates != 0) {
            std::vector<uint32_t> imm(s.k.immediates, s.k.immediates + s.k.numImmediates);
            imm.resize((imm.size() + 3) & ~size_t(3), 0);
            loadDirect(stream, s, ST6_CONSTANTS, s.k.immediatesOffsetVec4, uint32_t(imm.size() / 4), imm.data(), uint32_t(imm.size()));
        }
        for (const PushedRange& r : ranges) {
            if (!r.words.empty()) {
                loadDirect(stream, s, ST6_CONSTANTS, r.offsetVec4, r.units, r.words.data(), uint32_t(r.words.size()));
            } else {
                loadIndirect(stream, s, ST6_CONSTANTS, r.offsetVec4, r.units, r.from);
            }
        }
        // the UBO descriptors from the packet, before the constants in Gallium's order (fd6_emit_user_consts :332-337;
        // the two loads do not overlap, so the order is the witness's and nothing more)
        if (!ubos.empty()) {
            loadDirect(stream, s, ST6_UBO, 0, uint32_t(ubos.size() / 2), ubos.data(), uint32_t(ubos.size()));
        }
    };
    if (o.programVs) {
        programOf(vs, vsBindings, vsRanges, vsUbos, vsProgram, spVsCtrlReg0(uint32_t(vs.k.maxReg + 1), uint32_t(vs.k.maxHalfReg + 1), vs.k.branchstackHw, vs.k.mergedRegs, vs.k.earlyPreamble),
                  REG_A6XX_SP_VS_CTRL_REG0, REG_A6XX_SP_VS_INSTRLEN, REG_A6XX_SP_VS_OBJ_FIRST_EXEC_OFFSET, REG_A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET);
    }
    // 10a. the VS's driver params, every draw since they are the draw's own (tu6_emit_vs_params, tu_cmd_buffer.cc:
    // 6135-6205: one vec4 of draw id, vertex offset, first instance, 0). The AIR front end reads Metal's [[base_vertex]]
    // as first_vertex and makes [[instance_id]] the hardware's zero-based id plus base_instance, so a VS with either
    // reads these; the vertex offset is the one VFD_INDEX_OFFSET adds, the base vertex of an indexed draw or the
    // vertex start of the other kind, as Vulkan's firstVertex is.
    if (vs.k.driverParamsOffsetVec4 >= 0) {
        const uint32_t params[4] = {0, draw.indexBuffer != nullptr ? uint32_t(draw.baseVertex) : draw.vertexStart, draw.baseInstance, 0};
        loadDirect(stream, vs, ST6_CONSTANTS, uint32_t(vs.k.driverParamsOffsetVec4), 1, params, 4);
    }
    if (o.programFs) {
        programOf(fs, fsBindings, fsRanges, fsUbos, fsProgram,
                  spFsCtrlReg0(uint32_t(fs.k.maxReg + 1), uint32_t(fs.k.maxHalfReg + 1), fs.k.branchstackHw, fsThreads, fs.totalIn != 0, fs.needFullQuad != 0,
                               fs.needPixlod != 0, fs.k.earlyPreamble, fs.k.mergedRegs),
                  REG_A6XX_SP_FS_CTRL_REG0, REG_A6XX_SP_FS_INSTRLEN, REG_A6XX_SP_FS_OBJ_FIRST_EXEC_OFFSET, REG_A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET);
    }
    // 10b. the FS's textures and samplers, Gallium's FS_TEX group in its order (fd6_texture.cc:593-687): the samplers into
    // SB6_FS_TEX as ST6_SHADER (16 bytes a unit), their pointer, the textures as ST6_CONSTANTS (64), their pointer, the
    // count; SP_FS_TEX_COUNT 0 without textures, so that no earlier owner's count is left (the register is context state).
    // SP_FS_CONFIG's NTEX and NSAMP (step 9) stay ir3's num_samp, as compute leaves SP_CS_CONFIG's.
    if (o.fsTextures) {
        if (fsTex.numTex != 0) {
            uint32_t w[3] = {loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_FS_TEX, fsTex.numSamp), CP_LOAD_STATE6_1_EXT_SRC_ADDR(uint32_t(table + kFsTexSampAt)),
                             CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(uint32_t((table + kFsTexSampAt) >> 32))};
            stream.packet(CP_LOAD_STATE6_FRAG, w, 3);
            reg64(stream, REG_A6XX_SP_FS_TEX_SAMP, table + kFsTexSampAt);
            uint32_t t[3] = {loadState6_0(0, ST6_CONSTANTS, SS6_INDIRECT, SB6_FS_TEX, fsTex.numTex), CP_LOAD_STATE6_1_EXT_SRC_ADDR(uint32_t(table + kFsTexConstAt)),
                             CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(uint32_t((table + kFsTexConstAt) >> 32))};
            stream.packet(CP_LOAD_STATE6_FRAG, t, 3);
            reg64(stream, REG_A6XX_SP_FS_TEX_CONST, table + kFsTexConstAt);
        }
        reg(stream, REG_A6XX_SP_FS_TEX_COUNT, fsTex.numTex);
    }
    // 10c. the VS's, the same group by CP_LOAD_STATE6_GEOM into SB6_VS_TEX (fd6_texture.cc's VS_TEX); its count as well
    // written 0 without textures, so that no earlier owner's stays
    if (o.vsTextures) {
        if (vsTex.numTex != 0) {
            uint32_t w[3] = {loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_VS_TEX, vsTex.numSamp), CP_LOAD_STATE6_1_EXT_SRC_ADDR(uint32_t(table + kVsTexSampAt)),
                             CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(uint32_t((table + kVsTexSampAt) >> 32))};
            stream.packet(CP_LOAD_STATE6_GEOM, w, 3);
            reg64(stream, REG_A6XX_SP_VS_TEX_SAMP, table + kVsTexSampAt);
            uint32_t t[3] = {loadState6_0(0, ST6_CONSTANTS, SS6_INDIRECT, SB6_VS_TEX, vsTex.numTex), CP_LOAD_STATE6_1_EXT_SRC_ADDR(uint32_t(table + kVsTexConstAt)),
                             CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(uint32_t((table + kVsTexConstAt) >> 32))};
            stream.packet(CP_LOAD_STATE6_GEOM, t, 3);
            reg64(stream, REG_A6XX_SP_VS_TEX_CONST, table + kVsTexConstAt);
        }
        reg(stream, REG_A6XX_SP_VS_TEX_COUNT, vsTex.numTex);
    }
    // 11. the vertex fetch with no attributes (tu6_emit_vs, tu_shader.cc:1902-1967; fd6 emit_vs_system_values,
    // fd6_program.cc:510-556): PC/VFD_MULTIVIEW_CNTL 0, VFD_CONTROL_0 0 (no fetches, no decodes), VFD_CONTROL_1 with the
    // VS's vertex and instance id registers (kRegidNone for the rest), the HS/DS/GS id registers all kRegidNone
    // (VFD_CONTROL_2..5 for NULL stages, :1972-2036), VFD_CONTROL_6 0 (no primitive id), VFD_INDEX_OFFSET = vertexStart
    // (added to the vertex id through the static VFD_ADD_OFFSET.VERTEX: Metal's [[vertex_id]] includes it), the instance
    // start 0, PC_RESTART_INDEX all ones (fd6_draw.cc:416-422)
    // With [[stage_in]] attributes: VFD_CONTROL_0 counts them, the decode and destination tables follow by attribute,
    // each used fetch by Metal buffer index (BASE, SIZE to the bound end, STRIDE); the 32 fetch sizes were zeroed at the
    // batch start, so an unused fetch reads nothing (fd6_emit.cc:1056-1059).
    if (o.vfd) {
        reg(stream, REG_A6XX_PC_MULTIVIEW_CNTL, 0);
        reg(stream, REG_A6XX_VFD_MULTIVIEW_CNTL, 0);
        regs(stream, REG_A6XX_VFD_CONTROL_0,
             {A6XX_VFD_CONTROL_0_FETCH_CNT(fetch.count) | A6XX_VFD_CONTROL_0_DECODE_CNT(fetch.count),
              vfdControl1(vs.vertexIdRegid, vs.instanceIdRegid, kRegidNone, kRegidNone), vfdControl2None(), vfdControl3None(), vfdControl4None(),
              vfdControl5None(), 0});
        if (fetch.count != 0) {
            std::vector<uint32_t> dec, dst;
            for (uint32_t loc = 0; loc < fetch.count; loc++) {
                dec.push_back(fetch.decode[loc]);
                dec.push_back(fetch.stepRate[loc]);
                dst.push_back(fetch.dest[loc]);
            }
            stream.registers(REG_A6XX_VFD_DECODE_INSTR, dec.data(), uint32_t(dec.size()));
            stream.registers(REG_A6XX_VFD_DEST_CNTL_INSTR, dst.data(), uint32_t(dst.size()));
            for (uint32_t bi = 0; bi < kMaxVertexBuffers; bi++) {
                if (!fetch.used[bi]) continue;
                regs(stream, regVfdFetchBase(bi), {uint32_t(fetch.base[bi]), uint32_t(fetch.base[bi] >> 32), fetch.size[bi], fetch.stride[bi]});
            }
        }
        // the vertex id's offset (an indexed draw's base vertex) and the first instance, as tu6_emit_vs_params writes
        // them (tu_cmd_buffer.cc:6178-6180): the fetch of per-instance attributes starts there, while the instance id
        // the VS reads stays zero based and Metal's base is added from the driver params below; the restart index the
        // index type's all-ones (vk_index_to_restart: 0xffff for 16-bit indices, tu_cmd_buffer.cc:2840-2862)
        regs(stream, REG_A6XX_VFD_INDEX_OFFSET, {draw.indexBuffer != nullptr ? uint32_t(draw.baseVertex) : draw.vertexStart, draw.baseInstance});
        reg(stream, REG_A6XX_PC_RESTART_INDEX, draw.indexBuffer != nullptr && draw.indexType == 0 ? 0xffffu : 0xffffffffu);
    }
    // 12. the VPC link (tu6_emit_vpc, tu_pipeline.cc:890-1070; fd6 emit_vpc, fd6_program.cc:558-908) from render::link's
    // result: SP_VS_OUT_REG two entries a register, SP_VS_VPC_DST_REG four locations a register, VPC_VS_PACK (stride,
    // position location, no point size), the clip and layer registers with every location 0xff and both _V2 copies,
    // GRAS_VS_CL_CNTL and GRAS_VS_LAYER_CNTL 0, PC_VS_OUT_CNTL's stride, SP_VS_PRIMITIVE_CNTL.OUT, the eight interp and
    // eight replace-mode registers all written (fd6's form, emit_interp_state :1365-1370; turnip writes none without
    // varyings and a stale PS_REPL from the window server's RECTLIST blit would corrupt the first varying draw),
    // PC_PS_CNTL 0, and stream-out off as tu6_setup_streamout writes it (tu_pipeline.cc:496-512; type-4 here, not
    // CP_CONTEXT_REG_BUNCH)
    if (o.vpc) {
        for (uint32_t i = 0; i < (link.cnt + 1) / 2; i++) {
            const Linkage::Var& a = link.var[2 * i];
            const bool hasB = 2 * i + 1 < link.cnt;
            const Linkage::Var& b = link.var[hasB ? 2 * i + 1 : 2 * i];
            reg(stream, regSpVsOutReg(i), spVsOutReg(a.regid, a.compmask, hasB ? b.regid : 0u, hasB ? b.compmask : 0u));
        }
        for (uint32_t i = 0; i < (link.cnt + 3) / 4; i++) {
            uint32_t loc[4] = {0, 0, 0, 0};
            for (uint32_t j = 0; j < 4 && 4 * i + j < link.cnt; j++) loc[j] = link.var[4 * i + j].loc;
            reg(stream, regSpVsVpcDstReg(i), spVsVpcDstReg(loc[0], loc[1], loc[2], loc[3]));
        }
        reg(stream, REG_A6XX_VPC_VS_PACK, vpcVsPack(link.maxLoc, link.positionLoc, link.psizeLoc));
        reg(stream, REG_A6XX_VPC_VS_CLIP_CNTL, vpcVsClipCntl(0));
        reg(stream, REG_A6XX_VPC_VS_CLIP_CNTL_V2, vpcVsClipCntl(0));
        reg(stream, REG_A6XX_GRAS_VS_CL_CNTL, grasVsClCntl(0, 0));
        reg(stream, REG_A6XX_PC_VS_OUT_CNTL, pcVsOutCntl(link.maxLoc, link.psizeLoc != kLocNone));
        reg(stream, REG_A6XX_SP_VS_PRIMITIVE_CNTL, spVsPrimitiveCntl(link.cnt));
        reg(stream, REG_A6XX_VPC_VS_LAYER_CNTL, vpcVsLayerCntl());
        reg(stream, REG_A6XX_VPC_VS_LAYER_CNTL_V2, vpcVsLayerCntl());
        reg(stream, REG_A6XX_GRAS_VS_LAYER_CNTL, 0);
        stream.registers(regVpcVaryingInterpMode(0), link.interpMode, kVpcVaryingRegs);
        stream.registers(regVpcVaryingPsReplMode(0), link.psReplMode, kVpcVaryingRegs);
        reg(stream, REG_A6XX_PC_PS_CNTL, 0);
        reg(stream, REG_A6XX_VPC_SO_CNTL, 0);
        reg(stream, REG_A6XX_VPC_SO_STREAM_CNTL, 0);
        reg(stream, REG_A6XX_PC_SO_STREAM_CNTL, 0);
    }
    // 13. the FS's inputs (tu6_emit_fs_inputs, tu_shader.cc:1627-1812; fd6 emit_fs_inputs, fd6_program.cc:921-1112):
    // SP_FS_PREFETCH_CNTL with no prefetch commands and IJ_WRITE_DISABLE when no perspective pixel barycentric is read;
    // the five HLSQ_CONTROL registers (PRIMALLOCTHRESHOLD 7 for this chip; the regids, kRegidNone where unread; the ij
    // registers in ir3_bary order into the register fields' order); HLSQ_FS_CNTL_0; GRAS_CNTL and RB_RENDER_CONTROL0
    // with the same IJ bits and COORD_MASK (IJ_LINEAR_PIXEL also when the FS reads its face or its coordinate: need_size,
    // :1747-1756), RB_RENDER_CONTROL0's UNK10 with varyings, RB_RENDER_CONTROL1 from the face/sample regids; the
    // sample registers 0; VPC_VAR_DISABLE[0..3] = ~varmask; VPC_CNTL_0 with NUMNONPOSVAR = total_in
    const bool needSize = validReg(fs.faceRegid) || fs.fragcoordCompmask != 0;
    const uint32_t ijbits = ijBits(ij, needSize, false);
    if (o.fsInputs) {
        {
            // with the prefetch commands after it in the same pkt4 (fd6 emit_fs_inputs, fd6_program.cc:947-971; A6XX keeps the
            // slots in the command, no SP_FS_BINDLESS_PREFETCH_CMD)
            uint32_t w[1 + kMaxSamplerPrefetch];
            w[0] = spFsPrefetchCntl(fs.numSamplerPrefetch, !validReg(ij[IJ_PERSP_PIXEL]), fs.prefetchEndOfQuad != 0);
            for (uint32_t i = 0; i < fs.numSamplerPrefetch; i++) {
                const SamplerPrefetch& pf = fs.prefetch[i];
                w[1 + i] = spFsPrefetchCmd(pf.src, pf.sampId, pf.texId, pf.dst, pf.wrmask, pf.half != 0, pf.cmd);
            }
            stream.registers(REG_A6XX_SP_FS_PREFETCH_CNTL, w, 1 + fs.numSamplerPrefetch);
        }
        regs(stream, REG_A6XX_HLSQ_CONTROL_1_REG,
             {hlsqControl1Reg(), hlsqControl2Reg(fs.faceRegid, fs.sampleIdRegid, fs.sampleMaskInRegid, ij[IJ_PERSP_CENTER_RHW]),
              hlsqControl3Reg(ij[IJ_PERSP_PIXEL], ij[IJ_LINEAR_PIXEL], ij[IJ_PERSP_CENTROID], ij[IJ_LINEAR_CENTROID]),
              hlsqControl4Reg(ij[IJ_PERSP_SAMPLE], ij[IJ_LINEAR_SAMPLE], fs.fragCoordRegid, validReg(fs.fragCoordRegid) ? fs.fragCoordRegid + 2 : kRegidNone),
              hlsqControl5Reg()});
        reg(stream, REG_A6XX_HLSQ_FS_CNTL_0, hlsqFsCntl0(fsThreads, fs.totalIn != 0));
        reg(stream, REG_A6XX_GRAS_CNTL, grasCntl(ijbits, fs.fragcoordCompmask));
        regs(stream, REG_A6XX_RB_RENDER_CONTROL0,
             {rbRenderControl0(ijbits, fs.fragcoordCompmask, fs.totalIn != 0),
              rbRenderControl1(validReg(fs.faceRegid), validReg(fs.sampleIdRegid), validReg(fs.sampleMaskInRegid), validReg(ij[IJ_PERSP_CENTER_RHW]))});
        reg(stream, REG_A6XX_RB_SAMPLE_CNTL, 0);
        reg(stream, REG_A6XX_GRAS_LRZ_PS_INPUT_CNTL, 0);
        reg(stream, REG_A6XX_GRAS_SAMPLE_CNTL, 0);
        uint32_t disable[kVpcVarDisableRegs];
        for (uint32_t i = 0; i < kVpcVarDisableRegs; i++) disable[i] = vpcVarDisable(link.varmask[i]);
        stream.registers(regVpcVarDisable(0), disable, kVpcVarDisableRegs);
        reg(stream, REG_A6XX_VPC_CNTL_0, vpcCntl0(fs.totalIn, kLocNone, fs.totalIn != 0, kLocNone));
    }
    // 14. the FS's outputs (tu6_emit_fs_outputs, tu_shader.cc:1816-1898; fd6 emit_fs_outputs :1114-1184 and
    // build_prog_fb_rast, fd6_emit.cc:302-350): SP_FS_OUTPUT_CNTL0 with no depth/sample-mask/stencil register, MRT 1 in
    // both CNTL1 copies, SP_FS_OUTPUT_REG[0] the colour register, RENDER_COMPONENTS 0xf for the one target in both
    // copies, RB_FS_OUTPUT_CNTL0 0 (nothing but colour is written)
    // (with MRT n and SP_FS_OUTPUT_REG[0 .. n-1] for an FS that writes n targets: the registers run on from CNTL1)
    if (o.fsOutputs) {
        // (the components of a slot drawn as unwritten -- no attachment for it in this pass -- taken out of the link's)
        uint32_t components = link.mrtComponents;
        for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
            if (fs.colorRegid[i] == kRegidNone) components &= ~(0xfu << (4 * i));
        }
        std::vector<uint32_t> out = {components, spFsOutputCntl0(kRegidNone, kRegidNone, kRegidNone), A6XX_SP_FS_OUTPUT_CNTL1_MRT(numRt)};
        for (uint32_t i = 0; i < numRt; i++) out.push_back(spFsOutputReg(fs.colorRegid[i], fs.colorHalf[i] != 0));
        stream.registers(REG_A6XX_SP_FS_RENDER_COMPONENTS, out.data(), uint32_t(out.size()));
        regs(stream, REG_A6XX_RB_FS_OUTPUT_CNTL0, {0, A6XX_RB_FS_OUTPUT_CNTL1_MRT(numRt), components});
    }
    // 15. the rasterizer (tu6_emit_rast, tu_pipeline.cc:3120-3218; fd6_rasterizer.cc:22-140): GRAS_CL_CNTL with Metal's
    // [0, 1] depth (ZERO_GB_SCALE_Z) and VP_CLIP_CODE_IGNORE (0xc0, turnip's for Vulkan); GRAS_SU_CNTL the draw's cull
    // (MTLCullMode Front/Back as CULL_FRONT/CULL_BACK) and its front face: FRONT_CW for Metal's Clockwise. Measured, not
    // reasoned: the negative y scale did suggest the opposite, and probe 9cu found every case inverted that way -- T1,
    // counter-clockwise in Metal's NDC, is removed by a back cull only with FRONT_CW set. The facing is judged before the
    // y flip; half-pixel rectangular lines (0x2010); filled polygons in VPC and PC;
    // PC_RASTER_CNTL and VPC_UNKNOWN_9107 0; point size 1 with turnip's limits; polygon offsets 0; the four shading-rate
    // registers 0 (has_legacy_pipeline_shading_rate); GRAS_SC_CNTL for sysmem (:3351-3380); PC_PRIMITIVE_CNTL_0 0 (the
    // first vertex provokes, no restart)
    if (o.rast) {
        reg(stream, REG_A6XX_GRAS_CL_CNTL, grasClCntl());
        reg(stream, REG_A6XX_GRAS_SU_CNTL, grasSuCntl(draw.cullMode == 1, draw.cullMode == 2, draw.winding == 0));
        reg(stream, REG_A6XX_VPC_POLYGON_MODE, A6XX_VPC_POLYGON_MODE_MODE(POLYMODE6_TRIANGLES));
        reg(stream, REG_A6XX_PC_POLYGON_MODE, A6XX_PC_POLYGON_MODE_MODE(POLYMODE6_TRIANGLES));
        reg(stream, REG_A6XX_PC_RASTER_CNTL, 0);
        reg(stream, REG_A6XX_VPC_UNKNOWN_9107, 0);
        regs(stream, REG_A6XX_GRAS_SU_POINT_MINMAX, {grasSuPointMinmax(1, 0xffc0), grasSuPointSize(16)});
        regs(stream, REG_A6XX_GRAS_SU_POLY_OFFSET_SCALE, {0, 0, 0});
        reg(stream, REG_A6XX_RB_UNKNOWN_8A00, 0);
        reg(stream, REG_A6XX_RB_UNKNOWN_8A10, 0);
        reg(stream, REG_A6XX_RB_UNKNOWN_8A20, 0);
        reg(stream, REG_A6XX_RB_UNKNOWN_8A30, 0);
        reg(stream, REG_A6XX_GRAS_SC_CNTL, grasScCntlSysmem());
        // primitive restart on for an indexed draw (Metal restarts at the all-ones index; tu6_draw_common's primitive_restart,
        // :5851-5862, which is off for a draw without indices, where the index register would be stale)
        reg(stream, REG_A6XX_PC_PRIMITIVE_CNTL_0, draw.indexBuffer != nullptr ? A6XX_PC_PRIMITIVE_CNTL_0_PRIMITIVE_RESTART : 0u);
    }
    // 16. the viewport and the scissors (tu6_emit_viewport, tu_pipeline.cc:2432-2540; tu6_emit_scissor :2657-2688):
    // GRAS_CL_VPORT[0]'s six floats with Metal's NEGATIVE y scale (viewportWords: NDC y up, row 0 at the top -- reasoning
    // from the convention, which the probe's orientation line measures), the viewport scissor as the viewport's integer
    // rectangle with BR inclusive, the Z clamps znear..zfar in both GRAS and RB, the guardband from the offsets and scales
    // (fd_calc_guardband; MAX_GB for a 64x64 viewport), the screen scissor as Metal's scissor rectangle (the whole target
    // when none), BR inclusive (both witnesses: the fd6 'off by one' in a6xx-draw.md was spurious)
    if (o.viewportScissor) {
        const ViewportWords v = viewportWords(vp.x, vp.y, vp.width, vp.height, vp.znear, vp.zfar);
        regs(stream, regGrasClVport(REG_A6XX_GRAS_CL_VPORT_XOFFSET, 0), {v.xoffset, v.xscale, v.yoffset, v.yscale, v.zoffset, v.zscale});
        const uint32_t vx0 = uint32_t(std::max(0.0, std::floor(vp.x))), vy0 = uint32_t(std::max(0.0, std::floor(vp.y)));   // (clamped to the window)
        const uint32_t vx1 = std::min(W, uint32_t(std::ceil(vp.x + vp.width))) - 1, vy1 = std::min(H, uint32_t(std::ceil(vp.y + vp.height))) - 1;
        regs(stream, regGrasScViewportScissor(REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL, 0), {scissorXY(vx0, vy0), scissorXY(vx1, vy1)});
        const float zmin = float(std::min(vp.znear, vp.zfar)), zmax = float(std::max(vp.znear, vp.zfar));
        regs(stream, regGrasClZClamp(REG_A6XX_GRAS_CL_Z_CLAMP_MIN, 0), {fui(zmin), fui(zmax)});
        reg(stream, REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ, guardbandClipAdj(vp.x + vp.width / 2, vp.width / 2, vp.y + vp.height / 2, -vp.height / 2));
        regs(stream, REG_A6XX_RB_Z_CLAMP_MIN, {fui(zmin), fui(zmax)});
        regs(stream, regGrasScScreenScissor(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL, 0), {scissorXY(sc.x, sc.y), scissorXY(sc.x + sc.width - 1, sc.y + sc.height - 1)});
    }
    // 17. blending off, depth and stencil off (tu_pipeline.cc:2980-3062, 3240-3331; fd6_blend.cc:45-150; fd6_zsa.cc):
    // SP_BLEND_CNTL UNK8 (both pipelines; r3d 0), RB_BLEND_CNTL with every sample and turnip's INDEPENDENT_BLEND,
    // RB_MRT_CONTROL[0] the four-component write mask alone (0x780: turnip's ROP_CLEAR in the inert ROP_CODE field, not
    // fd6's ROP_COPY), RB_MRT_BLEND_CONTROL[0] 0, the blend constant 0, the alpha test off, the depth and stencil
    // registers 0 (EARLY_Z depth plane as r3d; pipelines with depth off write LATE_Z), Z bounds 0..1
    if (o.blendZsa) {
        // (ENABLE_BLEND is a mask of the targets that blend, one bit each; RB_MRT_CONTROL / BLEND_CONTROL per target)
        reg(stream, REG_A6XX_SP_BLEND_CNTL, A6XX_SP_BLEND_CNTL_ENABLE_BLEND(blendMask) | A6XX_SP_BLEND_CNTL_UNK8);
        reg(stream, REG_A6XX_RB_BLEND_CNTL, rbBlendCntl(true) | A6XX_RB_BLEND_CNTL_ENABLE_BLEND(blendMask));
        for (uint32_t i = 0; i < numRt; i++) {
            regs(stream, regRbMrt(REG_A6XX_RB_MRT_CONTROL, i), {blends[i].mrtControl, blends[i].mrtBlendControl});
        }
        // the blend constants as floats (tu6_emit_blend_constants), RB_ALPHA_CONTROL 0 after them
        regs(stream, REG_A6XX_RB_BLEND_RED_F32, {fui(draw.blendColor[0]), fui(draw.blendColor[1]), fui(draw.blendColor[2]), fui(draw.blendColor[3]), 0});
        // the depth test (tu6_emit_depth_stencil, tu_pipeline.cc:3300-3332: Z_READ with the test, Z_WRITE only with it)
        // and the Z mode (tu6_build_depth_plane_z_mode, tu_cmd_buffer.cc:5538-5583: LATE_Z with the test off or a kill
        // that writes depth, EARLY_Z otherwise); with no depth attachment r3d's zeros and EARLY_Z, as measured
        const bool stencilWrites = st.enabled && (st.front.writeMask != 0 || st.back.writeMask != 0);
        const a6xx_ztest_mode zmode = pass.depth == nullptr ? A6XX_EARLY_Z
                                      : (!depthTest && !st.enabled) || (fs.hasKill && (draw.depth.write || stencilWrites)) ? A6XX_LATE_Z : A6XX_EARLY_Z;
        const uint32_t depthCntl = pass.depth == nullptr ? 0u
                                   : (depthTest ? A6XX_RB_DEPTH_CNTL_Z_TEST_ENABLE | A6XX_RB_DEPTH_CNTL_Z_READ_ENABLE : 0u) |
                                         (depthTest && draw.depth.write ? A6XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE : 0u) |
                                         A6XX_RB_DEPTH_CNTL_ZFUNC(adreno_compare_func(draw.depth.compare));
        regs(stream, REG_A6XX_RB_DEPTH_PLANE_CNTL, {A6XX_RB_DEPTH_PLANE_CNTL_Z_MODE(zmode), depthCntl});   // ..., RB_DEPTH_CNTL
        regs(stream, REG_A6XX_GRAS_SU_DEPTH_CNTL, {depthTest && pass.depth != nullptr ? A6XX_GRAS_SU_DEPTH_CNTL_Z_TEST_ENABLE : 0u,
                                                  st.enabled ? A6XX_GRAS_SU_STENCIL_CNTL_STENCIL_ENABLE : 0u});   // ..., GRAS_SU_STENCIL_CNTL
        reg(stream, REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL, A6XX_GRAS_SU_DEPTH_PLANE_CNTL_Z_MODE(zmode));
        // the stencil test (tu6_emit_stencil_*, tu_pipeline.cc: both faces enabled and read, each face's function and
        // three operations; the references, read masks and write masks front | back << 8)
        if (st.enabled) {
            const StencilFace &f = st.front, &bk = st.back;
            reg(stream, REG_A6XX_RB_STENCIL_CONTROL,
                A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE | A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE_BF | A6XX_RB_STENCIL_CONTROL_STENCIL_READ |
                    A6XX_RB_STENCIL_CONTROL_FUNC(adreno_compare_func(f.compare)) | A6XX_RB_STENCIL_CONTROL_FAIL(adreno_stencil_op(f.stencilFail)) |
                    A6XX_RB_STENCIL_CONTROL_ZPASS(adreno_stencil_op(f.pass)) | A6XX_RB_STENCIL_CONTROL_ZFAIL(adreno_stencil_op(f.depthFail)) |
                    A6XX_RB_STENCIL_CONTROL_FUNC_BF(adreno_compare_func(bk.compare)) | A6XX_RB_STENCIL_CONTROL_FAIL_BF(adreno_stencil_op(bk.stencilFail)) |
                    A6XX_RB_STENCIL_CONTROL_ZPASS_BF(adreno_stencil_op(bk.pass)) | A6XX_RB_STENCIL_CONTROL_ZFAIL_BF(adreno_stencil_op(bk.depthFail)));
            regs(stream, REG_A6XX_RB_STENCILREF, {(st.frontReference & 0xffu) | (st.backReference & 0xffu) << 8, (f.readMask & 0xffu) | (bk.readMask & 0xffu) << 8,
                                                  (f.writeMask & 0xffu) | (bk.writeMask & 0xffu) << 8});
        } else {
            reg(stream, REG_A6XX_RB_STENCIL_CONTROL, 0);
            regs(stream, REG_A6XX_RB_STENCILREF, {0, 0, 0});   // ..., RB_STENCILMASK, RB_STENCILWRMASK
        }
        regs(stream, REG_A6XX_RB_Z_BOUNDS_MIN, {fui(0.0f), fui(1.0f)});
    }
    // 18. the draw (r3d_run, tu_clear_blit.cc:1661-1670; tu_CmdDraw :6209-6227; fd6 draw_emit, fd6_draw.cc:170-180):
    // CP_DRAW_INDX_OFFSET's 3-word form, the indices generated (AUTO_INDEX), IGNORE_VISIBILITY as r3d (the pipelines'
    // USE_VISIBILITY under visCullUse: with CP_SET_VISIBILITY_OVERRIDE 1 either should draw; unmeasured), one instance
    // An indexed draw is the 7-word form (tu_CmdDrawIndexed, :6291-6297): DI_SRC_SEL_DMA with the index size, the
    // instances, the index count, FIRST_INDX 0 (the offset is in the base), the index buffer's address past the offset,
    // and MAX_INDICES, how many indices it holds from there (the CP reads none past it)
    // an occlusion query (turnip's emit_begin_occlusion_query, A6XX path): the word zeroed when the encoder first counts
    // into it, then the running sample count's snapshot before the draw; after it the second snapshot, a wait for both
    // to land, and the word += end - begin
    const bool counting = draw.visibilityMode != 0;
    const uint64_t snapshots = table + kTableVisibilityAt;
    uint64_t result = 0;
    if (counting) {
        result = draw.visibility->gpuAddress() + draw.visibilityOffset;
        if (draw.visibilityReset) {
            uint32_t w[4] = {uint32_t(result), uint32_t(result >> 32), 0, 0};
            stream.packet(pm4::kMemWrite, w, 4);
        }
        reg(stream, REG_A6XX_RB_SAMPLE_COUNT_CONTROL, A6XX_RB_SAMPLE_COUNT_CONTROL_COPY);
        reg64(stream, REG_A6XX_RB_SAMPLE_COUNT_ADDR, snapshots);
        event(stream, ZPASS_DONE);
    }
    if (indirect) {
        // tu_CmdDrawIndirect / tu_CmdDrawIndexedIndirect (tu_cmd_buffer.cc:6385-6423): one draw, the CP reading its
        // words, setting VFD_INDEX_OFFSET and VFD_INSTANCE_START_OFFSET, and writing the VS's params at DST_OFF
        const uint64_t args = draw.indirect->gpuAddress() + draw.indirectOffset;
        const uint32_t dstOff = vs.k.driverParamsOffsetVec4 > 0 ? uint32_t(vs.k.driverParamsOffsetVec4) : 0;
        if (draw.indexBuffer != nullptr) {
            const uint64_t base = draw.indexBuffer->gpuAddress() + draw.indexOffset;
            uint32_t w[9] = {drawIndxOffset0(prim, DI_SRC_SEL_DMA, o.visCullUse ? USE_VISIBILITY : IGNORE_VISIBILITY) |
                                 CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(draw.indexType == 0 ? INDEX4_SIZE_16_BIT : INDEX4_SIZE_32_BIT),
                             drawIndirectMulti1(INDIRECT_OP_INDEXED, dstOff), 1, uint32_t(base), uint32_t(base >> 32), uint32_t(maxIndices),
                             uint32_t(args), uint32_t(args >> 32), 20};
            stream.packet(CP_DRAW_INDIRECT_MULTI, w, 9);
        } else {
            uint32_t w[6] = {drawIndxOffset0(prim, DI_SRC_SEL_AUTO_INDEX, o.visCullUse ? USE_VISIBILITY : IGNORE_VISIBILITY),
                             drawIndirectMulti1(INDIRECT_OP_NORMAL, dstOff), 1, uint32_t(args), uint32_t(args >> 32), 16};
            stream.packet(CP_DRAW_INDIRECT_MULTI, w, 6);
        }
    } else if (draw.indexBuffer != nullptr) {
        const uint64_t base = draw.indexBuffer->gpuAddress() + draw.indexOffset;
        uint32_t w[7] = {drawIndxOffset0(prim, DI_SRC_SEL_DMA, o.visCullUse ? USE_VISIBILITY : IGNORE_VISIBILITY) |
                             CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(draw.indexType == 0 ? INDEX4_SIZE_16_BIT : INDEX4_SIZE_32_BIT),
                         draw.instanceCount, draw.vertexCount, 0, uint32_t(base), uint32_t(base >> 32), uint32_t(maxIndices)};
        stream.packet(CP_DRAW_INDX_OFFSET, w, 7);
    } else {
        uint32_t w[3] = {drawIndxOffset0(prim, DI_SRC_SEL_AUTO_INDEX, o.visCullUse ? USE_VISIBILITY : IGNORE_VISIBILITY), draw.instanceCount, draw.vertexCount};
        stream.packet(CP_DRAW_INDX_OFFSET, w, 3);
    }
    if (counting) {
        // ZPASS_DONE's copy lands when the RB has it, not when the CP moves on: the end word is set to all ones first and
        // waited on until it is something else, as turnip does, before the CP reads it (on the phone, without that wait
        // every result came out as the count plus one and the same 0x204cc00e78bf7385)
        const uint64_t end = snapshots + kVisibilityEndAt;
        uint32_t ones[4] = {uint32_t(end), uint32_t(end >> 32), ~0u, ~0u};
        stream.packet(pm4::kMemWrite, ones, 4);
        stream.packet(pm4::kWaitMemWrites, nullptr, 0);
        reg(stream, REG_A6XX_RB_SAMPLE_COUNT_CONTROL, A6XX_RB_SAMPLE_COUNT_CONTROL_COPY);
        reg64(stream, REG_A6XX_RB_SAMPLE_COUNT_ADDR, end);
        event(stream, ZPASS_DONE);
        waitForWordNot(stream, end, ~0u);
        uint32_t w[9] = {CP_MEM_TO_MEM_0_DOUBLE | CP_MEM_TO_MEM_0_NEG_C, uint32_t(result), uint32_t(result >> 32), uint32_t(result), uint32_t(result >> 32),
                         uint32_t(end), uint32_t(end >> 32), uint32_t(snapshots), uint32_t(snapshots >> 32)};
        stream.packet(CP_MEM_TO_MEM, w, 9);
        stream.packet(pm4::kWaitMemWrites, nullptr, 0);
    }
    if (whole) {
        passTail(stream, table, o, pass.depth != nullptr);
    }
    return true;
}
}  // namespace

bool encode(pm4::Stream& stream, const Program& program, const RenderPass& pass, const Draw& draw, const Bindings& vsBindings,
            const Bindings& fsBindings, uint64_t vsProgram, uint64_t fsProgram, uint64_t table, uint8_t* tableCpu, std::string* why,
            const Options& o) {
    return encodeDraw(stream, program, pass, draw, vsBindings, fsBindings, vsProgram, fsProgram, table, tableCpu, why, o, true);
}

bool encodeInPass(pm4::Stream& stream, const Program& program, const RenderPass& pass, const Draw& draw, const Bindings& vsBindings,
                  const Bindings& fsBindings, uint64_t vsProgram, uint64_t fsProgram, uint64_t table, uint8_t* tableCpu, std::string* why,
                  const Options& o) {
    return encodeDraw(stream, program, pass, draw, vsBindings, fsBindings, vsProgram, fsProgram, table, tableCpu, why, o, false);
}

bool encodeBegin(pm4::Stream& stream, const RenderPass& pass, uint64_t table, uint8_t* tableCpu, std::string* why, const Options& o) {
    if (!checkTarget(pass, why)) {
        return false;
    }
    ClearWords clears[kMaxColorOutputs];
    if (!clearsOf(pass, clears, why)) {
        return false;
    }
    tableStart(tableCpu);
    passStart(stream, windowOf(pass).first, windowOf(pass).second, table, o);
    clearAll(stream, pass, clears, table);
    zsClears(stream, pass, table);
    return true;
}

void encodeEnd(pm4::Stream& stream, const RenderPass& pass, uint64_t table, const Options& o) { passTail(stream, table, o, pass.depth != nullptr); }

bool encodeClear(pm4::Stream& stream, const RenderPass& pass, uint64_t table, uint8_t* tableCpu, std::string* why, const Options& o) {
    if (!checkTarget(pass, why)) {
        return false;
    }
    const bool depthClear = (pass.depth != nullptr && pass.depthLoad == LoadAction::kClear) || (pass.stencil != nullptr && pass.stencilLoad == LoadAction::kClear);
    if (!anyColorClear(pass) && !depthClear) {
        return refuse(why, "a pass without a draw and with load action " + n(uint32_t(pass.load)) + ": only a Clear writes anything then");
    }
    ClearWords clears[kMaxColorOutputs];
    if (!clearsOf(pass, clears, why)) {
        return false;
    }
    tableStart(tableCpu);
    passStart(stream, windowOf(pass).first, windowOf(pass).second, table, o);
    clearAll(stream, pass, clears, table);
    zsClears(stream, pass, table);
    passTail(stream, table, o, pass.depth != nullptr);
    return true;
}

uint32_t halfBitsOf(float f) { return halfOf(f); }

bool encodeBlit(pm4::Stream& stream, const Texture& src, uint32_t srcLevel, BlitRect s, const Texture& dst, uint32_t dstLevel, BlitRect d, bool linear,
                uint64_t table, uint8_t* tableCpu, std::string* why, const Options& o) {
    const FormatInfo &sf = src.info(), &df = dst.info();
    if (sf.depth || df.depth) {
        return refuse(why, std::string("a copy of a ") + (sf.depth ? sf.name : df.name) + " texture: a depth texture is tiled, the 2D copy is written for linear ones");
    }
    if (sf.fmt6 != df.fmt6 || sf.swap != df.swap) {
        return refuse(why, std::string("a copy from ") + sf.name + " to " + df.name + ": the two must be one pixel format (an sRGB pair is one)");
    }
    if (srcLevel >= src.levels() || dstLevel >= dst.levels()) {
        return refuse(why, "a copy from mip level " + n(srcLevel) + " of " + n(src.levels()) + " into level " + n(dstLevel) + " of " + n(dst.levels()));
    }
    const TextureLevel SL = src.level(srcLevel), DL = dst.level(dstLevel);
    auto inside = [](const BlitRect& r, const TextureLevel& l) { return r.w != 0 && r.h != 0 && uint64_t(r.x) + r.w <= l.width && uint64_t(r.y) + r.h <= l.height; };
    if (!inside(s, SL) || !inside(d, DL)) {
        return refuse(why, "a copy of " + n(s.w) + " x " + n(s.h) + " at (" + n(s.x) + ", " + n(s.y) + ") of a " + n(SL.width) + " x " + n(SL.height) + " level into " +
                               n(d.w) + " x " + n(d.h) + " at (" + n(d.x) + ", " + n(d.y) + ") of a " + n(DL.width) + " x " + n(DL.height) + " one: a rectangle outside its level or empty");
    }
    const bool scaled = s.w != d.w || s.h != d.h;
    // A multisample source is a resolve (turnip's resolve_sysmem through r2d): the engine reads its SAMPLES samples a
    // pixel and, for a float or unorm format, averages them (fdl6_view_init's samples_average); an integer one takes
    // sample 0, as Metal's resolve of an integer attachment does. Nothing is written into a multisample texture here.
    if (dst.samples() != 1) {
        return refuse(why, "a copy into a multisample texture: its samples are drawn, cleared or resolved from, not copied into");
    }
    if (src.samples() != 1 && (scaled || linear)) {
        return refuse(why, "a resolve that scales or filters: a multisample source is resolved pixel for pixel");
    }
    if (linear && scaled && sf.element != kElemFloat) {
        return refuse(why, std::string("a linear scale of ") + sf.name + ": an integer format is not filtered");
    }
    if (&src == &dst && srcLevel == dstLevel && s.x < d.x + d.w && d.x < s.x + s.w && s.y < d.y + d.h && d.y < s.y + s.h) {
        return refuse(why, "a copy within one level whose two rectangles overlap: the engine reads and writes the same memory in no order");
    }
    uint32_t ifmt;
    switch (df.fmt6) {
    case FMT6_A8_UNORM: case FMT6_8_UNORM: case FMT6_8_8_UNORM: case FMT6_8_8_8_8_UNORM: case FMT6_5_6_5_UNORM: case FMT6_5_5_5_1_UNORM:
        ifmt = R2D_UNORM8;
        break;
    case FMT6_16_FLOAT: case FMT6_16_16_FLOAT: case FMT6_16_16_16_16_FLOAT: ifmt = R2D_FLOAT16; break;
    case FMT6_32_FLOAT: case FMT6_32_32_FLOAT: case FMT6_32_32_32_32_FLOAT: ifmt = R2D_FLOAT32; break;
    case FMT6_32_UINT: case FMT6_32_32_32_32_UINT: ifmt = R2D_INT32; break;
    default: return refuse(why, std::string("a copy of ") + df.name + ": the 2D engine's intermediate format for it is not worked out here");
    }
    // an sRGB format filtered in linear space reads and writes through the sRGB conversion (and the UNORM8_SRGB
    // intermediate, r2d_setup_common); a copy, scaled or not, otherwise moves the stored bytes as they are
    const bool srgb = linear && scaled && sf.srgb;
    if (srgb) ifmt = R2D_UNORM8_SRGB;
    tableStart(tableCpu);
    passStart(stream, DL.width, DL.height, table, o);
    const uint32_t blit = rb2dBlitCntlCopy(df.fmt6, ifmt);
    reg(stream, REG_A6XX_RB_2D_UNKNOWN_8C01, 0);
    reg(stream, REG_A6XX_RB_2D_BLIT_CNTL, blit);
    reg(stream, REG_A6XX_GRAS_2D_BLIT_CNTL, blit);
    reg(stream, REG_A6XX_SP_2D_DST_FORMAT, sp2dDstFormat(df.fmt6, df.element == kElemInt, df.element == kElemUint, srgb));
    // the source rectangle then the destination one: six consecutive registers (BR inclusive in both)
    regs(stream, REG_A6XX_GRAS_2D_SRC_TL_X, {gras2dSrc(s.x), gras2dSrc(s.x + s.w - 1), gras2dSrc(s.y), gras2dSrc(s.y + s.h - 1), regXY(d.x, d.y),
                                            regXY(d.x + d.w - 1, d.y + d.h - 1)});
    const uint64_t from = src.levelAddress(srcLevel), to = dst.levelAddress(dstLevel);
    const a3xx_msaa_samples srcSamples = src.samples() == 4 ? MSAA_FOUR : src.samples() == 2 ? MSAA_TWO : MSAA_ONE;
    const uint32_t srcInfo = sp2dSrcInfo(sf.fmt6, sf.swap, srgb, linear && scaled) | A6XX_SP_PS_2D_SRC_INFO_SAMPLES(srcSamples) |
                             (srcSamples != MSAA_ONE && sf.element == kElemFloat ? A6XX_SP_PS_2D_SRC_INFO_SAMPLES_AVERAGE : 0u);
    regs(stream, REG_A6XX_SP_PS_2D_SRC_INFO, {srcInfo, sp2dSrcSize(SL.width, SL.height), uint32_t(from),
                                              uint32_t(from >> 32), sp2dSrcPitch(uint32_t(SL.pitch))});
    regs(stream, REG_A6XX_SP_PS_2D_SRC_FLAGS, {0, 0, 0});
    regs(stream, REG_A6XX_RB_2D_DST_INFO, {rb2dDstInfo(df.fmt6, TILE6_LINEAR, df.swap, srgb), uint32_t(to), uint32_t(to >> 32), A6XX_RB_2D_DST_PITCH(uint32_t(DL.pitch))});
    regs(stream, REG_A6XX_RB_2D_DST_FLAGS, {0, 0, 0});
    command1(stream, CP_BLIT, cpBlit0(BLIT_OP_SCALE));
    // what a Clear's fill does after it, and then the pass's tail: the copy is in memory when the next thing reads it
    eventTimestamp(stream, PC_CCU_FLUSH_COLOR_TS, table + kTableClearFlushAt, kClearFlushValue, false);
    waitForWord(stream, table + kTableClearFlushAt, kClearFlushValue);
    event(stream, PC_CCU_INVALIDATE_COLOR);
    stream.waitForIdle();
    passTail(stream, table, o, false);
    return true;
}

}  // namespace render

// a pass's words onto the command buffer's stream, packet by packet, behind a NOP that marks where an encoder began
// (for whoever reads the stream back)
void appendStream(pm4::Stream& to, const pm4::Stream& from) {
    const std::vector<uint32_t>& w = from.words();
    to.packet(pm4::kNop, nullptr, 0);
    for (size_t i = 0; i < w.size();) {
        pm4::Header h = pm4::decode(w[i]);
        if (h.kind == pm4::Header::kRegisters) {
            to.registers(h.reg, &w[i + 1], h.count);
        } else {
            to.packet(h.opcode, h.count ? &w[i + 1] : nullptr, h.count);
        }
        i += 1 + h.count;
    }
}

bool CommandBuffer::draw(const Program& program, const RenderPass& pass, const Draw& d, const Bindings& vsBindings, const Bindings& fsBindings,
                         const render::Options& options) {
    if (status_ > kEnqueued) {
        return fail("draw after commit");
    }
    std::string why;
    // the null texture's slot is the front end's, not the application's: the queue's texture goes there, over anything
    // bound at that index (an application cannot know the index, so whatever it bound there was not meant for it)
    Bindings vsWithNull, fsWithNull;
    const Bindings* vsb = &vsBindings;
    const Bindings* fsb = &fsBindings;
    std::shared_ptr<Texture> null;
    // And so does a sampled slot the application left empty: an uber shader (chronod's widget renderer) declares
    // textures that the branches a given draw takes never read, and Metal lets such a slot go unbound. What a read of
    // it gives is undefined in Metal; here it is the null texture's zero, as Apple's GPUs answer. Both stages alike.
    for (const auto& [k, given, with, at] : {std::tuple<const Kernel*, const Bindings*, Bindings*, const Bindings**>{&program.vs.k, &vsBindings, &vsWithNull, &vsb},
                                             {&program.fs.k, &fsBindings, &fsWithNull, &fsb}}) {
        for (uint32_t i = 0; i < kMaxTextureSlots && i < k->numTextureSlots; i++) {
            const KernelTexture& kt = k->textures[i];
            if (kt.kind != kTexSampled || kt.fbTarget != 0) continue;
            const bool unbound = i >= given->textures.size() || given->textures[i] == nullptr;
            if (!kt.isNull && !unbound) continue;
            if (null == nullptr && (null = queue_.nullTexture(&why)) == nullptr) {
                return fail(std::string("no null texture: ") + why);
            }
            if (*at != with) {
                *with = *given;
                *at = with;
            }
            if (with->textures.size() <= i) with->textures.resize(i + 1, nullptr);
            with->textures[i] = null.get();
            if (!kt.isNull) with->nullMask |= 1u << i;
        }
    }
    // private memory is the queue's too: a stage that wants some is given the queue's buffer, which the submission
    // names for writing (the stage spills into it)
    for (const auto& [k, from, to] : {std::tuple<const Kernel*, const Bindings**, Bindings*>{&program.vs.k, &vsb, &vsWithNull}, {&program.fs.k, &fsb, &fsWithNull}}) {
        if (k->pvtmemSize == 0) continue;
        PrivateMemory pm;
        std::shared_ptr<Buffer> scratch = queue_.privateMemory(k->pvtmemSize, k->pvtmemPerWave, &pm, &why);
        if (scratch == nullptr) {
            return fail(std::string("no private memory: ") + why);
        }
        if (*from != to) {
            *to = **from;
            *from = to;
        }
        to->privateMemory = pm;
        use(*scratch, true);
    }
    // the two programs' words, once per command buffer, and the draw's table, in pieces (command.hpp: Piece): a piece is
    // 256-byte aligned, so SP_xS_OBJ_START is 128-byte aligned as the ST6_SHADER preload wants it (tu_shader.cc:1313-1316)
    const uint64_t vsProgram = programAddress(program.vs.k, &why);
    if (vsProgram == 0) {
        return fail(std::string("no memory for the vertex program: ") + why);
    }
    const uint64_t fsProgram = programAddress(program.fs.k, &why);
    if (fsProgram == 0) {
        return fail(std::string("no memory for the fragment program: ") + why);
    }
    Piece table;
    if (!piece(render::kTableBytes, &table, &why)) {
        return fail(std::string("no memory for the draw's table: ") + why);
    }
    // A draw into the open pass's attachments joins it: no batch start, no pass begin and no tail of its own, which a
    // pass per draw spent on every draw (the CCU flush and the cache flush waited for, each time). It joins when the
    // attachments are the same textures, it asks for no Clear (a Clear begins a pass), and the options are the same.
    bool joins = open_ != nullptr && open_->pass.color0 == pass.color0 && open_->pass.level == pass.level && open_->pass.depth == pass.depth &&
                 open_->pass.stencil == pass.stencil && !(pass.stencil != nullptr && pass.stencilLoad == LoadAction::kClear) &&
                 pass.load != LoadAction::kClear &&
                 !(pass.depth != nullptr && pass.depthLoad == LoadAction::kClear) && pass.store == open_->pass.store &&
                 pass.depthStore == open_->pass.depthStore && std::memcmp(&open_->options, &options, sizeof options) == 0;
    for (uint32_t i = 0; joins && i < kMaxColorOutputs - 1; i++) {
        // (the attachments past 0 the same textures and stores, none asking for a Clear: the same pass)
        const ColorAttachment &a = pass.more[i], &b = open_->pass.more[i];
        joins = a.texture == b.texture && a.level == b.level && a.store == b.store && (a.texture == nullptr || a.load != LoadAction::kClear);
    }
    if (!joins) {
        closePass();
    }
    Piece passTable;
    pm4::Stream head, body;
    if (!joins) {
        if (!piece(render::kTableBytes, &passTable, &why)) {
            return fail(std::string("no memory for the pass's table: ") + why);
        }
        if (!render::encodeBegin(head, pass, passTable.gpuAddress(), passTable.contents(), &why, options)) {
            return fail(why);
        }
    }
    if (!render::encodeInPass(body, program, pass, d, *vsb, *fsb, vsProgram, fsProgram, table.gpuAddress(), table.contents(), &why, options)) {
        return fail(why);
    }
    if (!joins) {
        appendStream(stream_, head);
        open_ = std::make_unique<OpenPass>(OpenPass{pass, options, passTable.gpuAddress()});
        useAttachments(pass);
    }
    appendStream(stream_, body);
    // a stage with device buffers may write them (ldib's table is stib's): its whole table is used for writing
    for (const auto& [b, writes] : {std::pair<const Bindings*, bool>{vsb, program.vs.k.numSsbos != 0}, {fsb, program.fs.k.numSsbos != 0}}) {
        for (const Binding& x : b->buffers) {
            if (x.buffer != nullptr) {
                use(*x.buffer, writes);
            }
        }
    }
    if (d.indexBuffer != nullptr) {
        use(*d.indexBuffer, false);
    }
    if (d.indirect != nullptr) {
        use(*d.indirect, false);
    }
    if (d.visibilityMode != 0 && d.visibility != nullptr) {
        use(*d.visibility, true);   // the result word is written; the command buffer keeps the buffer until it is
    }
    // both stages' sampled textures: read, and alive until the GPU is done (the descriptors in the table point at them)
    for (const Bindings* b : {vsb, fsb}) {
        for (const Texture* t : b->textures) {
            if (t != nullptr) {
                use(*t->buffer(), false);
                keep(t->buffer());
            }
        }
    }
    return true;
}

bool CommandBuffer::blit(const Texture& src, uint32_t srcLevel, render::BlitRect s, const Texture& dst, uint32_t dstLevel, render::BlitRect d, bool linear,
                         const render::Options& options) {
    if (status_ > kEnqueued) {
        return fail("blit after commit");
    }
    closePass();   // the copy reads what the draws before it wrote: their pass's tail first
    std::string why;
    Piece table;
    if (!piece(render::kTableBytes, &table, &why)) {
        return fail(std::string("no memory for the copy's table: ") + why);
    }
    pm4::Stream added;
    if (!render::encodeBlit(added, src, srcLevel, s, dst, dstLevel, d, linear, table.gpuAddress(), table.contents(), &why, options)) {
        return fail(why);
    }
    appendStream(stream_, added);
    use(*src.buffer(), false);
    use(*dst.buffer(), true);
    keep(src.buffer());
    keep(dst.buffer());
    return true;
}

bool CommandBuffer::generateMipmaps(const Texture& t, const render::Options& options) {
    for (uint32_t l = 1; l < t.levels(); l++) {
        const TextureLevel a = t.level(l - 1), b = t.level(l);
        if (!blit(t, l - 1, render::BlitRect{0, 0, a.width, a.height}, t, l, render::BlitRect{0, 0, b.width, b.height}, true, options)) {
            return false;
        }
    }
    return true;
}

// every attachment of a pass, used for writing and kept until completion
void CommandBuffer::useAttachments(const RenderPass& pass) {
    for (const Texture* t : {pass.color0, pass.depth}) {
        if (t == nullptr) continue;
        use(*t->buffer(), true);
        keep(t->buffer());
    }
    for (const ColorAttachment& a : pass.more) {
        if (a.texture == nullptr) continue;
        use(*a.texture->buffer(), true);
        keep(a.texture->buffer());
    }
}

// the open pass's tail: what puts its colour and depth in memory, before anything else is recorded or the buffer runs
void CommandBuffer::closePass() {
    if (open_ == nullptr) {
        return;
    }
    pm4::Stream tail;
    render::encodeEnd(tail, open_->pass, open_->table, open_->options);
    appendStream(stream_, tail);
    open_.reset();
}

bool CommandBuffer::clear(const RenderPass& pass, const render::Options& options) {
    if (status_ > kEnqueued) {
        return fail("clear after commit");
    }
    closePass();
    std::string why;
    Piece table;
    if (!piece(render::kTableBytes, &table, &why)) {
        return fail(std::string("no memory for the pass's table: ") + why);
    }
    pm4::Stream added;
    if (!render::encodeClear(added, pass, table.gpuAddress(), table.contents(), &why, options)) {
        return fail(why);
    }
    appendStream(stream_, added);
    useAttachments(pass);
    return true;
}

}  // namespace hakios::metal
