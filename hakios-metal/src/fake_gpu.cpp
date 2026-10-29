#include "hakios-metal/fake_gpu.hpp"

#include <cerrno>
#include <cmath>
#include <chrono>
#include <cstring>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/pm4.hpp"

namespace hakios::metal {

namespace {
// The fake GPU reads a header with arithmetic of its own and not with pm4::decode: while the two were one function an
// encoder whose parity bit was even passed every test here, and only the comparison with Mesa's formula (pm4_test.cpp)
// said so. The command processor does not share the encoder's code either. Odd parity: the bit is 1 when the field has
// an even number of ones, so that field and bit together have an odd count.
uint32_t oddBit(uint32_t field) { return (__builtin_popcount(field) & 1) ^ 1u; }

pm4::Header readHeader(uint32_t w) {
    pm4::Header h;
    switch (w >> 28) {
    case 4:
        h.count = w & 0x7f;
        h.reg = (w >> 8) & 0x3ffff;
        if (((w >> 7) & 1) == oddBit(h.count) && ((w >> 27) & 1) == oddBit(h.reg)) {
            h.kind = pm4::Header::kRegisters;
        }
        break;
    case 7:
        h.count = w & 0x3fff;
        h.opcode = (w >> 16) & 0x7f;
        if (((w >> 15) & 1) == oddBit(h.count) && ((w >> 23) & 1) == oddBit(h.opcode)) {
            h.kind = pm4::Header::kCommand;
        }
        break;
    default:
        break;
    }
    return h;
}

std::string text(const char* format, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
    char buffer[160];
    snprintf(buffer, sizeof buffer, format, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    return buffer;
}

// a field read back the way the registers hold it, from a6xx.hpp's masks (the fake decodes; the encoder encodes)
uint32_t fieldOf(uint32_t word, uint32_t mask, uint32_t shift) { return (word & mask) >> shift; }

// the pixel size of the formats this model knows (texture.hpp's eleven, by their FMT: fd6_format_table.c); 0 for another
uint32_t bytesPerPixel(uint32_t fmt) {
    switch (fmt) {
    case a6xx::FMT6_8_UNORM: return 1;
    case a6xx::FMT6_8_8_UNORM: return 2;
    case a6xx::FMT6_8_8_8_8_UNORM: return 4;
    case a6xx::FMT6_32_FLOAT: return 4;
    case a6xx::FMT6_32_UINT: return 4;
    case a6xx::FMT6_16_16_16_16_FLOAT: return 8;
    case a6xx::FMT6_32_32_32_32_FLOAT: return 16;
    case a6xx::FMT6_32_32_32_32_UINT: return 16;
    default: return 0;
    }
}

bool allZero(const uint32_t* d, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (d[i] != 0) return false;
    }
    return true;
}
}  // namespace

FakeGpu::FakeGpu(bool async, uint32_t delayUs, bool recovers) : async_(async), recovers_(recovers), delayUs_(delayUs) {
    if (async_) {
        thread_ = std::thread([this] { worker(); });
    }
}

FakeGpu::~FakeGpu() {
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
        paused_ = false;
    }
    changed_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

int64_t FakeGpu::nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

FakeGpu::Stats FakeGpu::stats() {
    std::lock_guard<std::mutex> g(lock_);
    return stats_;
}

uint32_t FakeGpu::liveBuffers() {
    std::lock_guard<std::mutex> g(lock_);
    return uint32_t(buffers_.size());
}

void FakeGpu::pause() {
    std::lock_guard<std::mutex> g(lock_);
    paused_ = true;
}

void FakeGpu::resume() {
    {
        std::lock_guard<std::mutex> g(lock_);
        paused_ = false;
    }
    changed_.notify_all();
}

void* FakeGpu::map(uint32_t handle, uint64_t size) {
    std::lock_guard<std::mutex> g(lock_);
    auto b = buffers_.find(handle);
    if (b == buffers_.end() || size > b->second.bytes.size()) {
        return nullptr;
    }
    return b->second.bytes.data();
}

void FakeGpu::unmap(uint32_t, void*) {}

uint8_t* FakeGpu::at(uint64_t iova, uint64_t length) {
    for (auto& [handle, b] : buffers_) {
        if (iova >= b.iova && length <= b.bytes.size() && iova - b.iova <= b.bytes.size() - length) {
            return b.bytes.data() + (iova - b.iova);
        }
    }
    return nullptr;
}

void FakeGpu::fault(const std::string& why) {
    stats_.faults++;
    stats_.lastFault = why;
}

uint32_t FakeGpu::reg(uint32_t r) const {
    auto it = regs_.find(r);
    return it == regs_.end() ? 0 : it->second;
}

uint64_t FakeGpu::reg64(uint32_t r) const { return reg(r) | uint64_t(reg(r + 1)) << 32; }

// CP_LOAD_STATE6_FRAG, _GEOM or the plain CP_LOAD_STATE6: a program, constants or descriptors into one stage's state
// block. Which opcode may load which block is Mesa's pairing (tu6_stage2opcode, tu_shader.cc:1349; fd6_stage2opcode,
// fd6_emit.h:331-334): the compute blocks by _FRAG (computerator, fd6_compute), the vertex stage's by _GEOM, the
// fragment stage's by _FRAG; the 3D IBO table (SB6_IBO, plain CP_LOAD_STATE6) has no non-bindless witness in Mesa 25.0.7
// and is not taken.
bool FakeGpu::loadState(uint32_t opcode, const uint32_t* p, uint32_t count) {
    using namespace a6xx;
    const char* op = opcode == CP_LOAD_STATE6_FRAG ? "CP_LOAD_STATE6_FRAG" : opcode == CP_LOAD_STATE6_GEOM ? "CP_LOAD_STATE6_GEOM" : "CP_LOAD_STATE6";
    if (count < 3) {
        fault(std::string(op) + " shorter than its three words");
        return false;
    }
    uint32_t w0 = p[0];
    uint32_t dstOff = fieldOf(w0, CP_LOAD_STATE6_0_DST_OFF__MASK, CP_LOAD_STATE6_0_DST_OFF__SHIFT);
    uint32_t type = fieldOf(w0, CP_LOAD_STATE6_0_STATE_TYPE__MASK, CP_LOAD_STATE6_0_STATE_TYPE__SHIFT);
    uint32_t src = fieldOf(w0, CP_LOAD_STATE6_0_STATE_SRC__MASK, CP_LOAD_STATE6_0_STATE_SRC__SHIFT);
    uint32_t block = fieldOf(w0, CP_LOAD_STATE6_0_STATE_BLOCK__MASK, CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT);
    uint32_t units = fieldOf(w0, CP_LOAD_STATE6_0_NUM_UNIT__MASK, CP_LOAD_STATE6_0_NUM_UNIT__SHIFT);
    StageLoads* xs = nullptr;   // the graphics stage the block belongs to; nullptr for the compute stage's blocks
    const char* stage = "compute";
    switch (block) {
    case SB6_CS_SHADER:
    case SB6_CS_TEX:
        if (opcode != CP_LOAD_STATE6_FRAG) {
            fault(std::string(op) + text(" for state block %llu: the compute stage's loads go by CP_LOAD_STATE6_FRAG", block));
            return false;
        }
        break;
    case SB6_VS_SHADER:
    case SB6_VS_TEX:
        if (opcode != CP_LOAD_STATE6_GEOM) {
            fault(std::string(op) + text(" for state block %llu: the vertex stage's loads go by CP_LOAD_STATE6_GEOM (tu6_stage2opcode)", block));
            return false;
        }
        xs = &vs_;
        stage = "vertex";
        break;
    case SB6_FS_SHADER:
    case SB6_FS_TEX:
        if (opcode != CP_LOAD_STATE6_FRAG) {
            fault(std::string(op) + text(" for state block %llu: the fragment stage's loads go by CP_LOAD_STATE6_FRAG (tu6_stage2opcode)", block));
            return false;
        }
        xs = &fs_;
        stage = "fragment";
        break;
    case SB6_IBO:
        fault("a load into the 3D IBO table (SB6_IBO): no Mesa driver drives it non-bindless, not what this GPU model runs");
        return false;
    default:
        fault(std::string(op) + text(" for state block %llu: not one this GPU model runs", block));
        return false;
    }
    // what a unit is depends on the block as well as the type: in a shader block ST6_SHADER is 16 instructions (128
    // bytes), ST6_CONSTANTS a vec4, ST6_IBO a 64-byte descriptor; in a texture block ST6_SHADER is a 16-byte sampler
    // descriptor and ST6_CONSTANTS a 64-byte texture descriptor (Gallium's fd6_texture.cc:593-687 is the only reading of
    // the block's two types, and nothing else may be loaded into it)
    bool texBlock = block == SB6_CS_TEX || block == SB6_VS_TEX || block == SB6_FS_TEX;
    if (texBlock && type != ST6_SHADER && type != ST6_CONSTANTS) {
        fault(text("a load of type %llu into the ", type) + stage + " texture block (SB6_" + (xs == nullptr ? "CS" : xs == &vs_ ? "VS" : "FS") +
              "_TEX): not samplers (ST6_SHADER) or textures (ST6_CONSTANTS)");
        return false;
    }
    if (xs != nullptr && !texBlock && type == ST6_IBO) {
        fault(std::string("an IBO load into the ") + stage + " shader block: the 3D pipeline's IBOs go through SB6_IBO, which this GPU model does not run");
        return false;
    }
    uint32_t unitBytes = texBlock ? (type == ST6_SHADER ? 16 : 64) : type == ST6_SHADER ? 128 : type == ST6_CONSTANTS ? 16 : type == ST6_UBO ? 8 : 64;
    if (!texBlock && type == ST6_SHADER && units > kInstrCacheSize) {
        fault(text("a program preload of %llu units: 128 hangs this GPU, the instruction cache holds %llu", units, kInstrCacheSize));
        return false;
    }
    uint64_t from = 0;
    if (src == SS6_DIRECT) {
        if (type == ST6_SHADER || texBlock) {
            fault(texBlock ? "a texture or sampler table loaded from the packet itself: not what this GPU model runs"
                           : "a program loaded from the packet itself: not what this GPU model runs");
            return false;
        }
        if (count != 3 + units * unitBytes / 4) {
            fault(text("a direct load of %llu units carries %llu words, not %llu", units, count - 3, units * unitBytes / 4));
            return false;
        }
    } else if (src == SS6_INDIRECT) {
        if (count != 3) {
            fault(text("an indirect load with %llu words after its header, not 3", count));
            return false;
        }
        from = (p[1] & CP_LOAD_STATE6_1_EXT_SRC_ADDR__MASK) | uint64_t(p[2]) << 32;
        if (at(from, uint64_t(units) * unitBytes) == nullptr) {
            fault(text("a load of %llu units from %#llx: outside every buffer", units, from));
            return false;
        }
    } else {
        fault("a bindless or UBO-sourced load: not what this GPU model runs");
        return false;
    }
    if (texBlock) {
        bool* loaded = type == ST6_SHADER ? (xs ? &xs->sampLoaded : &cs_.sampLoaded) : (xs ? &xs->texLoaded : &cs_.texLoaded);
        uint64_t* at_ = type == ST6_SHADER ? (xs ? &xs->sampFrom : &cs_.sampFrom) : (xs ? &xs->texFrom : &cs_.texFrom);
        uint32_t* n_ = type == ST6_SHADER ? (xs ? &xs->sampUnits : &cs_.sampUnits) : (xs ? &xs->texUnits : &cs_.texUnits);
        *loaded = true;
        *at_ = from;
        *n_ = units;
        return true;
    }
    switch (type) {
    case ST6_SHADER:
        (xs ? xs->programLoaded : cs_.programLoaded) = true;
        (xs ? xs->programFrom : cs_.programFrom) = from;
        (xs ? xs->programUnits : cs_.programUnits) = units;
        break;
    case ST6_CONSTANTS:
        (xs ? xs->consts : cs_.consts).push_back(ConstLoad{dstOff, units, src == SS6_DIRECT});
        break;
    case ST6_IBO:
        cs_.ibosLoaded = true;
        cs_.ibosFrom = from;
        cs_.iboUnits = units;
        break;
    default:
        break;   // ST6_UBO: taken, not checked further
    }
    return true;
}

// A 2D descriptor of a linear texture or image, as fdl6_view_init writes one (fd6_view.c:232-250 for the sampled view,
// 383-397 for the storage one): a format this model knows, a width and height of at least 1, a pitch that holds a row
// and is a multiple of 64 bytes (of 64 << PITCHALIGN for the sampled view, which carries the alignment the layout chose;
// the storage view carries no PITCHALIGN), a base 64-byte aligned (fdl6_layout's base_align), the rows inside a buffer
// counted to a multiple of 4 as the layout counts them for its last level (fd6_layout.c:249-250: the TP may over-fetch
// them), one level and one layer.
bool FakeGpu::check2d(const uint32_t* d, const char* what, uint32_t i, bool sampled) {
    using namespace a6xx;
    const std::string who = std::string(what) + " " + std::to_string(i);
    uint32_t fmt = fieldOf(d[0], A6XX_TEX_CONST_0_FMT__MASK, A6XX_TEX_CONST_0_FMT__SHIFT);
    uint32_t bpp = bytesPerPixel(fmt);
    if (bpp == 0) {
        fault(who + text(": FMT %llu is not one this GPU model knows", fmt));
        return false;
    }
    uint32_t width = fieldOf(d[1], A6XX_TEX_CONST_1_WIDTH__MASK, A6XX_TEX_CONST_1_WIDTH__SHIFT);
    uint32_t height = fieldOf(d[1], A6XX_TEX_CONST_1_HEIGHT__MASK, A6XX_TEX_CONST_1_HEIGHT__SHIFT);
    if (width == 0 || height == 0) {
        fault(who + text(": %llu x %llu pixels", width, height));
        return false;
    }
    uint64_t pitch = fieldOf(d[2], A6XX_TEX_CONST_2_PITCH__MASK, A6XX_TEX_CONST_2_PITCH__SHIFT);
    uint32_t pitchalign = sampled ? fieldOf(d[2], A6XX_TEX_CONST_2_PITCHALIGN__MASK, A6XX_TEX_CONST_2_PITCHALIGN__SHIFT) : 0;
    uint64_t unit = 64ull << pitchalign;
    if (pitch < uint64_t(width) * bpp || pitch % unit != 0) {
        fault(who + text(": pitch %llu for rows of %llu pixels of %llu bytes: shorter than a row or not a multiple of ", pitch, width, bpp) +
              text("%llu bytes (64 << PITCHALIGN %llu)", unit, pitchalign));
        return false;
    }
    uint64_t base = d[4] | uint64_t(fieldOf(d[5], A6XX_TEX_CONST_5_BASE_HI__MASK, A6XX_TEX_CONST_5_BASE_HI__SHIFT)) << 32;
    uint64_t rows = (uint64_t(height) + 3) & ~uint64_t(3);
    uint64_t reach = (rows - 1) * pitch + uint64_t(width) * bpp;
    if (base % 64 != 0 || at(base, reach) == nullptr) {
        fault(who + text(": %llu rows (%llu counted) of %llu bytes", height, rows, pitch) +
              text(" at %#llx: outside every buffer or not 64-byte aligned", base));
        return false;
    }
    if (sampled && (fieldOf(d[0], A6XX_TEX_CONST_0_MIPLVLS__MASK, A6XX_TEX_CONST_0_MIPLVLS__SHIFT) != 0 ||
                    fieldOf(d[0], A6XX_TEX_CONST_0_SAMPLES__MASK, A6XX_TEX_CONST_0_SAMPLES__SHIFT) != 0)) {
        fault(who + ": more than one level or sample: not what this GPU model runs");
        return false;
    }
    uint32_t depth = fieldOf(d[5], A6XX_TEX_CONST_5_DEPTH__MASK, A6XX_TEX_CONST_5_DEPTH__SHIFT);
    if (depth != 1) {
        fault(who + text(": DEPTH %llu, not the one layer this GPU model runs", depth));
        return false;
    }
    return true;
}

// A sampler descriptor: the last word has no fields and both Mesa drivers write 0; the wrap modes are a6xx_tex_clamp's
// (0 to 4); a border clamp (CLAMP_TO_BORDER, 3) reads entry BCOLOR of the table at SP_TP_BORDER_COLOR_BASE_ADDR, so
// that register must have been written (Gallium writes it at every batch start) and point at an entry inside a buffer.
bool FakeGpu::checkSampler(const uint32_t* d, uint32_t i) {
    using namespace a6xx;
    if (d[3] != 0) {
        fault(text("sampler %llu: dword 3 is %#llx, and it has no fields", i, d[3]));
        return false;
    }
    uint32_t wraps[3] = {fieldOf(d[0], A6XX_TEX_SAMP_0_WRAP_S__MASK, A6XX_TEX_SAMP_0_WRAP_S__SHIFT),
                         fieldOf(d[0], A6XX_TEX_SAMP_0_WRAP_T__MASK, A6XX_TEX_SAMP_0_WRAP_T__SHIFT),
                         fieldOf(d[0], A6XX_TEX_SAMP_0_WRAP_R__MASK, A6XX_TEX_SAMP_0_WRAP_R__SHIFT)};
    bool border = false;
    for (uint32_t w : wraps) {
        if (w > A6XX_TEX_MIRROR_CLAMP) {
            fault(text("sampler %llu: wrap mode %llu is not one a6xx_tex_clamp names", i, w));
            return false;
        }
        border = border || w == A6XX_TEX_CLAMP_TO_BORDER;
    }
    if (border) {
        if (regs_.find(REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR) == regs_.end()) {
            fault(text("sampler %llu: a border-clamping sampler with no border colour table (SP_TP_BORDER_COLOR_BASE_ADDR was never written)", i));
            return false;
        }
        uint64_t table = reg64(REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR);
        uint64_t entry = table + uint64_t(fieldOf(d[2], A6XX_TEX_SAMP_2_BCOLOR__MASK, A6XX_TEX_SAMP_2_BCOLOR__SHIFT)) * kBorderColorAlign;
        if (table % kBorderColorAlign != 0 || at(entry, kBorderColorAlign) == nullptr) {
            fault(text("sampler %llu: its border colour entry at %#llx: outside every buffer or the table not 128-byte aligned", i, entry));
            return false;
        }
    }
    return true;
}

// CP_EXEC_CS: the dispatch itself. Everything the registers and the loads say about it has to hold together; what
// the real GPU does with state that does not is hang, and that is what a fault stands for here.
bool FakeGpu::execCs(const uint32_t* p, uint32_t count) {
    using namespace a6xx;
    if (count != 4) {
        fault(text("CP_EXEC_CS of %llu words, not 4", count));
        return false;
    }
    uint32_t config = reg(REG_A6XX_SP_CS_CONFIG), hlsq = reg(REG_A6XX_HLSQ_CS_CNTL);
    if ((config & A6XX_SP_CS_CONFIG_ENABLED) == 0 || (hlsq & A6XX_HLSQ_CS_CNTL_ENABLED) == 0) {
        fault("CP_EXEC_CS with the compute stage not enabled in SP_CS_CONFIG and HLSQ_CS_CNTL");
        return false;
    }
    uint32_t instrlen = reg(REG_A6XX_SP_CS_INSTRLEN);
    uint64_t program = reg64(REG_A6XX_SP_CS_OBJ_START);
    if (instrlen == 0) {
        fault("CP_EXEC_CS with SP_CS_INSTRLEN 0");
        return false;
    }
    if (program % kSpCsObjStartAlign != 0 || at(program, uint64_t(instrlen) * 128) == nullptr) {
        fault(text("the program at SP_CS_OBJ_START %#llx, %llu units: outside every buffer or not 32-byte aligned", program, instrlen));
        return false;
    }
    if (!cs_.programLoaded || cs_.programFrom != program) {
        fault("CP_EXEC_CS without the program preloaded from where SP_CS_OBJ_START points");
        return false;
    }
    if (cs_.programUnits > instrlen) {
        fault(text("a preload of %llu units for a program of %llu", cs_.programUnits, instrlen));
        return false;
    }
    uint32_t constlen = fieldOf(hlsq, A6XX_HLSQ_CS_CNTL_CONSTLEN__MASK, A6XX_HLSQ_CS_CNTL_CONSTLEN__SHIFT) << A6XX_HLSQ_CS_CNTL_CONSTLEN__SCALE;
    for (const ConstLoad& c : cs_.consts) {
        if (c.dstOffVec4 + c.units > constlen) {
            fault(text("constants at vec4 %llu, %llu of them: past the program's constlen of %llu", c.dstOffVec4, c.units, constlen));
            return false;
        }
    }
    uint32_t ctrl0 = reg(REG_A6XX_SP_CS_CTRL_REG0);
    uint32_t spThreads = fieldOf(ctrl0, A6XX_SP_CS_CTRL_REG0_THREADSIZE__MASK, A6XX_SP_CS_CTRL_REG0_THREADSIZE__SHIFT);
    uint32_t hlsqThreads = fieldOf(reg(REG_A6XX_HLSQ_CS_CNTL_1), A6XX_HLSQ_CS_CNTL_1_THREADSIZE__MASK, A6XX_HLSQ_CS_CNTL_1_THREADSIZE__SHIFT);
    if (spThreads != hlsqThreads) {
        fault("SP_CS_CTRL_REG0 and HLSQ_CS_CNTL_1 disagree about the thread size");
        return false;
    }
    if (reg(REG_A6XX_HLSQ_CS_CNTL_0) != reg(REG_A6XX_SP_CS_CNTL_0) || reg(REG_A6XX_HLSQ_CS_CNTL_1) != reg(REG_A6XX_SP_CS_CNTL_1)) {
        fault("the HLSQ's and the SP's copies of the id registers (CS_CNTL_0, CS_CNTL_1) differ");
        return false;
    }
    if (reg(REG_A6XX_SP_CS_CTRL_REG1) != reg(REG_A6XX_HLSQ_CS_CTRL_REG1)) {
        fault("SP_CS_CTRL_REG1 and HLSQ_CS_CTRL_REG1 differ about shared memory and the constant split");
        return false;
    }
    uint32_t nd0 = reg(REG_A6XX_HLSQ_CS_NDRANGE_0);
    uint32_t dim = fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM__MASK, A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM__SHIFT);
    uint32_t local[3] = {fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__MASK, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__SHIFT) + 1,
                         fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__MASK, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__SHIFT) + 1,
                         fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__MASK, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__SHIFT) + 1};
    if (dim == 0) {
        fault("HLSQ_CS_NDRANGE_0 with a kernel dimension of 0");
        return false;
    }
    if (uint64_t(local[0]) * local[1] * local[2] > 1024) {
        fault(text("a threadgroup of %llu x %llu x %llu threads: more than the 1024 the GPU runs", local[0], local[1], local[2]));
        return false;
    }
    const uint32_t* groups = p + 1;
    uint32_t global[3] = {reg(REG_A6XX_HLSQ_CS_NDRANGE_1), reg(REG_A6XX_HLSQ_CS_NDRANGE_3), reg(REG_A6XX_HLSQ_CS_NDRANGE_5)};
    for (int i = 0; i < 3; i++) {
        if (groups[i] == 0) {
            fault("CP_EXEC_CS with 0 work groups along an axis");
            return false;
        }
        if (global[i] != local[i] * groups[i]) {
            fault(text("HLSQ_CS_NDRANGE global size %llu is not the local size %llu times the %llu groups CP_EXEC_CS asks for", global[i],
                       local[i], groups[i]));
            return false;
        }
    }
    uint32_t nibo = fieldOf(config, A6XX_SP_CS_CONFIG_NIBO__MASK, A6XX_SP_CS_CONFIG_NIBO__SHIFT);
    if (nibo != reg(REG_A6XX_SP_CS_IBO_COUNT)) {
        fault(text("SP_CS_CONFIG names %llu IBOs and SP_CS_IBO_COUNT %llu", nibo, reg(REG_A6XX_SP_CS_IBO_COUNT)));
        return false;
    }
    if (nibo != 0) {
        uint64_t table = reg64(REG_A6XX_SP_CS_IBO);
        if (!cs_.ibosLoaded || cs_.ibosFrom != table || cs_.iboUnits != nibo) {
            fault("the IBO descriptors were not loaded from where SP_CS_IBO points, or not as many as SP_CS_IBO_COUNT says");
            return false;
        }
        const uint8_t* bytes = at(table, uint64_t(nibo) * 64);
        if (table % kSpCsTexConstAlign != 0 || bytes == nullptr) {
            fault(text("the IBO descriptors at %#llx: outside every buffer or not 64-byte aligned", table));
            return false;
        }
        for (uint32_t i = 0; i < nibo; i++) {
            uint32_t d[kTexConstDwords];
            memcpy(d, bytes + 64 * i, sizeof d);
            if (allZero(d, kTexConstDwords)) {
                // an unaddressed slot gets a hole descriptor over real memory (compute.hpp); a zero one is what no Mesa
                // driver puts in a bound IBO table, and what a slot nobody wrote would look like
                fault(text("IBO %llu is all zeros: an unaddressed slot gets a hole descriptor, not zeros", i));
                return false;
            }
            uint32_t type = fieldOf(d[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT);
            if (type == A6XX_TEX_2D) {
                if (!check2d(d, "IBO", i, false)) {
                    return false;
                }
                continue;
            }
            if (type != A6XX_TEX_BUFFER) {
                fault(text("IBO %llu is of type %llu: not a buffer or a 2D image descriptor", i, type));
                return false;
            }
            uint64_t elements = fieldOf(d[1], A6XX_TEX_CONST_1_WIDTH__MASK, A6XX_TEX_CONST_1_WIDTH__SHIFT) |
                                uint64_t(fieldOf(d[1], A6XX_TEX_CONST_1_HEIGHT__MASK, A6XX_TEX_CONST_1_HEIGHT__SHIFT)) << 15;
            uint64_t base = (d[4] & A6XX_TEX_CONST_4_BASE_LO__MASK) |
                            uint64_t(fieldOf(d[5], A6XX_TEX_CONST_5_BASE_HI__MASK, A6XX_TEX_CONST_5_BASE_HI__SHIFT)) << 32;
            uint64_t start = 4ull * fieldOf(d[2], A6XX_TEX_CONST_2_STARTOFFSETTEXELS__MASK, A6XX_TEX_CONST_2_STARTOFFSETTEXELS__SHIFT);
            if (elements == 0 || at(base + start, 4 * elements) == nullptr) {
                fault(text("IBO %llu: %llu elements at %#llx: outside every buffer", i, elements, base + start));
                return false;
            }
        }
    }
    // --- the texture state, when the dispatch has any: Gallium's CS_TEX group (fd6_texture.cc:582-687) ---
    uint32_t texCount = reg(REG_A6XX_SP_CS_TEX_COUNT);
    uint64_t texTable = 0, sampTable = 0;
    if (texCount != 0) {
        texTable = reg64(REG_A6XX_SP_CS_TEX_CONST);
        sampTable = reg64(REG_A6XX_SP_CS_TEX_SAMP);
        if (!cs_.texLoaded || cs_.texFrom != texTable || cs_.texUnits != texCount) {
            fault("the texture descriptors were not loaded from where SP_CS_TEX_CONST points, or not as many as SP_CS_TEX_COUNT says");
            return false;
        }
        const uint8_t* texBytes = at(texTable, uint64_t(texCount) * 64);
        if (texTable % kSpCsTexConstAlign != 0 || texBytes == nullptr) {
            fault(text("the texture descriptors at %#llx: outside every buffer or not 64-byte aligned", texTable));
            return false;
        }
        // one sampler at least: an isaml names s#0 whether or not the kernel has a sampler, and Mesa never leaves a bound
        // texture without one
        if (!cs_.sampLoaded || cs_.sampFrom != sampTable || cs_.sampUnits == 0) {
            fault("the sampler descriptors were not loaded from where SP_CS_TEX_SAMP points (a texture load needs at least one)");
            return false;
        }
        const uint8_t* sampBytes = at(sampTable, uint64_t(cs_.sampUnits) * 16);
        if (sampTable % kSpCsTexSampAlign != 0 || sampBytes == nullptr) {
            fault(text("the sampler descriptors at %#llx: outside every buffer or not 16-byte aligned", sampTable));
            return false;
        }
        uint32_t ntex = fieldOf(config, A6XX_SP_CS_CONFIG_NTEX__MASK, A6XX_SP_CS_CONFIG_NTEX__SHIFT);
        uint32_t nsamp = fieldOf(config, A6XX_SP_CS_CONFIG_NSAMP__MASK, A6XX_SP_CS_CONFIG_NSAMP__SHIFT);
        if (ntex < texCount || ntex != nsamp) {
            // ir3's num_samp counts the images too, so NTEX may exceed the table (Gallium's stream for every shader with
            // an image); less than the table, or NTEX and NSAMP apart, is a stream nobody emits
            fault(text("SP_CS_CONFIG names %llu textures and %llu samplers; SP_CS_TEX_COUNT says %llu (NTEX must cover it and equal NSAMP)", ntex, nsamp, texCount));
            return false;
        }
        for (uint32_t i = 0; i < texCount; i++) {
            uint32_t d[kTexConstDwords];
            memcpy(d, texBytes + 64 * i, sizeof d);
            if (allZero(d, kTexConstDwords)) continue;   // Gallium's dummy_view for a slot the kernel does not read
            if (fieldOf(d[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT) != A6XX_TEX_2D) {
                fault(text("texture %llu is of type %llu: not the 2D descriptor this GPU model runs", i, fieldOf(d[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT)));
                return false;
            }
            if (!check2d(d, "texture", i, true)) {
                return false;
            }
        }
        for (uint32_t i = 0; i < cs_.sampUnits; i++) {
            uint32_t d[kTexSampDwords];
            memcpy(d, sampBytes + 16 * i, sizeof d);
            if (!checkSampler(d, i)) {
                return false;
            }
        }
    }
    stats_.dispatches++;
    Dispatch& d = stats_.lastDispatch;
    d = Dispatch{};
    for (int i = 0; i < 3; i++) {
        d.groups[i] = groups[i];
        d.local[i] = local[i];
    }
    d.program = program;
    d.instrlen = instrlen;
    d.constlen = constlen;
    d.ibos = nibo;
    d.consts = cs_.consts;
    d.textureCount = texCount;
    d.samplerCount = texCount != 0 ? cs_.sampUnits : 0;
    d.iboTable = nibo != 0 ? reg64(REG_A6XX_SP_CS_IBO) : 0;
    d.texConstTable = texTable;
    d.texSampTable = sampTable;
    if (nibo != 0) {
        const uint8_t* bytes = at(d.iboTable, uint64_t(nibo) * 64);
        d.ibo.resize(nibo);
        memcpy(d.ibo.data(), bytes, uint64_t(nibo) * 64);
    }
    if (texCount != 0) {
        d.tex.resize(texCount);
        memcpy(d.tex.data(), at(texTable, uint64_t(texCount) * 64), uint64_t(texCount) * 64);
        d.samp.resize(cs_.sampUnits);
        memcpy(d.samp.data(), at(sampTable, uint64_t(cs_.sampUnits) * 16), uint64_t(cs_.sampUnits) * 16);
    }
    cs_ = CsState{};   // the next dispatch loads its own
    return true;
}

// One graphics stage's program at the draw, by the rules CP_EXEC_CS applies to the compute stage's: enabled in both
// HLSQ_xS_CNTL and SP_xS_CONFIG, an instrlen, the program 128-byte aligned (turnip's assert, tu_shader.cc:1313-1316: the
// ST6_SHADER preload unit) and instrlen * 128 bytes inside a buffer, preloaded from exactly there by the stage's own
// opcode and block with at most instrlen units, every constant load within the decoded CONSTLEN (the field holds
// constlen >> 2, a6xx.xml.h:10201-10205).
bool FakeGpu::checkStageProgram(const char* what, const StageLoads& xs, uint32_t instrlenReg, uint32_t objStartReg, uint32_t hlsqCntl, uint32_t spConfig,
                                uint32_t* instrlen, uint64_t* program, uint32_t* constlen) {
    using namespace a6xx;
    if ((reg(hlsqCntl) & A6XX_HLSQ_VS_CNTL_ENABLED) == 0 || (reg(spConfig) & A6XX_SP_VS_CONFIG_ENABLED) == 0) {
        fault(std::string("a draw with the ") + what + " stage not enabled in both HLSQ_xS_CNTL and SP_xS_CONFIG");
        return false;
    }
    *instrlen = reg(instrlenReg);
    *program = reg64(objStartReg);
    if (*instrlen == 0) {
        fault(std::string("a draw with the ") + what + " stage's SP_xS_INSTRLEN 0");
        return false;
    }
    if (*program % kProgramAlign != 0 || at(*program, uint64_t(*instrlen) * 128) == nullptr) {
        fault(std::string("the ") + what + text(" program at SP_xS_OBJ_START %#llx, %llu units: outside every buffer or not 128-byte aligned", *program, *instrlen));
        return false;
    }
    if (!xs.programLoaded || xs.programFrom != *program) {
        fault(std::string("a draw without the ") + what + " program preloaded from where SP_xS_OBJ_START points (ST6_SHADER into its own block, by its own opcode)");
        return false;
    }
    if (xs.programUnits > *instrlen) {
        fault(text("a preload of %llu units for a ", xs.programUnits) + what + text(" program of %llu", *instrlen));
        return false;
    }
    *constlen = fieldOf(reg(hlsqCntl), A6XX_HLSQ_VS_CNTL_CONSTLEN__MASK, A6XX_HLSQ_VS_CNTL_CONSTLEN__SHIFT) << A6XX_HLSQ_VS_CNTL_CONSTLEN__SCALE;
    for (const ConstLoad& c : xs.consts) {
        if (c.dstOffVec4 + c.units > *constlen) {
            fault(std::string(what) + text(" constants at vec4 %llu, %llu of them: past the stage's constlen of %llu", c.dstOffVec4, c.units, *constlen));
            return false;
        }
    }
    return true;
}

// CP_DRAW_INDX_OFFSET: the draw itself. As with CP_EXEC_CS, everything the registers and the loads say about it must hold
// together (fake_gpu.hpp's file comment lists what); what the GPU does with state that does not is hang or write
// nothing, and a fault here stands for either. Nothing is rasterized.
bool FakeGpu::execDraw(const uint32_t* p, uint32_t count) {
    using namespace a6xx;
    if (count != 3) {
        fault(text("CP_DRAW_INDX_OFFSET of %llu words: indexed draws (7 words, DI_SRC_SEL_DMA) are not what this GPU model runs yet", count));
        return false;
    }
    Draw d;
    d.primType = fieldOf(p[0], CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__MASK, CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__SHIFT);
    d.sourceSelect = fieldOf(p[0], CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT__MASK, CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT__SHIFT);
    d.visCull = fieldOf(p[0], CP_DRAW_INDX_OFFSET_0_VIS_CULL__MASK, CP_DRAW_INDX_OFFSET_0_VIS_CULL__SHIFT);
    d.numInstances = p[1];
    d.numIndices = p[2];
    if (d.primType != DI_PT_TRILIST && d.primType != DI_PT_TRISTRIP) {
        fault(text("a draw of primitive type %llu: triangle lists (4) and strips (6) are what this GPU model runs", d.primType));
        return false;
    }
    if (d.sourceSelect != DI_SRC_SEL_AUTO_INDEX) {
        fault(text("a draw whose indices come from source %llu, not DI_SRC_SEL_AUTO_INDEX (2)", d.sourceSelect));
        return false;
    }
    if (d.numInstances != 1) {
        fault(text("a draw of %llu instances: one is what this GPU model runs", d.numInstances));
        return false;
    }
    if (d.numIndices == 0) {
        fault("a draw of 0 vertices");
        return false;
    }
    // the pass: begun by the marker, in sysmem with no bins (GRAS_BIN_CONTROL and RB_BIN_CONTROL alike)
    if (!passBegun_) {
        fault("a draw before CP_SET_MARKER RM6_DIRECT_RENDER: the sysmem pass was not begun in this submission");
        return false;
    }
    for (uint32_t r : {REG_A6XX_GRAS_BIN_CONTROL, REG_A6XX_RB_BIN_CONTROL}) {
        uint32_t v = reg(r);
        if (fieldOf(v, A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION__MASK, A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION__SHIFT) != BUFFERS_IN_SYSMEM ||
            fieldOf(v, A6XX_GRAS_BIN_CONTROL_RENDER_MODE__MASK, A6XX_GRAS_BIN_CONTROL_RENDER_MODE__SHIFT) != RENDERING_PASS ||
            fieldOf(v, A6XX_GRAS_BIN_CONTROL_BINW__MASK, A6XX_GRAS_BIN_CONTROL_BINW__SHIFT) != 0 ||
            fieldOf(v, A6XX_GRAS_BIN_CONTROL_BINH__MASK, A6XX_GRAS_BIN_CONTROL_BINH__SHIFT) != 0) {
            fault(std::string(r == REG_A6XX_GRAS_BIN_CONTROL ? "GRAS_BIN_CONTROL" : "RB_BIN_CONTROL") +
                  text(" %#llx: a draw outside a sysmem pass (BUFFERS_IN_SYSMEM, no bins, RENDERING_PASS): GMEM rendering is not what this GPU model runs", v));
            return false;
        }
    }
    // the stages: VS and FS on, HS/DS/GS off; each program preloaded from where its OBJ_START points, its constants within its constlen
    for (uint32_t r : {REG_A6XX_HLSQ_HS_CNTL, REG_A6XX_HLSQ_DS_CNTL, REG_A6XX_HLSQ_GS_CNTL, REG_A6XX_SP_HS_CONFIG, REG_A6XX_SP_DS_CONFIG, REG_A6XX_SP_GS_CONFIG}) {
        if (reg(r) != 0) {
            fault("a geometry or tessellation stage enabled (HLSQ_HS/DS/GS_CNTL or SP_HS/DS/GS_CONFIG not 0): not served");
            return false;
        }
    }
    if (!checkStageProgram("vertex", vs_, REG_A6XX_SP_VS_INSTRLEN, REG_A6XX_SP_VS_OBJ_START, REG_A6XX_HLSQ_VS_CNTL, REG_A6XX_SP_VS_CONFIG, &d.vsInstrlen,
                           &d.vsProgram, &d.vsConstlen) ||
        !checkStageProgram("fragment", fs_, REG_A6XX_SP_FS_INSTRLEN, REG_A6XX_SP_FS_OBJ_START, REG_A6XX_HLSQ_FS_CNTL, REG_A6XX_SP_FS_CONFIG, &d.fsInstrlen,
                           &d.fsProgram, &d.fsConstlen)) {
        return false;
    }
    if (fieldOf(reg(REG_A6XX_SP_FS_CTRL_REG0), A6XX_SP_FS_CTRL_REG0_THREADSIZE__MASK, A6XX_SP_FS_CTRL_REG0_THREADSIZE__SHIFT) !=
        fieldOf(reg(REG_A6XX_HLSQ_FS_CNTL_0), A6XX_HLSQ_FS_CNTL_0_THREADSIZE__MASK, A6XX_HLSQ_FS_CNTL_0_THREADSIZE__SHIFT)) {
        fault("SP_FS_CTRL_REG0 and HLSQ_FS_CNTL_0 disagree about the fragment thread size");
        return false;
    }
    // the target: RB_MRT[0] a linear image of a known format, its pitch holding the window's rows, its rows inside a buffer
    uint32_t bufInfo = reg(regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, 0));
    d.mrtFormat = fieldOf(bufInfo, A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__MASK, A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__SHIFT);
    d.mrtTileMode = fieldOf(bufInfo, A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__MASK, A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__SHIFT);
    d.mrtSwap = fieldOf(bufInfo, A6XX_RB_MRT_BUF_INFO_COLOR_SWAP__MASK, A6XX_RB_MRT_BUF_INFO_COLOR_SWAP__SHIFT);
    uint32_t bpp = bytesPerPixel(d.mrtFormat);
    if (bpp == 0) {
        fault(text("RB_MRT_BUF_INFO[0]: COLOR_FORMAT %llu is not one this GPU model knows", d.mrtFormat));
        return false;
    }
    if (d.mrtTileMode != TILE6_LINEAR) {
        fault(text("RB_MRT_BUF_INFO[0]: tile mode %llu: only linear targets are what this GPU model runs", d.mrtTileMode));
        return false;
    }
    d.mrtPitch = fieldOf(reg(regRbMrt(REG_A6XX_RB_MRT_PITCH, 0)), A6XX_RB_MRT_PITCH__MASK, A6XX_RB_MRT_PITCH__SHIFT) << A6XX_RB_MRT_PITCH__SCALE;
    d.mrtArrayPitch = fieldOf(reg(regRbMrt(REG_A6XX_RB_MRT_ARRAY_PITCH, 0)), A6XX_RB_MRT_ARRAY_PITCH__MASK, A6XX_RB_MRT_ARRAY_PITCH__SHIFT)
                      << A6XX_RB_MRT_ARRAY_PITCH__SCALE;
    d.mrtBase = reg64(regRbMrt(REG_A6XX_RB_MRT_BASE, 0));
    uint32_t winBr = reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR);
    d.windowScissor[0] = fieldOf(reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL), A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__MASK, A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__SHIFT);
    d.windowScissor[1] = fieldOf(reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL), A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK, A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__SHIFT);
    d.windowScissor[2] = fieldOf(winBr, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X__MASK, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X__SHIFT);
    d.windowScissor[3] = fieldOf(winBr, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y__MASK, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y__SHIFT);
    const uint64_t width = uint64_t(d.windowScissor[2]) + 1, rows = uint64_t(d.windowScissor[3]) + 1;
    if (d.mrtPitch < width * bpp) {
        fault(text("RB_MRT_PITCH[0] %llu bytes for rows of %llu pixels of %llu bytes: shorter than a row", d.mrtPitch, width, bpp));
        return false;
    }
    {
        // the rows counted to a multiple of 4, as the layout counts its last level (fd6_layout.c:249-250) and check2d does
        uint64_t counted = (rows + 3) & ~uint64_t(3);
        if (d.mrtBase % kMrtBaseAlign != 0 || at(d.mrtBase, counted * d.mrtPitch) == nullptr) {
            fault(text("RB_MRT[0]: %llu rows (%llu counted) of %llu bytes", rows, counted, d.mrtPitch) +
                  text(" at %#llx: outside every buffer or not 64-byte aligned", d.mrtBase));
            return false;
        }
    }
    d.spFsMrtReg = reg(regSpFsMrtReg(0));
    if (fieldOf(d.spFsMrtReg, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__MASK, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__SHIFT) != d.mrtFormat) {
        fault(text("SP_FS_MRT_REG[0] names format %llu, RB_MRT_BUF_INFO[0] %llu",
                   fieldOf(d.spFsMrtReg, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__MASK, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__SHIFT), d.mrtFormat));
        return false;
    }
    if (fieldOf(reg(REG_A6XX_GRAS_LRZ_MRT_BUF_INFO_0), A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT__MASK, A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT__SHIFT) != d.mrtFormat) {
        fault(text("GRAS_LRZ_MRT_BUF_INFO_0 names format %llu, RB_MRT_BUF_INFO[0] %llu", reg(REG_A6XX_GRAS_LRZ_MRT_BUF_INFO_0), d.mrtFormat));
        return false;
    }
    if (reg(regRbMrt(REG_A6XX_RB_MRT_BASE_GMEM, 0)) != 0) {
        fault(text("RB_MRT_BASE_GMEM[0] is %#llx: a sysmem draw has no GMEM base", reg(regRbMrt(REG_A6XX_RB_MRT_BASE_GMEM, 0))));
        return false;
    }
    // the FS's output path to it: one MRT in both copies, a colour register, all four components in both copies and the write mask
    d.mrtCount = fieldOf(reg(REG_A6XX_RB_FS_OUTPUT_CNTL1), A6XX_RB_FS_OUTPUT_CNTL1_MRT__MASK, A6XX_RB_FS_OUTPUT_CNTL1_MRT__SHIFT);
    uint32_t spMrt = fieldOf(reg(REG_A6XX_SP_FS_OUTPUT_CNTL1), A6XX_SP_FS_OUTPUT_CNTL1_MRT__MASK, A6XX_SP_FS_OUTPUT_CNTL1_MRT__SHIFT);
    if (d.mrtCount != 1 || spMrt != 1) {
        fault(text("SP_FS_OUTPUT_CNTL1.MRT %llu and RB_FS_OUTPUT_CNTL1.MRT %llu: one target, in both", spMrt, d.mrtCount));
        return false;
    }
    d.spFsOutputReg0 = reg(regSpFsOutputReg(0));
    if (fieldOf(d.spFsOutputReg0, A6XX_SP_FS_OUTPUT_REG_REGID__MASK, A6XX_SP_FS_OUTPUT_REG_REGID__SHIFT) == kRegidNone) {
        fault("SP_FS_OUTPUT_REG[0] names no register: the fragment stage writes no colour");
        return false;
    }
    d.renderComponents = reg(REG_A6XX_RB_RENDER_COMPONENTS);
    d.fsRenderComponents = reg(REG_A6XX_SP_FS_RENDER_COMPONENTS);
    if (d.renderComponents != d.fsRenderComponents || (d.renderComponents & A6XX_RB_RENDER_COMPONENTS_RT0__MASK) != 0xf) {
        fault(text("RENDER_COMPONENTS %#llx (SP) and %#llx (RB): RT0 needs all four components in both", d.fsRenderComponents, d.renderComponents));
        return false;
    }
    if (fieldOf(reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE__MASK, A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE__SHIFT) == 0) {
        fault("RB_MRT_CONTROL[0] enables no component: the draw would write nothing");
        return false;
    }
    // no depth, no stencil
    if (fieldOf(reg(REG_A6XX_RB_DEPTH_BUFFER_INFO), A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__MASK, A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__SHIFT) != DEPTH6_NONE ||
        (reg(REG_A6XX_RB_DEPTH_CNTL) & A6XX_RB_DEPTH_CNTL_Z_TEST_ENABLE) != 0 || reg(REG_A6XX_RB_STENCIL_CONTROL) != 0) {
        fault("depth or stencil state with no depth buffer: not what this GPU model runs");
        return false;
    }
    // the scissors inside the window
    auto scissor = [&](uint32_t tlReg, uint32_t* out) {
        out[0] = fieldOf(reg(tlReg), A6XX_GRAS_SC_SCREEN_SCISSOR_TL_X__MASK, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_X__SHIFT);
        out[1] = fieldOf(reg(tlReg), A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y__MASK, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y__SHIFT);
        out[2] = fieldOf(reg(tlReg + 1), A6XX_GRAS_SC_SCREEN_SCISSOR_BR_X__MASK, A6XX_GRAS_SC_SCREEN_SCISSOR_BR_X__SHIFT);
        out[3] = fieldOf(reg(tlReg + 1), A6XX_GRAS_SC_SCREEN_SCISSOR_BR_Y__MASK, A6XX_GRAS_SC_SCREEN_SCISSOR_BR_Y__SHIFT);
    };
    scissor(regGrasScScreenScissor(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL, 0), d.screenScissor);
    scissor(regGrasScViewportScissor(REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL, 0), d.viewportScissor);
    for (int which = 0; which < 2; which++) {
        const uint32_t* sc = which == 0 ? d.screenScissor : d.viewportScissor;
        if (sc[0] > sc[2] || sc[1] > sc[3] || sc[2] > d.windowScissor[2] || sc[3] > d.windowScissor[3]) {
            fault(std::string(which == 0 ? "GRAS_SC_SCREEN_SCISSOR[0]" : "GRAS_SC_VIEWPORT_SCISSOR[0]") +
                  text(" (%llu, %llu)-", sc[0], sc[1]) + text("(%llu, %llu) is not inside the window scissor", sc[2], sc[3]) +
                  text(" (%llu, %llu)", d.windowScissor[2], d.windowScissor[3]));
            return false;
        }
    }
    // the viewport: finite, non-zero scales; a guardband written
    for (uint32_t i = 0; i < kViewportWords; i++) {
        uint32_t bits = reg(regGrasClVport(REG_A6XX_GRAS_CL_VPORT_XOFFSET, 0) + i);
        memcpy(&d.viewport[i], &bits, 4);
    }
    for (uint32_t i = 0; i < 4; i++) {
        if (!std::isfinite(d.viewport[i]) || ((i == 1 || i == 3) && d.viewport[i] == 0.0f)) {
            fault("GRAS_CL_VPORT[0]: a scale of 0 or a non-finite offset or scale");
            return false;
        }
    }
    if (regs_.find(REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ) == regs_.end()) {
        fault("GRAS_CL_GUARDBAND_CLIP_ADJ was never written");
        return false;
    }
    // the vertex fetch: equal counts, and every fetch below them inside a buffer with a decode and a destination register
    uint32_t vfd0 = reg(REG_A6XX_VFD_CONTROL_0);
    d.vfdFetchCount = fieldOf(vfd0, A6XX_VFD_CONTROL_0_FETCH_CNT__MASK, A6XX_VFD_CONTROL_0_FETCH_CNT__SHIFT);
    d.vfdDecodeCount = fieldOf(vfd0, A6XX_VFD_CONTROL_0_DECODE_CNT__MASK, A6XX_VFD_CONTROL_0_DECODE_CNT__SHIFT);
    if (d.vfdFetchCount != d.vfdDecodeCount) {
        fault(text("VFD_CONTROL_0 counts %llu fetches and %llu decodes", d.vfdFetchCount, d.vfdDecodeCount));
        return false;
    }
    for (uint32_t i = 0; i < d.vfdFetchCount; i++) {
        uint64_t base = reg64(regVfdFetchBase(i));
        uint32_t size = reg(vfdFetchSize(i));
        if (size == 0 || at(base, size) == nullptr || regs_.find(regVfdDecodeInstr(i)) == regs_.end() ||
            fieldOf(reg(regVfdDestCntlInstr(i)), A6XX_VFD_DEST_CNTL_INSTR_REGID__MASK, A6XX_VFD_DEST_CNTL_INSTR_REGID__SHIFT) == kRegidNone) {
            fault(text("vertex fetch %llu: %llu bytes at %#llx outside every buffer, or no VFD_DECODE_INSTR, or no destination register", i, size, base));
            return false;
        }
    }
    d.vertexIdRegid = fieldOf(reg(REG_A6XX_VFD_CONTROL_1), A6XX_VFD_CONTROL_1_REGID4VTX__MASK, A6XX_VFD_CONTROL_1_REGID4VTX__SHIFT);
    d.vertexIndexOffset = reg(REG_A6XX_VFD_INDEX_OFFSET);
    // the link: a position location, the stride agreed, the output count the registers name
    uint32_t pack = reg(REG_A6XX_VPC_VS_PACK);
    d.positionLoc = fieldOf(pack, A6XX_VPC_VS_PACK_POSITIONLOC__MASK, A6XX_VPC_VS_PACK_POSITIONLOC__SHIFT);
    d.strideInVpc = fieldOf(pack, A6XX_VPC_VS_PACK_STRIDE_IN_VPC__MASK, A6XX_VPC_VS_PACK_STRIDE_IN_VPC__SHIFT);
    if (d.positionLoc == kLocNone) {
        fault("VPC_VS_PACK names no position location");
        return false;
    }
    if (d.strideInVpc != fieldOf(reg(REG_A6XX_PC_VS_OUT_CNTL), A6XX_PC_VS_OUT_CNTL_STRIDE_IN_VPC__MASK, A6XX_PC_VS_OUT_CNTL_STRIDE_IN_VPC__SHIFT)) {
        fault("VPC_VS_PACK and PC_VS_OUT_CNTL disagree about the VPC stride");
        return false;
    }
    d.vsOutCount = fieldOf(reg(REG_A6XX_SP_VS_PRIMITIVE_CNTL), A6XX_SP_VS_PRIMITIVE_CNTL_OUT__MASK, A6XX_SP_VS_PRIMITIVE_CNTL_OUT__SHIFT);
    {
        uint32_t named = 0;
        for (uint32_t i = 0; i < (d.vsOutCount + 1) / 2 && i < 16; i++) {
            uint32_t v = reg(regSpVsOutReg(i));
            named += fieldOf(v, A6XX_SP_VS_OUT_REG_A_COMPMASK__MASK, A6XX_SP_VS_OUT_REG_A_COMPMASK__SHIFT) != 0;
            named += fieldOf(v, A6XX_SP_VS_OUT_REG_B_COMPMASK__MASK, A6XX_SP_VS_OUT_REG_B_COMPMASK__SHIFT) != 0;
        }
        if (d.vsOutCount == 0 || d.vsOutCount > 32 || named != d.vsOutCount) {
            fault(text("SP_VS_PRIMITIVE_CNTL.OUT %llu, but %llu outputs are named in SP_VS_OUT_REG", d.vsOutCount, named));
            return false;
        }
    }
    uint32_t vpc0 = reg(REG_A6XX_VPC_CNTL_0);
    d.numNonPosVar = fieldOf(vpc0, A6XX_VPC_CNTL_0_NUMNONPOSVAR__MASK, A6XX_VPC_CNTL_0_NUMNONPOSVAR__SHIFT);
    {
        bool a = d.numNonPosVar != 0, b = (vpc0 & A6XX_VPC_CNTL_0_VARYING) != 0, c = (reg(REG_A6XX_SP_FS_CTRL_REG0) & A6XX_SP_FS_CTRL_REG0_VARYING) != 0,
             e = (reg(REG_A6XX_HLSQ_FS_CNTL_0) & A6XX_HLSQ_FS_CNTL_0_VARYINGS) != 0, f = (reg(REG_A6XX_RB_RENDER_CONTROL0) & A6XX_RB_RENDER_CONTROL0_UNK10) != 0;
        if (a != b || b != c || c != e || e != f) {
            fault("the VARYING flags disagree across VPC_CNTL_0 (NUMNONPOSVAR, VARYING), SP_FS_CTRL_REG0, HLSQ_FS_CNTL_0 and RB_RENDER_CONTROL0");
            return false;
        }
    }
    // the barycentric and coordinate registers: HLSQ_CONTROL_3/4's regids against GRAS_CNTL's and RB_RENDER_CONTROL0's bits
    {
        uint32_t c3 = reg(REG_A6XX_HLSQ_CONTROL_3_REG), c4 = reg(REG_A6XX_HLSQ_CONTROL_4_REG), gras = reg(REG_A6XX_GRAS_CNTL), rb = reg(REG_A6XX_RB_RENDER_CONTROL0);
        bool ijPersp = fieldOf(c3, A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL__MASK, A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL__SHIFT) != kRegidNone;
        bool ijLinear = fieldOf(c3, A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL__MASK, A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL__SHIFT) != kRegidNone;
        bool coord = fieldOf(c4, A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID__MASK, A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID__SHIFT) != kRegidNone;
        bool grasPersp = (gras & A6XX_GRAS_CNTL_IJ_PERSP_PIXEL) != 0, rbPersp = (rb & A6XX_RB_RENDER_CONTROL0_IJ_PERSP_PIXEL) != 0;
        bool grasLinear = (gras & A6XX_GRAS_CNTL_IJ_LINEAR_PIXEL) != 0, rbLinear = (rb & A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL) != 0;
        uint32_t grasCoord = fieldOf(gras, A6XX_GRAS_CNTL_COORD_MASK__MASK, A6XX_GRAS_CNTL_COORD_MASK__SHIFT);
        uint32_t rbCoord = fieldOf(rb, A6XX_RB_RENDER_CONTROL0_COORD_MASK__MASK, A6XX_RB_RENDER_CONTROL0_COORD_MASK__SHIFT);
        // IJ_LINEAR_PIXEL is also set for a fragment coordinate or face read (turnip's need_size), so a regid implies the
        // bits but not the other way round
        if (ijPersp != grasPersp || ijPersp != rbPersp || grasLinear != rbLinear || (ijLinear && !grasLinear) || grasCoord != rbCoord || (coord != (grasCoord != 0))) {
            fault("the barycentric and coordinate registers disagree across HLSQ_CONTROL_3/4_REG, GRAS_CNTL and RB_RENDER_CONTROL0");
            return false;
        }
    }
    // one sample everywhere
    for (uint32_t r : {REG_A6XX_SP_TP_DEST_MSAA_CNTL, REG_A6XX_GRAS_DEST_MSAA_CNTL, REG_A6XX_RB_DEST_MSAA_CNTL}) {
        uint32_t v = reg(r);
        if ((v & A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE) == 0 || fieldOf(v, A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__MASK, A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__SHIFT) != 0) {
            fault(text("multisampling in a DEST_MSAA_CNTL register (%#llx): not what this GPU model runs", v));
            return false;
        }
    }
    // RB_CCU_CNTL: written in this submission, right after a WFI, with the sysmem shape (colour cache after the depth cache at 0)
    {
        uint32_t ccu = reg(REG_A6XX_RB_CCU_CNTL);
        bool sysmem = fieldOf(ccu, A6XX_RB_CCU_CNTL_COLOR_OFFSET__MASK, A6XX_RB_CCU_CNTL_COLOR_OFFSET__SHIFT) != 0 &&
                      fieldOf(ccu, A6XX_RB_CCU_CNTL_DEPTH_OFFSET__MASK, A6XX_RB_CCU_CNTL_DEPTH_OFFSET__SHIFT) == 0;
        if (!ccuCntlWritten_ || !ccuCntlAfterWfi_ || !sysmem) {
            fault(text("RB_CCU_CNTL (%#llx) was not written in this submission right after a CP_WAIT_FOR_IDLE with the sysmem shape", ccu));
            return false;
        }
    }
    stats_.draws++;
    drawsInSubmission_++;
    drawsWithoutCcuFlush_++;
    drawsWithoutCacheFlush_++;
    d.vsConsts = vs_.consts;
    d.fsConsts = fs_.consts;
    stats_.lastDraw = d;
    return true;
}

// the packets from ADDRESS on, WORDS of them; false on a fault (the submission stops there)
bool FakeGpu::execute(uint64_t address, uint32_t words, int depth) {
    using namespace a6xx;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(at(address, 4ull * words));
    if (w == nullptr) {
        fault("the command stream is not inside a buffer");
        return false;
    }
    for (uint32_t i = 0; i < words;) {
        pm4::Header h = readHeader(w[i]);
        if (h.kind == pm4::Header::kBad) {
            fault(text("word %llu (0x%08llx): not a packet, or its parity is wrong", i, w[i]));
            return false;
        }
        if (h.count > words - i - 1) {
            fault("a packet runs past the end of the stream");
            return false;
        }
        const uint32_t* p = w + i + 1;
        stats_.packets++;
        if (h.kind == pm4::Header::kRegisters) {
            for (uint32_t j = 0; j < h.count; j++) {
                regs_[h.reg + j] = p[j];   // kept as written; a dispatch or a draw is checked against them
                if (h.reg + j == REG_A6XX_RB_CCU_CNTL) {
                    // "isn't pipelined" (tu_cmd_buffer.cc:373-377): both witnesses write it right after a CP_WAIT_FOR_IDLE
                    ccuCntlWritten_ = true;
                    ccuCntlAfterWfi_ = lastWasWfi_;
                }
            }
        } else {
            switch (h.opcode) {
            case pm4::kNop:
            case pm4::kWaitForIdle:
            case pm4::kWaitMemWrites:
            case pm4::kWaitForMe:
                break;
            case pm4::kMemWrite: {
                if (h.count < 2) {
                    fault("CP_MEM_WRITE without its address");
                    return false;
                }
                uint64_t to = p[0] | uint64_t(p[1]) << 32;
                uint8_t* d = at(to, 4ull * (h.count - 2));
                if (d == nullptr || to % 4 != 0) {
                    fault("CP_MEM_WRITE outside every buffer");
                    return false;
                }
                memcpy(d, p + 2, 4ull * (h.count - 2));
                break;
            }
            case pm4::kMemcpy: {
                if (h.count != 5) {
                    fault("CP_MEMCPY of other than 5 words");
                    return false;
                }
                uint64_t n = p[0], from = p[1] | uint64_t(p[2]) << 32, to = p[3] | uint64_t(p[4]) << 32;
                // The phone's GPU took 125 000 words in one packet (4.2 ms) and hung on 4 Mi (27 October); where between
                // the two it stops taking them is not measured. The driver cuts at 64 Ki, and the fake holds it to that.
                if (n > 64 * 1024) {
                    fault("CP_MEMCPY of more words in one packet than the driver's own limit of 64 Ki");
                    return false;
                }
                uint8_t* s = at(from, 4 * n);
                uint8_t* d = at(to, 4 * n);
                if (s == nullptr || d == nullptr || from % 4 != 0 || to % 4 != 0) {
                    fault("CP_MEMCPY outside every buffer");
                    return false;
                }
                memmove(d, s, 4 * n);
                stats_.wordsCopied += n;
                break;
            }
            case pm4::kIndirectBuffer: {
                if (h.count != 3 || depth >= 2) {
                    fault(depth >= 2 ? "indirect buffers nested deeper than the GPU's two levels" : "CP_INDIRECT_BUFFER of other than 3 words");
                    return false;
                }
                if (!execute(p[0] | uint64_t(p[1]) << 32, p[2], depth + 1)) {
                    return false;
                }
                break;
            }
            // --- the compute dispatch's and the draw's packets ---
            case CP_LOAD_STATE6_FRAG:
            case CP_LOAD_STATE6_GEOM:
            case CP_LOAD_STATE6:
                if (!loadState(h.opcode, p, h.count)) {
                    return false;
                }
                break;
            case CP_SET_MARKER:
                if (h.count != 1) {
                    fault("CP_SET_MARKER of other than 1 word");
                    return false;
                }
                // RM6_DIRECT_RENDER begins a sysmem pass (tu_cmd_buffer.cc:2130-2131; fd6_gmem.cc:2041-2042): a draw before it has no pass
                if (fieldOf(p[0], A6XX_CP_SET_MARKER_0_MODE__MASK, A6XX_CP_SET_MARKER_0_MODE__SHIFT) == RM6_DIRECT_RENDER) {
                    passBegun_ = true;
                }
                break;
            case CP_SET_MODE:
            case CP_SKIP_IB2_ENABLE_GLOBAL:
            case CP_SKIP_IB2_ENABLE_LOCAL:
            case CP_SET_VISIBILITY_OVERRIDE:
                // one word each (tu_cmd_buffer.cc:2134-2142); what they do the XML does not say, so nothing is judged of the value
                if (h.count != 1) {
                    fault(text("opcode 0x%02llx of %llu words, not 1", h.opcode, h.count));
                    return false;
                }
                break;
            case CP_SET_DRAW_STATE: {
                // only the 3-word disable-all form both batch starts send (fd6_emit_restore; tu_disable_draw_states): this
                // driver writes its registers directly, and a state group with an address would apply at the draw over them
                if (h.count != 3 || (p[0] & CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS) == 0 ||
                    fieldOf(p[0], CP_SET_DRAW_STATE__0_COUNT__MASK, CP_SET_DRAW_STATE__0_COUNT__SHIFT) != 0 || p[1] != 0 || p[2] != 0) {
                    fault("draw-state groups are not what this GPU model runs: registers are written directly (only CP_SET_DRAW_STATE's 3-word disable-all form is taken)");
                    return false;
                }
                break;
            }
            case CP_DRAW_INDX_OFFSET:
                if (!execDraw(p, h.count)) {
                    return false;
                }
                break;
            case CP_EVENT_WRITE: {
                // 1 word: the event; 4: and the address and value written when it completes (at once here: the model
                // is in order, so what a later wait reads is what an earlier event wrote)
                if (h.count != 1 && h.count != 4) {
                    fault(text("CP_EVENT_WRITE of %llu words, not 1 or 4", h.count));
                    return false;
                }
                if (h.count == 4) {
                    uint64_t to = p[1] | uint64_t(p[2]) << 32;
                    // With the TIMESTAMP bit the GPU writes a timestamp of its own, not the packet's value (the XML says so,
                    // and on the phone a wait for the value then never ended: 28 October, every dispatch); here a 64-bit
                    // count no wait for a small value can match, 8 bytes as the GPU's timestamp is.
                    bool timestamp = (p[0] & CP_EVENT_WRITE_0_TIMESTAMP) != 0;
                    uint8_t* d = at(to, timestamp ? 8 : 4);
                    if (d == nullptr || to % 4 != 0) {
                        fault(text("CP_EVENT_WRITE's timestamp at %#llx: outside every buffer", to));
                        return false;
                    }
                    if (timestamp) {
                        uint64_t ticks = 0x0001000000000000ull + stats_.packets;
                        memcpy(d, &ticks, 8);
                    } else {
                        memcpy(d, p + 3, 4);
                    }
                    // what a draw needs after it: the colour CCU flushed to memory (tu6_emit_flushes / fd6_emit_flushes: the
                    // 4-word form with an address, freedreno_gpu_event.h:60-85 needs_seqno) and then the cache flush
                    uint32_t ev = fieldOf(p[0], CP_EVENT_WRITE_0_EVENT__MASK, CP_EVENT_WRITE_0_EVENT__SHIFT);
                    if (ev == PC_CCU_FLUSH_COLOR_TS) drawsWithoutCcuFlush_ = 0;
                    if (ev == CACHE_FLUSH_TS) drawsWithoutCacheFlush_ = 0;
                }
                break;
            }
            case CP_WAIT_REG_MEM: {
                if (h.count != 6) {
                    fault(text("CP_WAIT_REG_MEM of %llu words, not 6", h.count));
                    return false;
                }
                if (fieldOf(p[0], CP_WAIT_REG_MEM_0_POLL__MASK, CP_WAIT_REG_MEM_0_POLL__SHIFT) != POLL_MEMORY) {
                    fault("CP_WAIT_REG_MEM on a register: not what this GPU model runs");
                    return false;
                }
                uint64_t a = p[1] | uint64_t(p[2]) << 32;
                const uint8_t* m = at(a, 4);
                if (m == nullptr || a % 4 != 0) {
                    fault(text("CP_WAIT_REG_MEM on %#llx: outside every buffer", a));
                    return false;
                }
                uint32_t value;
                memcpy(&value, m, 4);
                value &= p[4];
                uint32_t ref = p[3];
                bool holds = false;
                switch (fieldOf(p[0], CP_WAIT_REG_MEM_0_FUNCTION__MASK, CP_WAIT_REG_MEM_0_FUNCTION__SHIFT)) {
                case WRITE_ALWAYS: holds = true; break;
                case WRITE_LT: holds = value < ref; break;
                case WRITE_LE: holds = value <= ref; break;
                case WRITE_EQ: holds = value == ref; break;
                case WRITE_NE: holds = value != ref; break;
                case WRITE_GE: holds = value >= ref; break;
                case WRITE_GT: holds = value > ref; break;
                default: break;
                }
                if (!holds) {
                    // nothing after this packet can change the word, so the real command processor would spin forever
                    fault(text("CP_WAIT_REG_MEM would wait forever: the word at %#llx is %#llx, the reference %#llx", a, value, ref));
                    return false;
                }
                break;
            }
            case CP_WAIT_MEM_GTE: {
                if (h.count != 4) {
                    fault(text("CP_WAIT_MEM_GTE of %llu words, not 4", h.count));
                    return false;
                }
                uint64_t a = p[1] | uint64_t(p[2]) << 32;
                const uint8_t* m = at(a, 4);
                if (m == nullptr || a % 4 != 0) {
                    fault(text("CP_WAIT_MEM_GTE on %#llx: outside every buffer", a));
                    return false;
                }
                uint32_t value;
                memcpy(&value, m, 4);
                if (int32_t(value - p[3]) < 0) {
                    fault(text("CP_WAIT_MEM_GTE would wait forever: the word at %#llx is %#llx, below %#llx", a, value, p[3]));
                    return false;
                }
                break;
            }
            case CP_EXEC_CS:
                if (!execCs(p, h.count)) {
                    return false;
                }
                break;
            default: {
                fault(text("opcode 0x%02llx is not one this GPU model runs", h.opcode));
                return false;
            }
            }
        }
        lastWasWfi_ = h.kind == pm4::Header::kCommand && h.opcode == pm4::kWaitForIdle;
        i += 1 + h.count;
    }
    return true;
}

// A submission the GPU never finishes, as the kernel treats it. With recovery, the GPU is reset and everything it
// had been given is given up as done: the fence word is set to the last submission's number (hakios_gpu_recover),
// so every wait ends, the hung submission's own with the timeout it was found by, and FAULTS says it happened.
// Without recovery nothing is waited for again in this run.
void FakeGpu::hung() {
    jobs_.clear();
    if (recovers_) {
        stats_.recoveries++;
        completed_ = submitted_;
    } else {
        wedged_ = true;
    }
}

void FakeGpu::run(const Job& job) {
    stats_.submits++;
    // a submission starts with nothing loaded and no pass begun: the hardware keeps no state across them that this models
    // (the registers it does keep, regs_, stay)
    cs_ = CsState{};
    vs_ = StageLoads{};
    fs_ = StageLoads{};
    passBegun_ = ccuCntlWritten_ = ccuCntlAfterWfi_ = lastWasWfi_ = false;
    drawsInSubmission_ = drawsWithoutCcuFlush_ = drawsWithoutCacheFlush_ = 0;
    bool ok = execute(job.address, job.words, 0);
    if (ok && drawsInSubmission_ != 0) {
        // the colour a draw wrote sits in the CCU until PC_CCU_FLUSH_COLOR_TS (the kernel's own end-of-submit fence
        // flushes the UCHE, not the CCU: hakios.adreno.c:3496-3516), and what the CCU flushed sits in the UCHE until
        // CACHE_FLUSH_TS: a stream that draws and ends without both would complete with the target unchanged
        if (drawsWithoutCcuFlush_ != 0) {
            fault("the colour stays in the CCU: no PC_CCU_FLUSH_COLOR_TS (4-word) after the draw");
            ok = false;
        } else if (drawsWithoutCacheFlush_ != 0) {
            fault("the colour may stay in the cache: no CACHE_FLUSH_TS (4-word) after the draw");
            ok = false;
        }
        stats_.lastDraw.ccuFlushed = drawsWithoutCcuFlush_ == 0;
        stats_.lastDraw.cacheFlushed = drawsWithoutCacheFlush_ == 0;
    }
    if (ok) {
        completed_ = job.fence;
        return;
    }
    hungFences_.insert(job.fence);
    hung();
}

void FakeGpu::worker() {
    std::unique_lock<std::mutex> g(lock_);
    for (;;) {
        changed_.wait(g, [this] { return quit_ || (!paused_ && !jobs_.empty()); });
        if (quit_ && jobs_.empty()) {
            return;
        }
        if (paused_) {
            continue;
        }
        Job job = jobs_.front();
        jobs_.pop_front();
        if (delayUs_ != 0) {
            g.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(delayUs_));
            g.lock();
        }
        run(job);
        changed_.notify_all();
    }
}

int FakeGpu::call(uint32_t request, void* data, size_t size) {
    std::unique_lock<std::mutex> g(lock_);
    switch (request) {
    case kGetParam: {
        if (size != sizeof(MsmParam)) return EINVAL;
        auto* p = static_cast<MsmParam*>(data);
        // the kernel's table (bsd/dev/hakios.adreno.c, hakios_gpu_parameter), number for number
        switch (p->param) {
        case kParamGpuId: p->value = 0; return 0;                   // "none, the chip ID says it"
        case kParamGmemSize: p->value = kGmemBytes; return 0;
        case kParamChipId: p->value = kChipId | kSpeedBin << 32; return 0;
        case kParamMaxFreq: p->value = kMaxFrequencyHz; return 0;
        case kParamTimestamp: p->value = uint64_t(nowNs()); return 0;
        case kParamGmemBase: p->value = 0; return 0;
        case kParamPriorities: p->value = 1; return 0;
        case kParamPpPgtable: p->value = 0; return 0;
        case kParamFaults: p->value = stats_.recoveries; return 0;
        case kParamSuspends: p->value = 0; return 0;
        case kParamHighestBankBit: p->value = 14; return 0;
        case kParamRaytracing: p->value = 0; return 0;
        case kParamUbwcSwizzle: p->value = 6; return 0;
        case kParamMacrotileMode: p->value = 1; return 0;
        default: return EINVAL;                                      // (kIOReturnBadArgument there: VA_START, VA_SIZE among them)
        }
    }
    case kGemNew: {
        if (size != sizeof(MsmGemNew)) return EINVAL;
        auto* n = static_cast<MsmGemNew*>(data);
        if (n->size == 0 || n->size > (256ull << 20)) return EINVAL;
        Buffer b{nextIova_, std::vector<uint8_t>(n->size)};
        nextIova_ += (n->size + 0xffff) & ~0xffffull;
        n->handle = nextHandle_++;
        buffers_.emplace(n->handle, std::move(b));
        return 0;
    }
    case kGemInfo: {
        if (size != sizeof(MsmGemInfo)) return EINVAL;
        auto* i = static_cast<MsmGemInfo*>(data);
        auto b = buffers_.find(i->handle);
        if (b == buffers_.end()) return ENOENT;
        if (i->info == kInfoGetIova) { i->value = b->second.iova; return 0; }
        if (i->info == kInfoGetOffset) { i->value = 0; return 0; }
        return EINVAL;
    }
    case kGemClose: {
        if (size != sizeof(DrmGemClose)) return EINVAL;
        return buffers_.erase(static_cast<DrmGemClose*>(data)->handle) ? 0 : ENOENT;
    }
    case kSubmitQueueNew: {
        if (size != sizeof(MsmSubmitQueue)) return EINVAL;
        auto* q = static_cast<MsmSubmitQueue*>(data);
        q->id = nextQueue_++;
        queues_.insert(q->id);
        return 0;
    }
    case kSubmitQueueClose:
        return 0;
    case kGemSubmit: {
        if (size != sizeof(MsmGemSubmit)) return EINVAL;
        auto* s = static_cast<MsmGemSubmit*>(data);
        if (s->nr_cmds != 1 || s->nr_bos == 0 || (s->queueid != 0 && queues_.count(s->queueid) == 0)) return EINVAL;
        auto* bos = reinterpret_cast<const MsmSubmitBo*>(uintptr_t(s->bos));
        auto* cmd = reinterpret_cast<const MsmSubmitCmd*>(uintptr_t(s->cmds));
        if (cmd->submit_idx >= s->nr_bos || cmd->type != kSubmitCmdBuf || cmd->size % 4 != 0) return EINVAL;
        for (uint32_t i = 0; i < s->nr_bos; i++) {
            if (buffers_.count(bos[i].handle) == 0) return ENOENT;   // a buffer the GPU would not know: refused
        }
        Buffer& c = buffers_.at(bos[cmd->submit_idx].handle);
        if (uint64_t(cmd->submit_offset) + cmd->size > c.bytes.size()) return EINVAL;
        Job job{++submitted_, c.iova + cmd->submit_offset, cmd->size / 4};
        s->fence = job.fence;
        // (a wedged GPU still takes submissions, as the kernel's ring does until it is full: they are never run)
        if (wedged_) {
            return 0;
        }
        if (async_) {
            jobs_.push_back(job);
            changed_.notify_all();
        } else {
            run(job);
        }
        return 0;
    }
    case kWaitFence: {
        if (size != sizeof(MsmWaitFence)) return EINVAL;
        auto* w = static_cast<MsmWaitFence*>(data);
        if (w->queueid != 0 && queues_.count(w->queueid) == 0) return EINVAL;
        if (w->fence > submitted_) {
            // The kernel does not know this fence was never handed out: it waits the caller's whole patience for a word
            // that never comes, and that is a hang like any other. Here it is one at once.
            fault("a fence that was never handed out was waited for");
            hung();
            return ETIMEDOUT;
        }
        if (wedged_ || hungFences_.count(w->fence) != 0) {
            // (the kernel takes the caller's whole patience to say so; the fake knows already)
            return ETIMEDOUT;
        }
        int64_t deadline = w->timeout.seconds * 1000000000 + w->timeout.nanoseconds;
        while (completed_ < w->fence) {
            int64_t left = deadline - nowNs();
            if (left <= 0) return ETIMEDOUT;
            changed_.wait_for(g, std::chrono::nanoseconds(left));
            if (wedged_ || hungFences_.count(w->fence) != 0) return ETIMEDOUT;
        }
        return 0;
    }
    case kGemCpuPrep:
    case kGemCpuFini:
        return 0;
    default:
        return ENOSYS;
    }
}

}  // namespace hakios::metal
