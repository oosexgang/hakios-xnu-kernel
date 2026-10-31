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

// the pixel size of the formats this model knows (texture.hpp's, by their FMT: fd6_format_table.c); 0 for another
uint32_t bytesPerPixel(uint32_t fmt) {
    switch (fmt) {
    case a6xx::FMT6_A8_UNORM: return 1;
    case a6xx::FMT6_8_UINT: return 1;
    case a6xx::FMT6_8_UNORM: return 1;
    case a6xx::FMT6_16_FLOAT: return 2;
    case a6xx::FMT6_16_UNORM: return 2;
    case a6xx::FMT6_16_UINT: return 2;
    case a6xx::FMT6_16_SINT: return 2;
    case a6xx::FMT6_8_8_UNORM: return 2;
    case a6xx::FMT6_5_6_5_UNORM: return 2;
    case a6xx::FMT6_5_5_5_1_UNORM: return 2;
    case a6xx::FMT6_16_16_FLOAT: return 4;
    case a6xx::FMT6_11_11_10_FLOAT: return 4;
    case a6xx::FMT6_10_10_10_2_UNORM: return 4;
    case a6xx::FMT6_10_10_10_2_UNORM_DEST: return 4;
    case a6xx::FMT6_32_32_FLOAT: return 8;
    case a6xx::FMT6_8_8_8_8_UNORM: return 4;
    case a6xx::FMT6_32_FLOAT: return 4;
    case a6xx::FMT6_32_UINT: return 4;
    case a6xx::FMT6_16_16_16_16_FLOAT: return 8;
    case a6xx::FMT6_32_32_32_32_FLOAT: return 16;
    case a6xx::FMT6_32_32_32_32_UINT: return 16;
    // the integer and snorm colour formats, sized from their names
    case a6xx::FMT6_8_SNORM: return 1;
    case a6xx::FMT6_8_SINT: return 1;
    case a6xx::FMT6_16_SNORM: return 2;
    case a6xx::FMT6_8_8_SNORM: return 2;
    case a6xx::FMT6_8_8_UINT: return 2;
    case a6xx::FMT6_8_8_SINT: return 2;
    case a6xx::FMT6_32_SINT: return 4;
    case a6xx::FMT6_16_16_UNORM: return 4;
    case a6xx::FMT6_16_16_SNORM: return 4;
    case a6xx::FMT6_16_16_UINT: return 4;
    case a6xx::FMT6_16_16_SINT: return 4;
    case a6xx::FMT6_8_8_8_8_SNORM: return 4;
    case a6xx::FMT6_8_8_8_8_UINT: return 4;
    case a6xx::FMT6_8_8_8_8_SINT: return 4;
    case a6xx::FMT6_32_32_UINT: return 8;
    case a6xx::FMT6_32_32_SINT: return 8;
    case a6xx::FMT6_16_16_16_16_UNORM: return 8;
    case a6xx::FMT6_16_16_16_16_SNORM: return 8;
    case a6xx::FMT6_16_16_16_16_UINT: return 8;
    case a6xx::FMT6_16_16_16_16_SINT: return 8;
    case a6xx::FMT6_32_32_32_32_SINT: return 16;
    default: return 0;
    }
}

// one element of a vertex attribute in the formats render.cpp serves (fd6_format_table.c's vertex column); 0 for another
uint32_t vertexElementBytes(uint32_t fmt) {
    // the element's size read off the format's name, channel widths summed: kept apart from render.cpp's table, so a
    // size the driver gets wrong there faults here
    switch (fmt) {
    case a6xx::FMT6_8_UNORM: return 1;
    case a6xx::FMT6_8_SNORM: return 1;
    case a6xx::FMT6_8_UINT: return 1;
    case a6xx::FMT6_8_SINT: return 1;
    case a6xx::FMT6_8_8_UNORM: return 2;
    case a6xx::FMT6_8_8_SNORM: return 2;
    case a6xx::FMT6_8_8_UINT: return 2;
    case a6xx::FMT6_8_8_SINT: return 2;
    case a6xx::FMT6_8_8_8_UNORM: return 3;
    case a6xx::FMT6_8_8_8_SNORM: return 3;
    case a6xx::FMT6_8_8_8_UINT: return 3;
    case a6xx::FMT6_8_8_8_SINT: return 3;
    case a6xx::FMT6_8_8_8_8_UNORM: return 4;
    case a6xx::FMT6_8_8_8_8_SNORM: return 4;
    case a6xx::FMT6_8_8_8_8_UINT: return 4;
    case a6xx::FMT6_8_8_8_8_SINT: return 4;
    case a6xx::FMT6_16_UNORM: return 2;
    case a6xx::FMT6_16_SNORM: return 2;
    case a6xx::FMT6_16_UINT: return 2;
    case a6xx::FMT6_16_SINT: return 2;
    case a6xx::FMT6_16_FLOAT: return 2;
    case a6xx::FMT6_16_16_UNORM: return 4;
    case a6xx::FMT6_16_16_SNORM: return 4;
    case a6xx::FMT6_16_16_UINT: return 4;
    case a6xx::FMT6_16_16_SINT: return 4;
    case a6xx::FMT6_16_16_FLOAT: return 4;
    case a6xx::FMT6_11_11_10_FLOAT: return 4;
    case a6xx::FMT6_10_10_10_2_UNORM: return 4;
    case a6xx::FMT6_10_10_10_2_UNORM_DEST: return 4;
    case a6xx::FMT6_16_16_16_UNORM: return 6;
    case a6xx::FMT6_16_16_16_SNORM: return 6;
    case a6xx::FMT6_16_16_16_UINT: return 6;
    case a6xx::FMT6_16_16_16_SINT: return 6;
    case a6xx::FMT6_16_16_16_FLOAT: return 6;
    case a6xx::FMT6_16_16_16_16_UNORM: return 8;
    case a6xx::FMT6_16_16_16_16_SNORM: return 8;
    case a6xx::FMT6_16_16_16_16_UINT: return 8;
    case a6xx::FMT6_16_16_16_16_SINT: return 8;
    case a6xx::FMT6_16_16_16_16_FLOAT: return 8;
    case a6xx::FMT6_32_UINT: return 4;
    case a6xx::FMT6_32_SINT: return 4;
    case a6xx::FMT6_32_FLOAT: return 4;
    case a6xx::FMT6_32_32_UINT: return 8;
    case a6xx::FMT6_32_32_SINT: return 8;
    case a6xx::FMT6_32_32_FLOAT: return 8;
    case a6xx::FMT6_32_32_32_UINT: return 12;
    case a6xx::FMT6_32_32_32_SINT: return 12;
    case a6xx::FMT6_32_32_32_FLOAT: return 12;
    case a6xx::FMT6_32_32_32_32_UINT: return 16;
    case a6xx::FMT6_32_32_32_32_SINT: return 16;
    case a6xx::FMT6_32_32_32_32_FLOAT: return 16;
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

void FakeGpu::addSurface(uint32_t id, uint32_t width, uint32_t height, uint32_t rowBytes, uint32_t pixelFormat, uint32_t offset, uint32_t planes,
                         bool client) {
    std::lock_guard<std::mutex> g(lock_);
    Surface s{width, height, rowBytes, pixelFormat, offset, planes, client, {}, 0};
    // (a two-plane surface carries an NV12 chroma plane of the same row bytes after the luma, as IOSurface lays out a
    // '420f' one: the kernel maps the whole allocation, so the fake's memory holds both)
    const uint64_t chroma = planes == 2 ? uint64_t(rowBytes) * ((height + 1) / 2) : 0;
    s.bytes.resize((uint64_t(offset) + uint64_t(rowBytes) * height + chroma + 0xfff) & ~0xfffull);
    surfaces_[id] = std::move(s);
}

std::vector<uint8_t> FakeGpu::surfaceBytes(uint32_t id) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return {};
    if (it->second.handle != 0) return buffers_.at(it->second.handle).bytes;
    return it->second.bytes;
}

void FakeGpu::writeSurface(uint32_t id, uint64_t at, const void* bytes, uint64_t length) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return;
    std::vector<uint8_t>& v = it->second.handle != 0 ? buffers_.at(it->second.handle).bytes : it->second.bytes;
    if (at + length <= v.size()) memcpy(v.data() + at, bytes, length);
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

std::vector<uint8_t> FakeGpu::memory(uint64_t iova, uint64_t length) {
    std::lock_guard<std::mutex> g(lock_);
    const uint8_t* p = at(iova, length);
    return p ? std::vector<uint8_t>(p, p + length) : std::vector<uint8_t>{};
}

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
        // the 3D pipeline's one IBO table, by the plain CP_LOAD_STATE6 (tu_pipeline.cc:183) and as ST6_SHADER: 64-byte
        // descriptors from memory, judged at the draw against SP_IBO and SP_IBO_COUNT
        if (opcode != CP_LOAD_STATE6 || type != ST6_SHADER || src != SS6_INDIRECT || count != 3) {
            fault(std::string(op) + text(" into SB6_IBO of type %llu, source %llu: the plain CP_LOAD_STATE6 of ST6_SHADER from memory is what this GPU model runs", type, src));
            return false;
        }
        {
            const uint64_t from = (p[1] & CP_LOAD_STATE6_1_EXT_SRC_ADDR__MASK) | uint64_t(p[2]) << 32;
            if (at(from, uint64_t(units) * 64) == nullptr) {
                fault(text("a load of %llu IBO descriptors from %#llx: outside every buffer", units, from));
                return false;
            }
            gfxIbosLoaded_ = true;
            gfxIbosFrom_ = from;
            gfxIboUnits_ = units;
        }
        return true;
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
        if (type == ST6_CONSTANTS && from % 16 != 0) {
            // the CP reads an indirect source from the address rounded down to 16 bytes (probe 9o4): constants from
            // anywhere else load the bytes before the ones asked for
            fault(text("constants loaded from %#llx, which is not a multiple of 16: the CP would read from %#llx", from, from & ~0xfull));
            return false;
        }
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
    case ST6_UBO:
        // the descriptors themselves, two words each, as the draw (or the dispatch) will read them: from the packet
        if (src == SS6_DIRECT) {
            auto& u = xs ? xs->ubos : cs_.ubos;
            u.resize(std::max<size_t>(u.size(), dstOff + units));
            for (uint32_t i = 0; i < units; i++) {
                const uint32_t lo = p[3 + 2 * i], hi = p[4 + 2 * i];
                u[dstOff + i] = UboDescriptor{lo | uint64_t(hi & ~A6XX_UBO_1_SIZE__MASK) << 32, (hi & A6XX_UBO_1_SIZE__MASK) >> A6XX_UBO_1_SIZE__SHIFT};
            }
        }
        break;
    default:
        break;
    }
    return true;
}

// The IBO descriptors at TABLE, NIBO of them, as a dispatch's and a draw's are judged: each a buffer view whose
// elements lie inside a buffer or a 2D image check2d takes, never all zeros.
bool FakeGpu::checkIboDescriptors(uint64_t table, uint32_t nibo) {
    using namespace a6xx;
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
        if (type == A6XX_TEX_2D || type == A6XX_TEX_3D) {
            if (!check2d(d, "IBO", i, false)) {
                return false;
            }
            continue;
        }
        if (type != A6XX_TEX_BUFFER) {
            fault(text("IBO %llu is of type %llu: not a buffer, a 2D or a 3D image descriptor", i, type));
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

    return true;
}

// A 2D descriptor of a linear texture or image, as fdl6_view_init writes one (fd6_view.c:232-250 for the sampled view,
// 383-397 for the storage one): a format this model knows, a width and height of at least 1, a pitch that holds a row
// and is a multiple of 64 bytes (of 64 << PITCHALIGN for the sampled view, which carries the alignment the layout chose;
// the storage view carries no PITCHALIGN), a base 64-byte aligned (fdl6_layout's base_align), the rows inside a buffer
// counted to a multiple of 4 as the layout counts them for its last level (fd6_layout.c:249-250: the TP may over-fetch
// them), one level, and its layers (if more than one) apart by ARRAY_PITCH inside the buffer.
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
    // (a multisample view's rows hold its SAMPLES samples a pixel side by side)
    const uint32_t ns = sampled ? 1u << fieldOf(d[0], A6XX_TEX_CONST_0_SAMPLES__MASK, A6XX_TEX_CONST_0_SAMPLES__SHIFT) : 1u;
    bpp *= ns;
    uint64_t base = d[4] | uint64_t(fieldOf(d[5], A6XX_TEX_CONST_5_BASE_HI__MASK, A6XX_TEX_CONST_5_BASE_HI__SHIFT)) << 32;
    const uint32_t tile = fieldOf(d[0], A6XX_TEX_CONST_0_TILE_MODE__MASK, A6XX_TEX_CONST_0_TILE_MODE__SHIFT);
    // (GMEM's layout first: its rows are the bin's, not the image's)
    if (tile == TILE6_2) {
        // a bin in GMEM (a framebuffer fetch in a tile, tu_emit_input_attachments): sampled, in a tile, a 2D of one
        // layer whose rows are the bin's, below the CCU's colour cache -- nothing of memory is read
        uint32_t binW = 0, binH = 0, win[4] = {};
        if (!sampled || !inTile_ || !tileState(&binW, &binH, win)) {
            if (inTile_) return false;
            fault(who + ": GMEM's layout (TILE6_2) outside a tile");
            return false;
        }
        const uint32_t ccu = reg(REG_A6XX_RB_CCU_CNTL);
        const uint64_t cache = uint64_t(fieldOf(ccu, A6XX_RB_CCU_CNTL_COLOR_OFFSET__MASK, A6XX_RB_CCU_CNTL_COLOR_OFFSET__SHIFT)) << A6XX_RB_CCU_CNTL_COLOR_OFFSET__SCALE |
                               uint64_t(fieldOf(ccu, A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI__MASK, A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI__SHIFT)) << 21;
        if (pitch != uint64_t(binW) * bpp || base + uint64_t(binW) * binH * bpp > cache ||
            fieldOf(d[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT) != A6XX_TEX_2D ||
            fieldOf(d[5], A6XX_TEX_CONST_5_DEPTH__MASK, A6XX_TEX_CONST_5_DEPTH__SHIFT) != 1) {
            fault(who + text(": a GMEM read of pitch %llu at %#llx: ", pitch, base) + text("not the %llu-pixel bin's rows, ", binW) +
                  "past the colour cache, or not a 2D of one layer");
            return false;
        }
        return true;
    }
    if (pitch < uint64_t(width) * bpp || pitch % unit != 0) {
        fault(who + text(": pitch %llu for rows of %llu pixels of %llu bytes: shorter than a row or not a multiple of ", pitch, width, bpp) +
              text("%llu bytes (64 << PITCHALIGN %llu)", unit, pitchalign));
        return false;
    }
    // linear, or a depth layout's tiles (TILE6_3 on every level, so TILE_ALL; 32 bits a texel, its rows counted to 16 as
    // fdl6_tile_alignment counts them) -- sampled only, nothing stores into tiles here
    if (tile != TILE6_LINEAR && (tile != TILE6_3 || !sampled || bpp != 4 || (d[3] & A6XX_TEX_CONST_3_TILE_ALL) == 0)) {
        fault(who + text(": tile mode %llu", tile) + ((d[3] & A6XX_TEX_CONST_3_TILE_ALL) ? " with TILE_ALL" : " without TILE_ALL") +
              ": linear, or a sampled 32-bit depth layout (TILE6_3, TILE_ALL), is what this GPU model reads");
        return false;
    }
    uint64_t rows = tile == TILE6_3 ? (uint64_t(height) + 15) & ~uint64_t(15) : (uint64_t(height) + 3) & ~uint64_t(3);
    uint64_t reach = (rows - 1) * pitch + uint64_t(width) * bpp;
    if (base % 64 != 0 || at(base, reach) == nullptr) {
        fault(who + text(": %llu rows (%llu counted) of %llu bytes", height, rows, pitch) +
              text(" at %#llx: outside every buffer or not 64-byte aligned", base));
        return false;
    }
    if (sampled && fieldOf(d[0], A6XX_TEX_CONST_0_MIPLVLS__MASK, A6XX_TEX_CONST_0_MIPLVLS__SHIFT) != 0) {
        fault(who + ": more than one level: not what this GPU model runs");
        return false;
    }
    // more than one layer (a 2D array, or a 3D view's slices): each its rows ARRAY_PITCH after the last, as fdl6_layout
    // lays them out 4 KiB aligned (fd6_layout.c:245-262), and the last layer inside the same buffer as the first
    uint32_t depth = fieldOf(d[5], A6XX_TEX_CONST_5_DEPTH__MASK, A6XX_TEX_CONST_5_DEPTH__SHIFT);
    // (a cube view counts cubes: its layers are six times DEPTH)
    if (fieldOf(d[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT) == A6XX_TEX_CUBE) depth *= 6;
    if (depth == 0) {
        fault(who + ": DEPTH 0");
        return false;
    }
    if (depth > 1) {
        const uint64_t layer = uint64_t(fieldOf(d[3], A6XX_TEX_CONST_3_ARRAY_PITCH__MASK, A6XX_TEX_CONST_3_ARRAY_PITCH__SHIFT)) << A6XX_TEX_CONST_3_ARRAY_PITCH__SCALE;
        if (layer < rows * pitch || at(base, (depth - 1) * layer + reach) == nullptr) {
            fault(who + text(": %llu layers %llu bytes apart, of %llu rows", depth, layer, rows) + text(" of %llu bytes each", pitch) +
                  text(" at %#llx: layers that overlap, or the last outside the buffer of the first", base));
            return false;
        }
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
bool FakeGpu::execCs(const uint32_t* p, uint32_t count, bool indirect) {
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
        // (an indirect dispatch's NDRANGE global sizes are the CP's to fill from the counts, as it runs: turnip leaves them 0)
        if (!indirect && global[i] != local[i] * groups[i]) {
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
        if (!checkIboDescriptors(table, nibo)) {
            return false;
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
            const uint32_t ty = fieldOf(d[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT);
            if (ty != A6XX_TEX_2D && ty != A6XX_TEX_3D && ty != A6XX_TEX_1D && ty != A6XX_TEX_CUBE) {
                fault(text("texture %llu is of type %llu: not a 1D, 2D, cube or 3D descriptor this GPU model runs", i, fieldOf(d[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT)));
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
    // (a base is a multiple of 4: probe 9o2 measured the ldc taking one at 4, 8 and 120 as given, not rounded to a vec4)
    for (size_t u = 0; u < cs_.ubos.size(); u++) {
        const UboDescriptor& ub = cs_.ubos[u];
        if (ub.base == 0 && ub.vec4s == 0) continue;
        if (ub.vec4s == 0 || at(ub.base, uint64_t(ub.vec4s) * 16) == nullptr || ub.base % 4 != 0) {
            fault(text("UBO descriptor %llu of the compute stage: %llu vec4 at %#llx, empty, misaligned or outside every buffer", u, ub.vec4s, ub.base));
            return false;
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

// CP_BLIT: the 2D engine. The solid fill a Clear is (r2d_setup_common with clear = true, tu_clear_blit.cc:455-521), and
// the copy render::encodeBlit writes (clear false: SP_PS_2D_SRC_* the source, GRAS_2D_SRC_* its rectangle): texel for
// texel at the same size, bilinear at another with FILTER (each destination pixel's centre mapped into the source),
// nearest without. Only between linear images of one format: a conversion is not modelled.
bool FakeGpu::execBlit(const uint32_t* p, uint32_t count) {
    using namespace a6xx;
    if (count != 1 || fieldOf(p[0], CP_BLIT_0_OP__MASK, CP_BLIT_0_OP__SHIFT) != BLIT_OP_SCALE) {
        fault(text("CP_BLIT of %llu words, op %llu: one word, BLIT_OP_SCALE (3), is what this GPU model runs", count, count ? fieldOf(p[0], CP_BLIT_0_OP__MASK, CP_BLIT_0_OP__SHIFT) : 0));
        return false;
    }
    const uint32_t rb = reg(REG_A6XX_RB_2D_BLIT_CNTL), gras = reg(REG_A6XX_GRAS_2D_BLIT_CNTL);
    if (rb != gras) {
        fault(text("RB_2D_BLIT_CNTL %#llx and GRAS_2D_BLIT_CNTL %#llx differ: the two copies of the blit word must agree", rb, gras));
        return false;
    }
    const bool copy = (rb & A6XX_RB_2D_BLIT_CNTL_SOLID_COLOR) == 0;
    if (fieldOf(rb, A6XX_RB_2D_BLIT_CNTL_MASK__MASK, A6XX_RB_2D_BLIT_CNTL_MASK__SHIFT) != 0xf ||
        fieldOf(reg(REG_A6XX_SP_2D_DST_FORMAT), A6XX_SP_2D_DST_FORMAT_MASK__MASK, A6XX_SP_2D_DST_FORMAT_MASK__SHIFT) != 0xf) {
        fault("a fill whose component mask is not 0xf in RB_2D_BLIT_CNTL and SP_2D_DST_FORMAT: some channels would keep what they held");
        return false;
    }
    const uint32_t info = reg(REG_A6XX_RB_2D_DST_INFO), sp = reg(REG_A6XX_SP_2D_DST_FORMAT);
    const uint32_t fmt = fieldOf(rb, A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT__MASK, A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT__SHIFT);
    if (fieldOf(info, A6XX_RB_2D_DST_INFO_COLOR_FORMAT__MASK, A6XX_RB_2D_DST_INFO_COLOR_FORMAT__SHIFT) != fmt ||
        fieldOf(sp, A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT__MASK, A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT__SHIFT) != fmt) {
        fault(text("the fill's format differs between RB_2D_BLIT_CNTL (%llu), RB_2D_DST_INFO (%llu) and SP_2D_DST_FORMAT (%llu)", fmt,
                   fieldOf(info, A6XX_RB_2D_DST_INFO_COLOR_FORMAT__MASK, A6XX_RB_2D_DST_INFO_COLOR_FORMAT__SHIFT),
                   fieldOf(sp, A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT__MASK, A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT__SHIFT)));
        return false;
    }
    const uint32_t bpp = bytesPerPixel(fmt);
    // a linear image of a known format, or the one tiled image this driver makes: a depth texture (FMT6_32_FLOAT, TILE6_3)
    const uint32_t tile = fieldOf(info, A6XX_RB_2D_DST_INFO_TILE_MODE__MASK, A6XX_RB_2D_DST_INFO_TILE_MODE__SHIFT);
    const bool tiled = tile == TILE6_3 && (fmt == FMT6_32_FLOAT || fmt == FMT6_8_UINT);
    if (bpp == 0 || (tile != TILE6_LINEAR && !tiled)) {
        fault(text("a fill into format %llu, tile mode %llu: a linear image of a known format (or a TILE6_3 depth texture) is what this GPU model fills", fmt, tile));
        return false;
    }
    // the intermediate format turnip's format_to_ifmt picks for each (tu_clear_blit.cc:41-71): by the red channel's size
    const uint32_t ifmt = fieldOf(rb, A6XX_RB_2D_BLIT_CNTL_IFMT__MASK, A6XX_RB_2D_BLIT_CNTL_IFMT__SHIFT);
    const bool srgbIfmt = ifmt == R2D_UNORM8_SRGB;
    uint32_t want = 0;
    switch (fmt) {
    case FMT6_A8_UNORM: case FMT6_8_UNORM: case FMT6_8_8_UNORM: case FMT6_8_8_8_8_UNORM: case FMT6_5_6_5_UNORM: case FMT6_5_5_5_1_UNORM:
        want = srgbIfmt ? uint32_t(R2D_UNORM8_SRGB) : uint32_t(R2D_UNORM8);
        break;
    case FMT6_16_FLOAT: case FMT6_16_16_FLOAT: case FMT6_16_16_16_16_FLOAT: case FMT6_11_11_10_FLOAT: case FMT6_10_10_10_2_UNORM_DEST:
        want = R2D_FLOAT16;
        break;
    case FMT6_32_FLOAT: case FMT6_32_32_FLOAT: case FMT6_32_32_32_32_FLOAT: want = R2D_FLOAT32; break;
    case FMT6_32_UINT: case FMT6_32_32_32_32_UINT: want = R2D_INT32; break;
    case FMT6_8_UINT: want = R2D_INT8; break;
    default: break;
    }
    if (ifmt != want) {
        fault(text("a fill of format %llu through intermediate format %llu: format_to_ifmt picks %llu", fmt, ifmt, want));
        return false;
    }
    const bool srgbInfo = (info & A6XX_RB_2D_DST_INFO_SRGB) != 0, srgbSp = (sp & A6XX_SP_2D_DST_FORMAT_SRGB) != 0;
    if (srgbInfo != srgbIfmt || srgbSp != srgbIfmt) {
        fault("the sRGB bits of RB_2D_DST_INFO and SP_2D_DST_FORMAT disagree with the intermediate format R2D_UNORM8_SRGB");
        return false;
    }
    const uint32_t tl = reg(REG_A6XX_GRAS_2D_DST_TL), br = reg(REG_A6XX_GRAS_2D_DST_BR);
    const uint32_t x0 = fieldOf(tl, A6XX_GRAS_2D_DST_TL_X__MASK, 0), y0 = tl >> 16 & 0x3fff;
    const uint32_t x1 = fieldOf(br, A6XX_GRAS_2D_DST_BR_X__MASK, 0), y1 = br >> 16 & 0x3fff;
    const uint64_t base = reg64(REG_A6XX_RB_2D_DST);
    const uint64_t pitch = uint64_t(fieldOf(reg(REG_A6XX_RB_2D_DST_PITCH), A6XX_RB_2D_DST_PITCH__MASK, A6XX_RB_2D_DST_PITCH__SHIFT)) << A6XX_RB_2D_DST_PITCH__SCALE;
    if (x1 < x0 || y1 < y0 || pitch < uint64_t(x1 + 1) * bpp || base % 64 != 0) {
        fault(text("a fill of (%llu, %llu) to ", x0, y0) + text("(%llu, %llu) with a pitch of ", x1, y1) + text("%llu bytes at %#llx: an empty rectangle, rows wider than the pitch or a base not 64-aligned", pitch, base));
        return false;
    }
    if (copy) {
        return execCopy(fmt, bpp, ifmt, x0, y0, x1, y1, base, pitch, tiled);
    }
    if (tiled) {
        // a TILE6_3 image: 4096-aligned, the pitch a multiple of 256 bytes, its rows counted to 16 (fdl6_tile_alignment
        // for cpp 4). Its bytes are in an order this model does not reproduce, so the fill is judged and not written.
        // (an S8 plane: cpp 1, so its pitch is a multiple of 128 bytes and its rows are counted to 32)
        const bool s8 = fmt == FMT6_8_UINT;
        const uint64_t rows = (uint64_t(y1) + 1 + (s8 ? 31 : 15)) / (s8 ? 32 : 16) * (s8 ? 32 : 16);
        if (base % 4096 != 0 || pitch % (s8 ? 128 : 256) != 0 || at(base, rows * pitch) == nullptr) {
            fault(text("a tiled fill at %#llx with a pitch of %llu bytes, %llu rows: not 4096-aligned, not a multiple of 256, or outside every buffer", base, pitch, rows));
            return false;
        }
        stats_.fills++;
        stats_.tiledFills++;
        drawsInSubmission_++;
        drawsWithoutCcuFlush_++;
        drawsWithoutCacheFlush_++;
        return true;
    }
    uint8_t* d = at(base + y0 * pitch, (y1 - y0) * pitch + uint64_t(x1 + 1) * bpp);
    if (d == nullptr) {
        fault(text("a fill at %#llx, %llu rows of %llu bytes: outside every buffer", base, y1 - y0 + 1, pitch));
        return false;
    }
    // one pixel's bytes from the four solid words: 8-bit channels in the swap's order (WXYZ puts B first), halfs and words as they are
    const uint32_t swap = fieldOf(info, A6XX_RB_2D_DST_INFO_COLOR_SWAP__MASK, A6XX_RB_2D_DST_INFO_COLOR_SWAP__SHIFT);
    uint32_t c[4];
    for (uint32_t i = 0; i < 4; i++) c[i] = reg(REG_A6XX_RB_2D_SRC_SOLID_C0 + i);
    uint8_t pixel[16] = {};
    if (ifmt == R2D_UNORM8 || ifmt == R2D_UNORM8_SRGB) {
        static const uint32_t rgba[4] = {0, 1, 2, 3}, bgra[4] = {2, 1, 0, 3};
        for (uint32_t i = 0; i < bpp; i++) pixel[i] = uint8_t(c[(swap == WXYZ ? bgra : rgba)[i]]);
        if (fmt == FMT6_A8_UNORM) pixel[0] = uint8_t(c[3]);   // the one channel is alpha: the fill's fourth word (as measured, probe 9v)
        if (fmt == FMT6_5_6_5_UNORM || fmt == FMT6_5_5_5_1_UNORM) {
            // packed from the low bits in the swap's order, each 8-bit value cut to the channel's width by rounding
            // (probe 9v2 measures the layout: BGR5A1 cleared to red and alpha is 0xfc00)
            static const uint32_t rgba[4] = {0, 1, 2, 3}, bgra[4] = {2, 1, 0, 3};
            const uint32_t* order = swap == WXYZ ? bgra : rgba;
            const uint32_t w565[3] = {5, 6, 5}, w5551[4] = {5, 5, 5, 1};
            const uint32_t* widths = fmt == FMT6_5_6_5_UNORM ? w565 : w5551;
            uint32_t v = 0, at = 0;
            for (uint32_t i = 0; i < (fmt == FMT6_5_6_5_UNORM ? 3u : 4u); i++) {
                const uint32_t max = (1u << widths[i]) - 1;
                v |= ((c[order[i]] * max + 127) / 255) << at;
                at += widths[i];
            }
            pixel[0] = uint8_t(v);
            pixel[1] = uint8_t(v >> 8);
        }
    } else if (ifmt == R2D_FLOAT16) {
        for (uint32_t i = 0; i < bpp / 2; i++) { uint16_t v = uint16_t(c[i]); memcpy(pixel + 2 * i, &v, 2); }
    } else {
        for (uint32_t i = 0; i < bpp / 4; i++) memcpy(pixel + 4 * i, &c[i], 4);
    }
    for (uint32_t y = y0; y <= y1; y++) {
        for (uint32_t x = x0; x <= x1; x++) memcpy(d + (y - y0) * pitch + uint64_t(x) * bpp, pixel, bpp);
    }
    stats_.fills++;
    drawsInSubmission_++;   // the fill sits in the CCU as a draw's colour does: the same two flushes must follow it
    drawsWithoutCcuFlush_++;
    drawsWithoutCacheFlush_++;
    return true;
}

bool FakeGpu::execCopy(uint32_t fmt, uint32_t bpp, uint32_t ifmt, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint64_t base, uint64_t pitch, bool tiled) {
    using namespace a6xx;
    if (tiled) {
        fault("a copy into a tiled image: not modelled");
        return false;
    }
    const uint32_t info = reg(REG_A6XX_SP_PS_2D_SRC_INFO), size = reg(REG_A6XX_SP_PS_2D_SRC_SIZE);
    const uint32_t sfmt = info & A6XX_SP_PS_2D_SRC_INFO_COLOR_FORMAT__MASK;
    // (the TP's format at the source, the colour column's at the destination: they differ for RGB10A2 alone)
    const bool sameFormat = sfmt == fmt || (sfmt == FMT6_10_10_10_2_UNORM && fmt == FMT6_10_10_10_2_UNORM_DEST);
    if (!sameFormat || fieldOf(info, A6XX_SP_PS_2D_SRC_INFO_TILE_MODE__MASK, A6XX_SP_PS_2D_SRC_INFO_TILE_MODE__SHIFT) != TILE6_LINEAR ||
        fieldOf(info, A6XX_SP_PS_2D_SRC_INFO_COLOR_SWAP__MASK, A6XX_SP_PS_2D_SRC_INFO_COLOR_SWAP__SHIFT) !=
            fieldOf(reg(REG_A6XX_RB_2D_DST_INFO), A6XX_RB_2D_DST_INFO_COLOR_SWAP__MASK, A6XX_RB_2D_DST_INFO_COLOR_SWAP__SHIFT)) {
        fault(text("a copy from format %llu into %llu, or from a tiled source, or across swaps: one linear format is what this model copies", sfmt, fmt));
        return false;
    }
    const uint32_t sw = size & A6XX_SP_PS_2D_SRC_SIZE_WIDTH__MASK, sh = fieldOf(size, A6XX_SP_PS_2D_SRC_SIZE_HEIGHT__MASK, A6XX_SP_PS_2D_SRC_SIZE_HEIGHT__SHIFT);
    const uint64_t sbase = reg64(REG_A6XX_SP_PS_2D_SRC);
    const uint64_t spitch = uint64_t(fieldOf(reg(REG_A6XX_SP_PS_2D_SRC_PITCH), A6XX_SP_PS_2D_SRC_PITCH_PITCH__MASK, A6XX_SP_PS_2D_SRC_PITCH_PITCH__SHIFT)) << 6;
    // a multisample source (a resolve): NS samples side by side in each pixel of its rows
    const uint32_t ns = 1u << fieldOf(info, A6XX_SP_PS_2D_SRC_INFO_SAMPLES__MASK, A6XX_SP_PS_2D_SRC_INFO_SAMPLES__SHIFT);
    const bool average = (info & A6XX_SP_PS_2D_SRC_INFO_SAMPLES_AVERAGE) != 0;
    if (ns > 4 || (average && ns == 1)) {
        fault(text("a copy from a source of %llu samples", ns) + (average ? ", averaged" : "") + ": 1, 2 or 4, averaged only when more than one");
        return false;
    }
    auto coord = [&](uint32_t r) { return fieldOf(reg(r), A6XX_GRAS_2D_SRC_TL_X__MASK, A6XX_GRAS_2D_SRC_TL_X__SHIFT); };
    const uint32_t sx0 = coord(REG_A6XX_GRAS_2D_SRC_TL_X), sx1 = coord(REG_A6XX_GRAS_2D_SRC_BR_X), sy0 = coord(REG_A6XX_GRAS_2D_SRC_TL_Y),
                   sy1 = coord(REG_A6XX_GRAS_2D_SRC_BR_Y);
    if (sw == 0 || sh == 0 || sx1 < sx0 || sy1 < sy0 || sx1 >= sw || sy1 >= sh || spitch < uint64_t(sw) * bpp * ns || sbase % 64 != 0 ||
        at(sbase, uint64_t(sh - 1) * spitch + uint64_t(sw) * bpp * ns) == nullptr) {
        fault(text("a copy from (%llu, %llu) ", sx0, sy0) + text("to (%llu, %llu) of a ", sx1, sy1) + text("%llu x %llu source", sw, sh) +
              text(" with a pitch of %llu at %#llx: outside the source, its buffer, or not 64-aligned", spitch, sbase));
        return false;
    }
    if (at(base + uint64_t(y0) * pitch, uint64_t(y1 - y0) * pitch + uint64_t(x1 + 1) * bpp) == nullptr) {
        fault(text("a copy into %#llx, %llu rows: outside every buffer", base, y1 - y0 + 1));
        return false;
    }
    const uint32_t dw = x1 - x0 + 1, dh = y1 - y0 + 1, srw = sx1 - sx0 + 1, srh = sy1 - sy0 + 1;
    const bool filter = (info & A6XX_SP_PS_2D_SRC_INFO_FILTER) != 0;
    if (ns > 1 && (srw != dw || srh != dh || filter || (average && ifmt != R2D_UNORM8))) {
        fault("a resolve that scales, filters, or averages a format other than 8-bit unorm: not modelled");
        return false;
    }
    // the source read whole first: a copy within one buffer reads what was there before it
    const uint64_t srcRow = uint64_t(sw) * bpp * ns;
    std::vector<uint8_t> source(uint64_t(sh) * spitch);
    memcpy(source.data(), at(sbase, source.size() - spitch + srcRow), source.size() - spitch + srcRow);
    auto texel = [&](int64_t x, int64_t y) {
        x = std::min<int64_t>(std::max<int64_t>(x, 0), sw - 1);
        y = std::min<int64_t>(std::max<int64_t>(y, 0), sh - 1);
        return source.data() + uint64_t(y) * spitch + uint64_t(x) * bpp * ns;
    };
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            uint8_t* out = at(base + uint64_t(y0 + y) * pitch + uint64_t(x0 + x) * bpp, bpp);
            if (ns > 1 && average) {
                // the mean of the pixel's samples, byte by byte (each a unorm channel), rounded to nearest
                const uint8_t* px = texel(sx0 + x, sy0 + y);
                for (uint32_t c = 0; c < bpp; c++) {
                    uint32_t sum = 0;
                    for (uint32_t k = 0; k < ns; k++) sum += px[k * bpp + c];
                    out[c] = uint8_t((sum + ns / 2) / ns);
                }
                continue;
            }
            if (srw == dw && srh == dh) {
                memcpy(out, texel(sx0 + x, sy0 + y), bpp);   // (a multisample source unaveraged: its sample 0)
                continue;
            }
            const double u = sx0 + (x + 0.5) * srw / dw - 0.5, v = sy0 + (y + 0.5) * srh / dh - 0.5;
            if (!filter) {
                memcpy(out, texel(int64_t(std::floor(u + 0.5)), int64_t(std::floor(v + 0.5))), bpp);
                continue;
            }
            if (ifmt != R2D_UNORM8) {
                fault("a filtered scale of a format other than 8-bit unorm: not modelled");
                return false;
            }
            const int64_t ix = int64_t(std::floor(u)), iy = int64_t(std::floor(v));
            const double fx = u - ix, fy = v - iy;
            for (uint32_t c = 0; c < bpp; c++) {
                const double t = texel(ix, iy)[c] * (1 - fx) * (1 - fy) + texel(ix + 1, iy)[c] * fx * (1 - fy) + texel(ix, iy + 1)[c] * (1 - fx) * fy +
                                 texel(ix + 1, iy + 1)[c] * fx * fy;
                out[c] = uint8_t(std::lround(t));
            }
        }
    }
    stats_.copies++;
    drawsInSubmission_++;   // the copy sits in the CCU as a fill does: the same two flushes must follow it
    drawsWithoutCcuFlush_++;
    drawsWithoutCacheFlush_++;
    return true;
}

// A GMEM tile's state, as a draw or an event blit in it needs it: GRAS and RB_BIN_CONTROL alike with BUFFERS_IN_GMEM
// and a bin, RB_BIN_CONTROL2 the same bin, the window scissor no larger than the bin with its corner where all four
// window offsets put GMEM's origin (tu6_emit_tile_select); the bin's size and the window, X1 Y1 X2 Y2 inclusive, out
bool FakeGpu::tileState(uint32_t* binW, uint32_t* binH, uint32_t win[4]) {
    using namespace a6xx;
    const uint32_t g = reg(REG_A6XX_GRAS_BIN_CONTROL), r = reg(REG_A6XX_RB_BIN_CONTROL), r2 = reg(REG_A6XX_RB_BIN_CONTROL2);
    *binW = fieldOf(g, A6XX_GRAS_BIN_CONTROL_BINW__MASK, A6XX_GRAS_BIN_CONTROL_BINW__SHIFT) << A6XX_GRAS_BIN_CONTROL_BINW__SCALE;
    *binH = fieldOf(g, A6XX_GRAS_BIN_CONTROL_BINH__MASK, A6XX_GRAS_BIN_CONTROL_BINH__SHIFT) << A6XX_GRAS_BIN_CONTROL_BINH__SCALE;
    if (g != r || fieldOf(g, A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION__MASK, A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION__SHIFT) != BUFFERS_IN_GMEM || *binW == 0 ||
        *binH == 0 || r2 != (A6XX_RB_BIN_CONTROL2_BINW(*binW) | A6XX_RB_BIN_CONTROL2_BINH(*binH))) {
        fault(text("GRAS_BIN_CONTROL %#llx, RB_BIN_CONTROL %#llx, RB_BIN_CONTROL2 %#llx in a tile: the same bin in all three, in GMEM", g, r, r2));
        return false;
    }
    const uint32_t tl = reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL), br = reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR);
    win[0] = tl & 0x3fff;
    win[1] = tl >> 16 & 0x3fff;
    win[2] = br & 0x3fff;
    win[3] = br >> 16 & 0x3fff;
    bool offsets = true;
    for (uint32_t o : {REG_A6XX_RB_WINDOW_OFFSET, REG_A6XX_RB_WINDOW_OFFSET2, REG_A6XX_SP_WINDOW_OFFSET, REG_A6XX_SP_TP_WINDOW_OFFSET}) {
        offsets = offsets && reg(o) == tl;
    }
    if (win[2] < win[0] || win[3] < win[1] || win[2] - win[0] + 1 > *binW || win[3] - win[1] + 1 > *binH || !offsets ||
        reg(REG_A6XX_GRAS_2D_RESOLVE_CNTL_1) != tl || reg(REG_A6XX_GRAS_2D_RESOLVE_CNTL_2) != br) {
        fault(text("a tile's window (%#llx)-(%#llx) larger than its bin, or not where the window offsets and the resolve window are", tl, br));
        return false;
    }
    return true;
}

// CP_EVENT_WRITE BLIT in a tile: RB_BLIT_INFO's LOAD or STORE carried out over the tile's part of RB_BLIT_SCISSOR,
// between the destination's rows and GMEM at RB_BLIT_BASE_GMEM, the bin's width a GMEM row (what the draws see there,
// RB_MRT's BASE_GMEM 0 with the window offset at the bin's corner)
bool FakeGpu::eventBlit() {
    using namespace a6xx;
    if (!inTile_) {
        fault("an event blit outside a GMEM tile");
        return false;
    }
    uint32_t binW = 0, binH = 0, win[4] = {};
    if (!tileState(&binW, &binH, win)) return false;
    const uint32_t info = reg(REG_A6XX_RB_BLIT_INFO), type = info & A6XX_RB_BLIT_INFO_TYPE__MASK;
    if (type != BLIT_EVENT_LOAD && type != BLIT_EVENT_STORE && type != BLIT_EVENT_CLEAR) {
        fault(text("RB_BLIT_INFO %#llx: a load, a store or a clear is what this GPU model runs", info));
        return false;
    }
    if (type == BLIT_EVENT_CLEAR) {
        // the tile's part of the blit scissor in GMEM filled with RB_BLIT_CLEAR_COLOR as the base format packs it (all
        // four components: a partial mask is not what this driver writes)
        const uint32_t cbpp = bytesPerPixel(fieldOf(reg(REG_A6XX_RB_BLIT_DST_INFO), A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__MASK, A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__SHIFT));
        const uint32_t cbase = reg(REG_A6XX_RB_BLIT_BASE_GMEM), stl = reg(REG_A6XX_RB_BLIT_SCISSOR_TL), sbr = reg(REG_A6XX_RB_BLIT_SCISSOR_BR);
        if (cbpp == 0 || cbpp > 16 || fieldOf(info, A6XX_RB_BLIT_INFO_CLEAR_MASK__MASK, A6XX_RB_BLIT_INFO_CLEAR_MASK__SHIFT) != 0xf ||
            uint64_t(cbase) + uint64_t(binW) * binH * cbpp > gmem_.size()) {
            fault(text("a GMEM clear of %llu-byte pixels at %#llx: a known format, mask 0xf, inside GMEM", cbpp, cbase));
            return false;
        }
        uint32_t words[4];
        for (int k = 0; k < 4; k++) words[k] = reg(REG_A6XX_RB_BLIT_CLEAR_COLOR_DW0 + k);
        const uint32_t x1 = std::max(win[0], stl & 0x3fff), y1 = std::max(win[1], stl >> 16 & 0x3fff);
        const uint32_t x2 = std::min(win[2], sbr & 0x3fff), y2 = std::min(win[3], sbr >> 16 & 0x3fff);
        for (uint32_t y = y1; x1 <= x2 && y <= y2; y++) {
            for (uint32_t x = x1; x <= x2; x++) memcpy(gmem_.data() + cbase + (uint64_t(y - win[1]) * binW + (x - win[0])) * cbpp, words, cbpp);
        }
        stats_.tileClears++;
        return true;
    }
    const uint32_t dstInfo = reg(REG_A6XX_RB_BLIT_DST_INFO);
    const uint32_t bpp = bytesPerPixel(fieldOf(dstInfo, A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__MASK, A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__SHIFT));
    const uint64_t dst = reg64(REG_A6XX_RB_BLIT_DST), pitch = uint64_t(reg(REG_A6XX_RB_BLIT_DST_PITCH) & 0xffff) << 6;
    const uint32_t gbase = reg(REG_A6XX_RB_BLIT_BASE_GMEM);
    const uint32_t stl = reg(REG_A6XX_RB_BLIT_SCISSOR_TL), sbr = reg(REG_A6XX_RB_BLIT_SCISSOR_BR);
    if (bpp == 0 || (dstInfo & A6XX_RB_BLIT_DST_INFO_TILE_MODE__MASK) != TILE6_LINEAR || reg(REG_A6XX_RB_BLIT_GMEM_MSAA_CNTL) != 0) {
        fault(text("RB_BLIT_DST_INFO %#llx: a linear, single-sampled destination of a known format", dstInfo));
        return false;
    }
    const uint32_t x1 = std::max(win[0], stl & 0x3fff), y1 = std::max(win[1], stl >> 16 & 0x3fff);
    const uint32_t x2 = std::min(win[2], sbr & 0x3fff), y2 = std::min(win[3], sbr >> 16 & 0x3fff);
    if (x1 > x2 || y1 > y2) return true;
    const uint64_t rowBytes = uint64_t(x2 - x1 + 1) * bpp;
    const bool swapBR = bpp == 4 && fieldOf(dstInfo, A6XX_RB_BLIT_DST_INFO_COLOR_SWAP__MASK, A6XX_RB_BLIT_DST_INFO_COLOR_SWAP__SHIFT) == WXYZ &&
                        fieldOf(dstInfo, A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__MASK, A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__SHIFT) == FMT6_8_8_8_8_UNORM;
    if (uint64_t(gbase) + uint64_t(binW) * binH * bpp > gmem_.size() || pitch < rowBytes) {
        fault("an event blit past GMEM's end, or a destination pitch shorter than its rows");
        return false;
    }
    for (uint32_t y = y1; y <= y2; y++) {
        uint8_t* m = at(dst + uint64_t(y) * pitch + uint64_t(x1) * bpp, rowBytes);
        if (m == nullptr) {
            fault(text("an event blit's row %llu at %#llx: outside every buffer", y, dst + uint64_t(y) * pitch));
            return false;
        }
        uint8_t* g = gmem_.data() + gbase + (uint64_t(y - win[1]) * binW + (x1 - win[0])) * bpp;
        // (GMEM holds the components in the RB's order, RGBA: a BGRA target's swap is undone by the load and redone by
        // the store -- what a Clear in the tile, packed RGBA, needs to come out as the 2D engine's fill does)
        uint8_t* to = type == BLIT_EVENT_LOAD ? g : m;
        memcpy(to, type == BLIT_EVENT_LOAD ? m : g, rowBytes);
        for (uint64_t o = 0; swapBR && o < rowBytes; o += 4) std::swap(to[o], to[o + 2]);
    }
    if (type == BLIT_EVENT_LOAD) stats_.tileLoads++;
    else {
        stats_.tileStores++;
        stats_.storedPixels += uint64_t(x2 - x1 + 1) * (y2 - y1 + 1);
    }
    return true;
}

// CP_DRAW_INDX_OFFSET: the draw itself. As with CP_EXEC_CS, everything the registers and the loads say about it must hold
// together (fake_gpu.hpp's file comment lists what); what the GPU does with state that does not is hang or write
// nothing, and a fault here stands for either. Nothing is rasterized.
bool FakeGpu::execDraw(const uint32_t* p, uint32_t count) {
    using namespace a6xx;
    if (count != 3 && count != 7) {
        fault(text("CP_DRAW_INDX_OFFSET of %llu words: the 3-word auto-indexed form and the 7-word indexed one are what this GPU model runs", count));
        return false;
    }
    Draw d;
    d.primType = fieldOf(p[0], CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__MASK, CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__SHIFT);
    d.sourceSelect = fieldOf(p[0], CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT__MASK, CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT__SHIFT);
    d.visCull = fieldOf(p[0], CP_DRAW_INDX_OFFSET_0_VIS_CULL__MASK, CP_DRAW_INDX_OFFSET_0_VIS_CULL__SHIFT);
    d.numInstances = p[1];
    d.numIndices = p[2];
    if (d.primType != DI_PT_TRILIST && d.primType != DI_PT_TRISTRIP && d.primType != DI_PT_LINELIST && d.primType != DI_PT_LINESTRIP) {
        fault(text("a draw of primitive type %llu: triangle lists (4) and strips (6) and line lists (2) and strips (3) are what this GPU model runs", d.primType));
        return false;
    }
    if ((count == 3) != (d.sourceSelect == DI_SRC_SEL_AUTO_INDEX) || (count == 7 && d.sourceSelect != DI_SRC_SEL_DMA)) {
        fault(text("a draw whose indices come from source %llu in a %llu-word packet: AUTO_INDEX (2) takes 3 words, DMA (0) 7", d.sourceSelect, count));
        return false;
    }
    if (d.numInstances == 0) {
        fault("a draw of 0 instances");
        return false;
    }
    if (d.numIndices == 0) {
        fault("a draw of 0 vertices");
        return false;
    }
    // the indices of a DMA draw: 16 or 32 bits, MAX_INDICES of them from INDX_BASE inside a buffer, the count within it;
    // read here (the model has the memory) for the highest index the fetch below must hold, the restart index skipped
    // when PC_PRIMITIVE_CNTL_0 restarts, and then the restart index the type's all-ones
    d.indexed = count == 7;
    uint64_t highestIndex = 0;
    if (d.indexed) {
        d.indexSize = fieldOf(p[0], CP_DRAW_INDX_OFFSET_0_INDEX_SIZE__MASK, CP_DRAW_INDX_OFFSET_0_INDEX_SIZE__SHIFT);
        d.indexBase = p[4] | uint64_t(p[5]) << 32;
        d.maxIndices = p[6];
        if (d.indexSize != INDEX4_SIZE_16_BIT && d.indexSize != INDEX4_SIZE_32_BIT) {
            fault(text("an indexed draw of index size %llu: 16-bit (1) and 32-bit (2) are what this GPU model runs", d.indexSize));
            return false;
        }
        const uint32_t bytes = d.indexSize == INDEX4_SIZE_16_BIT ? 2 : 4;
        const uint8_t* idx = at(d.indexBase, uint64_t(d.maxIndices) * bytes);
        if (idx == nullptr || d.indexBase % bytes != 0 || p[3] != 0 || uint64_t(d.numIndices) > d.maxIndices) {
            fault(text("an indexed draw of %llu indices from %#llx, ", d.numIndices, d.indexBase) +
                  text("MAX_INDICES %llu, FIRST_INDX %llu: outside every buffer, misaligned, or more than the buffer holds", d.maxIndices, p[3]));
            return false;
        }
        const bool restart = (reg(REG_A6XX_PC_PRIMITIVE_CNTL_0) & A6XX_PC_PRIMITIVE_CNTL_0_PRIMITIVE_RESTART) != 0;
        const uint32_t restartIndex = reg(REG_A6XX_PC_RESTART_INDEX);
        if (restart && restartIndex != (bytes == 2 ? 0xffffu : 0xffffffffu)) {
            fault(text("primitive restart at index %#llx for %llu-byte indices: the type's all-ones is what Metal restarts at", restartIndex, bytes));
            return false;
        }
        for (uint32_t i = 0; i < d.numIndices; i++) {
            uint32_t v = 0;
            memcpy(&v, idx + uint64_t(i) * bytes, bytes);
            if (restart && v == restartIndex) continue;
            highestIndex = std::max<uint64_t>(highestIndex, v);
        }
    }
    // the pass: begun by the marker, in sysmem with no bins (GRAS_BIN_CONTROL and RB_BIN_CONTROL alike), or in a GMEM
    // tile whose bin, window and CCU hold together (tileState)
    if (!passBegun_) {
        fault("a draw before CP_SET_MARKER RM6_DIRECT_RENDER: the sysmem pass was not begun in this submission");
        return false;
    }
    if (binning_) {
        // (a draw of the binning pass: the vertex stage alone runs, into the VSC; GRAS_BIN_CONTROL says so)
        if (fieldOf(reg(REG_A6XX_GRAS_BIN_CONTROL), A6XX_GRAS_BIN_CONTROL_RENDER_MODE__MASK, A6XX_GRAS_BIN_CONTROL_RENDER_MODE__SHIFT) != BINNING_PASS ||
            fieldOf(reg(REG_A6XX_VFD_MODE_CNTL), A6XX_VFD_MODE_CNTL_RENDER_MODE__MASK, A6XX_VFD_MODE_CNTL_RENDER_MODE__SHIFT) != BINNING_PASS) {
            fault("a draw of the binning pass without GRAS_BIN_CONTROL and VFD_MODE_CNTL in BINNING_PASS");
            return false;
        }
        stats_.binnedDraws++;
        return true;
    }
    uint32_t binW = 0, binH = 0, tileWin[4] = {};
    if (inTile_ && !tileState(&binW, &binH, tileWin)) {
        return false;
    }
    for (uint32_t r : {REG_A6XX_GRAS_BIN_CONTROL, REG_A6XX_RB_BIN_CONTROL}) {
        if (inTile_) break;
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
    // the targets: as many as both copies of OUTPUT_CNTL1.MRT say (1 to 8, the same in both), each RB_MRT[i] a linear
    // image of a known format, its pitch holding the window's rows, its rows inside a buffer, SP_FS_MRT_REG[i] the same
    // format, no GMEM base; target 0's format in GRAS_LRZ_MRT_BUF_INFO_0
    d.mrtCount = fieldOf(reg(REG_A6XX_RB_FS_OUTPUT_CNTL1), A6XX_RB_FS_OUTPUT_CNTL1_MRT__MASK, A6XX_RB_FS_OUTPUT_CNTL1_MRT__SHIFT);
    uint64_t gmemSpans[8][2] = {};
    uint32_t spMrt = fieldOf(reg(REG_A6XX_SP_FS_OUTPUT_CNTL1), A6XX_SP_FS_OUTPUT_CNTL1_MRT__MASK, A6XX_SP_FS_OUTPUT_CNTL1_MRT__SHIFT);
    if (d.mrtCount < 1 || d.mrtCount > 8 || spMrt != d.mrtCount) {
        fault(text("SP_FS_OUTPUT_CNTL1.MRT %llu and RB_FS_OUTPUT_CNTL1.MRT %llu: 1 to 8 targets, the same in both", spMrt, d.mrtCount));
        return false;
    }
    uint32_t winBr = reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR);
    d.windowScissor[0] = fieldOf(reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL), A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__MASK, A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__SHIFT);
    d.windowScissor[1] = fieldOf(reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL), A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK, A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__SHIFT);
    d.windowScissor[2] = fieldOf(winBr, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X__MASK, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X__SHIFT);
    d.windowScissor[3] = fieldOf(winBr, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y__MASK, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y__SHIFT);
    uint64_t width = uint64_t(d.windowScissor[2]) + 1, rows = uint64_t(d.windowScissor[3]) + 1;
    if (inTile_) {
        // (a tile's window may reach past the target's edge, as turnip's whole bins do; what the draw writes stops at
        // its screen scissor, which must then be inside the target's rows instead)
        const uint32_t sbr = reg(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_BR);
        width = std::min<uint64_t>(width, (sbr & 0xffff) + 1);
        rows = std::min<uint64_t>(rows, (sbr >> 16) + 1);
    }
    // The sample count, one in all three blocks (tu6_emit_msaa): RAS and DEST alike, MSAA_DISABLE exactly when it is one,
    // at most four (the RB's own maximum here). A target's rows then hold that many samples a pixel.
    {
        const uint32_t s = fieldOf(reg(REG_A6XX_GRAS_DEST_MSAA_CNTL), A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__MASK, A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__SHIFT);
        for (uint32_t r : {REG_A6XX_SP_TP_RAS_MSAA_CNTL, REG_A6XX_SP_TP_DEST_MSAA_CNTL, REG_A6XX_GRAS_RAS_MSAA_CNTL, REG_A6XX_GRAS_DEST_MSAA_CNTL,
                           REG_A6XX_RB_RAS_MSAA_CNTL, REG_A6XX_RB_DEST_MSAA_CNTL}) {
            const uint32_t v = reg(r);
            const bool dest = r == REG_A6XX_SP_TP_DEST_MSAA_CNTL || r == REG_A6XX_GRAS_DEST_MSAA_CNTL || r == REG_A6XX_RB_DEST_MSAA_CNTL;
            if (fieldOf(v, A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__MASK, A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__SHIFT) != s ||
                (dest && ((v & A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE) != 0) != (s == 0)) || s > MSAA_FOUR) {
                fault(text("the MSAA registers disagree (%#llx", v) + text(" against GRAS_DEST_MSAA_CNTL's count %llu): one sample count in all three blocks, "
                                                                            "MSAA_DISABLE with one sample alone", s));
                return false;
            }
        }
        d.samples = 1u << s;
    }
    uint32_t unwrittenMask = 0;
    for (uint32_t t = 0; t < d.mrtCount; t++) {
        const std::string which = std::to_string(t);
        uint32_t bufInfo = reg(regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, t));
        const uint32_t format = fieldOf(bufInfo, A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__MASK, A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__SHIFT);
        const uint32_t tile = fieldOf(bufInfo, A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__MASK, A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__SHIFT);
        uint32_t bpp = bytesPerPixel(format);
        if (bpp == 0) {
            fault("RB_MRT_BUF_INFO[" + which + "]: " + text("COLOR_FORMAT %llu is not one this GPU model knows", format));
            return false;
        }
        if (tile != TILE6_LINEAR) {
            fault("RB_MRT_BUF_INFO[" + which + "]: " + text("tile mode %llu: only linear targets are what this GPU model runs", tile));
            return false;
        }
        const uint64_t pitch = uint64_t(fieldOf(reg(regRbMrt(REG_A6XX_RB_MRT_PITCH, t)), A6XX_RB_MRT_PITCH__MASK, A6XX_RB_MRT_PITCH__SHIFT)) << A6XX_RB_MRT_PITCH__SCALE;
        const uint64_t base = reg64(regRbMrt(REG_A6XX_RB_MRT_BASE, t));
        if (pitch < width * bpp * d.samples) {
            fault("RB_MRT_PITCH[" + which + "] " + text("%llu bytes for rows of %llu pixels", pitch, width) + text(" of %llu bytes", bpp) +
                  text(" and %llu sample(s): shorter than a row", d.samples));
            return false;
        }
        // the rows counted to a multiple of 4, as the layout counts its last level (fd6_layout.c:249-250) and check2d does
        uint64_t counted = (rows + 3) & ~uint64_t(3);
        if (base % kMrtBaseAlign != 0 || at(base, counted * pitch) == nullptr) {
            fault("RB_MRT[" + which + "]: " + text("%llu rows (%llu counted)", rows, counted) + text(" of %llu bytes", pitch) +
                  text(" at %#llx: outside every buffer or not 64-byte aligned", base));
            return false;
        }
        const uint32_t spFsMrtReg = reg(regSpFsMrtReg(t));
        if (fieldOf(spFsMrtReg, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__MASK, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__SHIFT) != format) {
            fault("SP_FS_MRT_REG[" + which + "] " + text("names format %llu, RB_MRT_BUF_INFO %llu",
                                                       fieldOf(spFsMrtReg, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__MASK, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__SHIFT), format));
            return false;
        }
        // (BASE_GMEM: where the target's bin lies in a tile, each inside the GMEM below the CCU's colour cache and the
        // bins of the targets written apart -- gmemSpans, checked below; a sysmem draw has none)
        if (inTile_) {
            gmemSpans[t][0] = reg(regRbMrt(REG_A6XX_RB_MRT_BASE_GMEM, t));
            gmemSpans[t][1] = gmemSpans[t][0] + uint64_t(binW) * binH * bpp;
        } else if (reg(regRbMrt(REG_A6XX_RB_MRT_BASE_GMEM, t)) != 0) {
            fault("RB_MRT_BASE_GMEM[" + which + "] " + text("is %#llx: a sysmem draw has no GMEM base", reg(regRbMrt(REG_A6XX_RB_MRT_BASE_GMEM, t))));
            return false;
        }
        // a slot the stage writes nothing to names no register, and must then write no components (RENDER_COMPONENTS'
        // nibble 0, below) -- else the RB writes what an unset register holds. Every slot may be unwritten: turnip's
        // tu6_emit_fs_outputs (tu_shader.cc:1853-1897) gives a stage without outputs no SP_FS_OUTPUT_REG and RENDER_COMPONENTS
        // 0 beside a pass's colour attachments, which is a depth or stencil pass such as CoreAnimation's path_edges
        const bool unwritten = fieldOf(reg(regSpFsOutputReg(t)), A6XX_SP_FS_OUTPUT_REG_REGID__MASK, A6XX_SP_FS_OUTPUT_REG_REGID__SHIFT) == kRegidNone;
        if (unwritten) unwrittenMask |= 1u << t;
        d.mrtBases[t] = base;
        d.mrtFormats[t] = format;
        if (t == 0) {
            d.mrtFormat = format;
            d.mrtTileMode = tile;
            d.mrtSwap = fieldOf(bufInfo, A6XX_RB_MRT_BUF_INFO_COLOR_SWAP__MASK, A6XX_RB_MRT_BUF_INFO_COLOR_SWAP__SHIFT);
            d.mrtPitch = pitch;
            d.mrtArrayPitch = fieldOf(reg(regRbMrt(REG_A6XX_RB_MRT_ARRAY_PITCH, 0)), A6XX_RB_MRT_ARRAY_PITCH__MASK, A6XX_RB_MRT_ARRAY_PITCH__SHIFT)
                              << A6XX_RB_MRT_ARRAY_PITCH__SCALE;
            d.mrtBase = base;
            d.spFsMrtReg = spFsMrtReg;
        }
    }
    if (fieldOf(reg(REG_A6XX_GRAS_LRZ_MRT_BUF_INFO_0), A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT__MASK, A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT__SHIFT) != d.mrtFormat) {
        fault(text("GRAS_LRZ_MRT_BUF_INFO_0 names format %llu, RB_MRT_BUF_INFO[0] %llu", reg(REG_A6XX_GRAS_LRZ_MRT_BUF_INFO_0), d.mrtFormat));
        return false;
    }
    d.spFsOutputReg0 = reg(regSpFsOutputReg(0));
    // all four components of each target in both copies of RENDER_COMPONENTS
    d.renderComponents = reg(REG_A6XX_RB_RENDER_COMPONENTS);
    d.fsRenderComponents = reg(REG_A6XX_SP_FS_RENDER_COMPONENTS);
    for (uint32_t t = 0; t < d.mrtCount; t++) {
        const uint32_t want = (unwrittenMask >> t) & 1 ? 0u : 0xfu;
        if (d.renderComponents != d.fsRenderComponents || ((d.renderComponents >> (4 * t)) & 0xf) != want) {
            fault(text("RENDER_COMPONENTS %#llx (SP) and %#llx (RB): ", d.fsRenderComponents, d.renderComponents) + "RT" + std::to_string(t) +
                  (want ? " needs all four components in both" : " is written by no register and must name no component"));
            return false;
        }
    }
    // the blend of each target: RB_MRT_CONTROL[i]'s two BLEND bits together, and the same answer in bit i of
    // SP_BLEND_CNTL's and RB_BLEND_CNTL's ENABLE_BLEND (tu6_emit_blend writes all three from one mask); a blending
    // target's factors and opcodes ones the enums have. (The component enable may be anything, 0 included: Metal's
    // MTLColorWriteMaskNone draws and writes nothing.)
    // ...and all of them written in this submission: they are context registers the kernel does not reset, so a stream
    // that leaves them out draws with whatever write mask and blend the last owner of the GPU set
    for (uint32_t t = 0; t < d.mrtCount; t++) {
        const std::string which = std::to_string(t);
        // (target 0's two, then the two the targets share, then each later target's)
        std::vector<uint32_t> must = {regRbMrt(REG_A6XX_RB_MRT_CONTROL, t), regRbMrt(REG_A6XX_RB_MRT_BLEND_CONTROL, t)};
        if (t == 0) {
            must.push_back(REG_A6XX_SP_BLEND_CNTL);
            must.push_back(REG_A6XX_RB_BLEND_CNTL);
        }
        for (uint32_t r : must) {
            if (written_.count(r) == 0) {
                fault(text("blend register %#llx not written in this submission: ", r) + "the draw's write mask and blend would be the last owner's");
                return false;
            }
        }
        const uint32_t mrtControl = reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, t)), mrtBlend = reg(regRbMrt(REG_A6XX_RB_MRT_BLEND_CONTROL, t));
        const bool blend = (mrtControl & A6XX_RB_MRT_CONTROL_BLEND) != 0;
        if (blend != ((mrtControl & A6XX_RB_MRT_CONTROL_BLEND2) != 0) ||
            blend != (((reg(REG_A6XX_SP_BLEND_CNTL) >> t) & 1u) != 0) || blend != (((reg(REG_A6XX_RB_BLEND_CNTL) >> t) & 1u) != 0)) {
            fault("the blend of target " + which + text(" is on in some of RB_MRT_CONTROL (%#llx: BLEND and BLEND2), SP_BLEND_CNTL (%#llx) and ", mrtControl,
                                                      reg(REG_A6XX_SP_BLEND_CNTL)) +
                  text("RB_BLEND_CNTL (%#llx) and off in others", reg(REG_A6XX_RB_BLEND_CNTL)));
            return false;
        }
        if (blend) {
            const uint32_t factors[4] = {
                fieldOf(mrtBlend, A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR__MASK, A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR__SHIFT),
                fieldOf(mrtBlend, A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR__MASK, A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR__SHIFT),
                fieldOf(mrtBlend, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR__MASK, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR__SHIFT),
                fieldOf(mrtBlend, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR__MASK, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR__SHIFT)};
            for (uint32_t f : factors) {
                // adreno_rb_blend_factor: 0, 1, 4..16, 20..23 (adreno_common.xml.h:92-110)
                if (f == 2 || f == 3 || (f > 16 && f < 20) || f > 23) {
                    fault("RB_MRT_BLEND_CONTROL[" + which + "] " + text("%#llx names blend factor %llu, which adreno_rb_blend_factor does not have", mrtBlend, f));
                    return false;
                }
            }
            if (fieldOf(mrtBlend, A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE__MASK, A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE__SHIFT) > BLEND_MAX_DST_SRC ||
                fieldOf(mrtBlend, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE__MASK, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE__SHIFT) > BLEND_MAX_DST_SRC) {
                fault("RB_MRT_BLEND_CONTROL[" + which + "] " + text("%#llx names a blend opcode past BLEND_MAX_DST_SRC (4)", mrtBlend));
                return false;
            }
        }
        if (t == 0) {
            d.mrtControl = mrtControl;
            d.mrtBlendControl = mrtBlend;
        }
    }
    // the FS's textures (Gallium's FS_TEX group, fd6_texture.cc:575-687), as the compute dispatch's are judged:
    // SP_FS_TEX_COUNT written in this submission (context state: a stale count would sample a stale table), and when it
    // is not 0 the two tables loaded into SB6_FS_TEX from where SP_FS_TEX_CONST / _SAMP point, as many textures as the
    // count and at least one sampler, SP_FS_CONFIG's NTEX covering the count and equal to NSAMP, every non-zero
    // descriptor a 2D linear view inside a buffer, every sampler with its last word 0. Then the prefetch commands
    // (fd6 emit_fs_inputs): at most four, each a plain sample (TEX_PREFETCH_SAM) of a loaded, non-zero texture with a
    // loaded sampler, writing at least one component into a valid register.
    // The VS's the same (fd6_texture.cc's VS_TEX: SB6_VS_TEX, SP_VS_TEX_*, SP_VS_CONFIG), judged by the one rule.
    auto stageTextures = [&](const char* label, const StageLoads& xs, uint32_t countReg, uint32_t constReg, uint32_t sampReg, uint32_t configReg,
                             uint32_t* count, uint32_t* sampCount, uint64_t* constAt) {
        const std::string L = label, U = L == "FS" ? "FS" : "VS";
        if (written_.count(countReg) == 0) {
            fault("SP_" + U + "_TEX_COUNT not written in this submission: the draw would sample the last owner's texture table");
            return false;
        }
        *count = reg(countReg);
        *sampCount = 0;
        if (*count == 0) return true;
        const uint64_t texTable = reg64(constReg), sampTable = reg64(sampReg);
        *constAt = texTable;
        if (!xs.texLoaded || xs.texFrom != texTable || xs.texUnits != *count) {
            fault("the " + L + "'s texture descriptors were not loaded from where SP_" + U + "_TEX_CONST points, or not as many as SP_" + U + "_TEX_COUNT says");
            return false;
        }
        const uint8_t* texBytes = at(texTable, uint64_t(*count) * 64);
        if (texTable % kSpCsTexConstAlign != 0 || texBytes == nullptr) {
            fault("the " + L + text("'s texture descriptors at %#llx: outside every buffer or not 64-byte aligned", texTable));
            return false;
        }
        if (!xs.sampLoaded || xs.sampFrom != sampTable || xs.sampUnits == 0) {
            fault("the " + L + "'s sampler descriptors were not loaded from where SP_" + U + "_TEX_SAMP points (a texture load needs at least one)");
            return false;
        }
        const uint8_t* sampBytes = at(sampTable, uint64_t(xs.sampUnits) * 16);
        if (sampTable % kSpCsTexSampAlign != 0 || sampBytes == nullptr) {
            fault("the " + L + text("'s sampler descriptors at %#llx: outside every buffer or not 16-byte aligned", sampTable));
            return false;
        }
        const uint32_t config = reg(configReg);
        const uint32_t ntex = fieldOf(config, A6XX_SP_FS_CONFIG_NTEX__MASK, A6XX_SP_FS_CONFIG_NTEX__SHIFT);
        const uint32_t nsamp = fieldOf(config, A6XX_SP_FS_CONFIG_NSAMP__MASK, A6XX_SP_FS_CONFIG_NSAMP__SHIFT);
        if (ntex < *count || ntex != nsamp) {
            fault("SP_" + U + text("_CONFIG names %llu textures and %llu samplers; ", ntex, nsamp) + "SP_" + U +
                  text("_TEX_COUNT says %llu (NTEX must cover it and equal NSAMP)", *count));
            return false;
        }
        for (uint32_t i = 0; i < *count; i++) {
            uint32_t t[kTexConstDwords];
            memcpy(t, texBytes + 64 * i, sizeof t);
            if (allZero(t, kTexConstDwords)) continue;
            const uint32_t ty = fieldOf(t[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT);
            if (ty != A6XX_TEX_2D && ty != A6XX_TEX_3D && ty != A6XX_TEX_1D && ty != A6XX_TEX_CUBE) {
                fault(L + text(" texture %llu is of type %llu: not a 1D, 2D, cube or 3D descriptor this GPU model runs", i, fieldOf(t[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT)));
                return false;
            }
            if (!check2d(t, (L + " texture").c_str(), i, true)) {
                return false;
            }
        }
        for (uint32_t i = 0; i < xs.sampUnits; i++) {
            uint32_t m[kTexSampDwords];
            memcpy(m, sampBytes + 16 * i, sizeof m);
            if (!checkSampler(m, i)) {
                return false;
            }
        }
        *sampCount = xs.sampUnits;
        return true;
    };
    if (!stageTextures("FS", fs_, REG_A6XX_SP_FS_TEX_COUNT, REG_A6XX_SP_FS_TEX_CONST, REG_A6XX_SP_FS_TEX_SAMP, REG_A6XX_SP_FS_CONFIG, &d.fsTextureCount,
                       &d.fsSamplerCount, &d.fsTexConst) ||
        !stageTextures("VS", vs_, REG_A6XX_SP_VS_TEX_COUNT, REG_A6XX_SP_VS_TEX_CONST, REG_A6XX_SP_VS_TEX_SAMP, REG_A6XX_SP_VS_CONFIG, &d.vsTextureCount,
                       &d.vsSamplerCount, &d.vsTexConst)) {
        return false;
    }
    // in a tile, no texture over a target's memory: what the draws before it wrote is in GMEM, and memory holds the
    // target as it was before the pass (a framebuffer fetch reads GMEM there, TILE6_2)
    for (uint32_t i = 0; inTile_ && i < d.fsTextureCount; i++) {
        const uint32_t* td = reinterpret_cast<const uint32_t*>(at(d.fsTexConst + 64ull * i, 64));
        if (td == nullptr || fieldOf(td[0], A6XX_TEX_CONST_0_TILE_MODE__MASK, A6XX_TEX_CONST_0_TILE_MODE__SHIFT) == TILE6_2) continue;
        const uint64_t base = td[4] | uint64_t(fieldOf(td[5], A6XX_TEX_CONST_5_BASE_HI__MASK, A6XX_TEX_CONST_5_BASE_HI__SHIFT)) << 32;
        for (uint32_t t = 0; t < d.mrtCount; t++) {
            if (base == d.mrtBases[t]) {
                fault(text("FS texture %llu in a tile reads target %llu's memory, which holds the target as it was before the pass", i, t));
                return false;
            }
        }
    }
    {
        const uint32_t cntl = reg(REG_A6XX_SP_FS_PREFETCH_CNTL);
        d.prefetchCount = fieldOf(cntl, A6XX_SP_FS_PREFETCH_CNTL_COUNT__MASK, A6XX_SP_FS_PREFETCH_CNTL_COUNT__SHIFT);
        if (d.prefetchCount > 4) {
            fault(text("SP_FS_PREFETCH_CNTL counts %llu prefetches: ir3 makes at most 4", d.prefetchCount));
            return false;
        }
        for (uint32_t i = 0; i < d.prefetchCount; i++) {
            const uint32_t cmd = reg(REG_A6XX_SP_FS_PREFETCH_CMD + i);
            d.prefetchCmd[i] = cmd;
            if (written_.count(REG_A6XX_SP_FS_PREFETCH_CMD + i) == 0) {
                fault(text("SP_FS_PREFETCH_CMD[%llu] not written in this submission: the prefetch would be the last owner's", i));
                return false;
            }
            const uint32_t texId = fieldOf(cmd, A6XX_SP_FS_PREFETCH_CMD_TEX_ID__MASK, A6XX_SP_FS_PREFETCH_CMD_TEX_ID__SHIFT);
            const uint32_t sampId = fieldOf(cmd, A6XX_SP_FS_PREFETCH_CMD_SAMP_ID__MASK, A6XX_SP_FS_PREFETCH_CMD_SAMP_ID__SHIFT);
            const uint32_t dst = fieldOf(cmd, A6XX_SP_FS_PREFETCH_CMD_DST__MASK, A6XX_SP_FS_PREFETCH_CMD_DST__SHIFT);
            const uint32_t wrmask = fieldOf(cmd, A6XX_SP_FS_PREFETCH_CMD_WRMASK__MASK, A6XX_SP_FS_PREFETCH_CMD_WRMASK__SHIFT);
            if (fieldOf(cmd, A6XX_SP_FS_PREFETCH_CMD_CMD__MASK, A6XX_SP_FS_PREFETCH_CMD_CMD__SHIFT) != TEX_PREFETCH_SAM ||
                (cmd & A6XX_SP_FS_PREFETCH_CMD_BINDLESS) != 0) {
                fault(text("SP_FS_PREFETCH_CMD[%llu] %#llx is not a plain, non-bindless sample (TEX_PREFETCH_SAM)", i, cmd));
                return false;
            }
            if (texId >= d.fsTextureCount || sampId >= d.fsSamplerCount) {
                fault(text("SP_FS_PREFETCH_CMD[%llu] samples texture %llu", i, texId) +
                      text(" with sampler %llu: past the loaded tables (%llu textures, ", sampId, d.fsTextureCount) + text("%llu samplers)", d.fsSamplerCount));
                return false;
            }
            uint32_t t[kTexConstDwords];
            memcpy(t, at(d.fsTexConst + 64 * uint64_t(texId), 64), sizeof t);   // inside: the table was checked whole above
            if (allZero(t, kTexConstDwords)) {
                fault(text("SP_FS_PREFETCH_CMD[%llu] samples texture %llu, whose descriptor is zero (no texture is bound there)", i, texId));
                return false;
            }
            if (wrmask == 0 || dst >= kRegidNone) {
                fault(text("SP_FS_PREFETCH_CMD[%llu] writes components %#llx into register %#llx: nothing, or no register", i, wrmask, dst));
                return false;
            }
        }
    }
    // the UBO descriptors each stage loaded: every one names memory inside a buffer for the size it says (an ldc past
    // a descriptor's size reads zeros; past the memory it would read whatever is there)
    for (const StageLoads* xs : {&vs_, &fs_}) {
        for (size_t u = 0; u < xs->ubos.size(); u++) {
            const UboDescriptor& ub = xs->ubos[u];
            if (ub.base == 0 && ub.vec4s == 0) continue;
            if (ub.vec4s == 0 || at(ub.base, uint64_t(ub.vec4s) * 16) == nullptr || ub.base % 4 != 0) {
                fault(text("UBO descriptor %llu of the ", u) + (xs == &vs_ ? "vertex" : "fragment") +
                      text(" stage: %llu vec4 at %#llx, empty, misaligned or outside every buffer", ub.vec4s, ub.base));
                return false;
            }
        }
    }
    // the 3D IBO table: SP_IBO_COUNT descriptors loaded from where SP_IBO points, each judged as a dispatch's are; neither
    // stage naming more IBOs in SP_xS_CONFIG.NIBO than the table holds
    d.gfxIbos = reg(REG_A6XX_SP_IBO_COUNT);
    for (uint32_t r : {REG_A6XX_SP_VS_CONFIG, REG_A6XX_SP_FS_CONFIG}) {
        if (fieldOf(reg(r), A6XX_SP_CS_CONFIG_NIBO__MASK, A6XX_SP_CS_CONFIG_NIBO__SHIFT) > d.gfxIbos) {
            fault(std::string(r == REG_A6XX_SP_VS_CONFIG ? "SP_VS_CONFIG" : "SP_FS_CONFIG") + text(" names more IBOs than SP_IBO_COUNT's %llu", d.gfxIbos));
            return false;
        }
    }
    if (d.gfxIbos != 0) {
        const uint64_t table = reg64(REG_A6XX_SP_IBO);
        if (!gfxIbosLoaded_ || gfxIbosFrom_ != table || gfxIboUnits_ != d.gfxIbos) {
            fault("the 3D IBO descriptors were not loaded into SB6_IBO from where SP_IBO points, or not as many as SP_IBO_COUNT says");
            return false;
        }
        if (!checkIboDescriptors(table, d.gfxIbos)) {
            return false;
        }
    }
    d.vsUbos = uint32_t(vs_.ubos.size());
    d.fsUbos = uint32_t(fs_.ubos.size());
    // the depth: none, or a DEPTH6_32 buffer laid out as texture.cpp lays a Depth32Float texture (TILE6_3: 4096-aligned,
    // the pitch a multiple of 256 bytes holding the window's width, the window's rows counted to 16 inside a buffer),
    // the format the same in GRAS_SU_DEPTH_BUFFER_INFO, the depth registers written in this submission (context state),
    // the test enable the same in RB_DEPTH_CNTL and GRAS_SU_DEPTH_CNTL, a write only with the test. A draw that writes
    // depth leaves it in the depth CCU until a 4-word PC_CCU_FLUSH_DEPTH_TS. No stencil in either case.
    {
        const uint32_t depthFormat = fieldOf(reg(REG_A6XX_RB_DEPTH_BUFFER_INFO), A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__MASK, A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__SHIFT);
        const uint32_t depthCntl = reg(REG_A6XX_RB_DEPTH_CNTL);
        const bool test = (depthCntl & A6XX_RB_DEPTH_CNTL_Z_TEST_ENABLE) != 0, write = (depthCntl & A6XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE) != 0;
        // stencil: a test only with a separate S8 buffer (RB_STENCIL_INFO SEPARATE_STENCIL, beside a depth buffer), laid out
        // as texture.cpp lays the plane out (4096-aligned, the pitch a multiple of 128 bytes holding the width, the
        // window's rows counted to 32 inside a buffer), the enable the same in RB_STENCIL_CONTROL and GRAS_SU_STENCIL_CNTL;
        // what is written goes into the depth CCU as depth does
        const uint32_t stencilControl = reg(REG_A6XX_RB_STENCIL_CONTROL), stencilInfo = reg(REG_A6XX_RB_STENCIL_INFO);
        const bool stencilTest = (stencilControl & A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE) != 0;
        if (stencilTest != ((reg(REG_A6XX_GRAS_SU_STENCIL_CNTL) & A6XX_GRAS_SU_STENCIL_CNTL_STENCIL_ENABLE) != 0)) {
            fault(text("RB_STENCIL_CONTROL %#llx and GRAS_SU_STENCIL_CNTL %#llx: the stencil enable must agree", stencilControl, reg(REG_A6XX_GRAS_SU_STENCIL_CNTL)));
            return false;
        }
        if (stencilControl != 0 && ((stencilInfo & A6XX_RB_STENCIL_INFO_SEPARATE_STENCIL) == 0 || depthFormat == DEPTH6_NONE)) {
            fault("stencil state with no separate stencil buffer beside a depth buffer: what this GPU model runs is the S8 plane of a Depth32Float_Stencil8 texture");
            return false;
        }
        if ((stencilInfo & A6XX_RB_STENCIL_INFO_SEPARATE_STENCIL) != 0) {
            const uint64_t spitch = uint64_t(reg(REG_A6XX_RB_STENCIL_INFO + 1) & 0xfff) << 6, sbase = reg64(REG_A6XX_RB_STENCIL_INFO + 3);
            const uint32_t br = reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR);
            const uint64_t width = (br & 0x3fff) + 1, rows = ((br >> 16 & 0x3fff) + 1 + 31) / 32 * 32;
            if (sbase % 4096 != 0 || spitch % 128 != 0 || spitch < width || at(sbase, rows * spitch) == nullptr) {
                fault(text("the stencil buffer at %#llx with a pitch of %llu bytes, ", sbase, spitch) +
                      text("%llu rows: not 4096-aligned, not a multiple of 128, narrower than the window, or outside every buffer", rows));
                return false;
            }
            if (stencilTest) drawsWithoutDepthFlush_++;
        }
        if (depthFormat == DEPTH6_NONE) {
            if (test || write) {
                fault("a depth test or write with no depth buffer (RB_DEPTH_BUFFER_INFO DEPTH6_NONE)");
                return false;
            }
        } else {
            if (depthFormat != DEPTH6_32 || fieldOf(reg(REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO), A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__MASK, 0) != depthFormat) {
                fault(text("depth format %llu in RB_DEPTH_BUFFER_INFO and %llu in GRAS_SU_DEPTH_BUFFER_INFO: DEPTH6_32 in both is what this GPU model runs", depthFormat,
                           fieldOf(reg(REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO), A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__MASK, 0)));
                return false;
            }
            for (uint32_t r : {REG_A6XX_RB_DEPTH_BUFFER_INFO, REG_A6XX_RB_DEPTH_BUFFER_PITCH, REG_A6XX_RB_DEPTH_BUFFER_BASE, REG_A6XX_RB_DEPTH_CNTL, REG_A6XX_GRAS_SU_DEPTH_CNTL}) {
                if (written_.count(r) == 0) {
                    fault(text("depth register %#llx not written in this submission: the draw would test against the last owner's depth", r));
                    return false;
                }
            }
            const uint64_t pitch = uint64_t(fieldOf(reg(REG_A6XX_RB_DEPTH_BUFFER_PITCH), A6XX_RB_DEPTH_BUFFER_PITCH__MASK, A6XX_RB_DEPTH_BUFFER_PITCH__SHIFT))
                                   << A6XX_RB_DEPTH_BUFFER_PITCH__SCALE;
            const uint64_t base = reg64(REG_A6XX_RB_DEPTH_BUFFER_BASE);
            const uint32_t br = reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR);
            const uint64_t width = (br & 0x3fff) + 1, rows = ((br >> 16 & 0x3fff) + 1 + 15) / 16 * 16;
            // (a multisample one holds the draw's sample count of 4-byte depths a texel: fdl6's cpp 4 * samples, its pitch a
            // multiple of 64 such texels)
            if (base % 4096 != 0 || pitch % (256 * d.samples) != 0 || pitch < width * 4 * d.samples || at(base, rows * pitch) == nullptr) {
                fault(text("the depth buffer at %#llx with a pitch of %llu bytes, ", base, pitch) + text("%llu rows, ", rows) +
                      text("%llu sample(s): not 4096-aligned, not a multiple of 256 a sample, narrower than the window, or outside every buffer", d.samples));
                return false;
            }
            if (test != ((reg(REG_A6XX_GRAS_SU_DEPTH_CNTL) & A6XX_GRAS_SU_DEPTH_CNTL_Z_TEST_ENABLE) != 0) || (write && !test)) {
                fault(text("RB_DEPTH_CNTL %#llx and GRAS_SU_DEPTH_CNTL %#llx: the test enable must agree, and a write needs the test", depthCntl,
                           reg(REG_A6XX_GRAS_SU_DEPTH_CNTL)));
                return false;
            }
            if (write) drawsWithoutDepthFlush_++;
            d.depthBase = base;
        }
        d.depthCntl = depthCntl;
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
        // (in a tile the window scissor is the tile's and the draw's scissors are the target's, as on any pass: the
        // window clips them, which is how a draw visits every tile)
        if (sc[0] > sc[2] || sc[1] > sc[3] || (!inTile_ && (sc[2] > d.windowScissor[2] || sc[3] > d.windowScissor[3]))) {
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
    // the vertex fetch: equal counts (turnip writes both as the attribute count, tu_shader.cc:1592-1595)
    uint32_t vfd0 = reg(REG_A6XX_VFD_CONTROL_0);
    d.vfdFetchCount = fieldOf(vfd0, A6XX_VFD_CONTROL_0_FETCH_CNT__MASK, A6XX_VFD_CONTROL_0_FETCH_CNT__SHIFT);
    d.vfdDecodeCount = fieldOf(vfd0, A6XX_VFD_CONTROL_0_DECODE_CNT__MASK, A6XX_VFD_CONTROL_0_DECODE_CNT__SHIFT);
    if (d.vfdFetchCount != d.vfdDecodeCount) {
        fault(text("VFD_CONTROL_0 counts %llu fetches and %llu decodes", d.vfdFetchCount, d.vfdDecodeCount));
        return false;
    }
    // Decode entry n is [[attribute(n)]]; its IDX names the fetch entry (the buffer) it reads, as turnip programs them
    // (tu6_emit_vertex_input, tu_pipeline.cc:2330-2360). A decode with a destination must have a known format, its fetch a
    // size inside a buffer, and the draw's last vertex's element inside that size: past it the GPU reads what follows.
    for (uint32_t i = 0; i < d.vfdDecodeCount; i++) {
        uint32_t dest = reg(regVfdDestCntlInstr(i));
        if (fieldOf(dest, A6XX_VFD_DEST_CNTL_INSTR_REGID__MASK, A6XX_VFD_DEST_CNTL_INSTR_REGID__SHIFT) == kRegidNone) {
            continue;   // a hole: no register written
        }
        if (regs_.find(regVfdDecodeInstr(i)) == regs_.end()) {
            fault(text("vertex attribute %llu has a destination register and no VFD_DECODE_INSTR", i));
            return false;
        }
        uint32_t dec = reg(regVfdDecodeInstr(i));
        uint32_t b = fieldOf(dec, A6XX_VFD_DECODE_INSTR_IDX__MASK, A6XX_VFD_DECODE_INSTR_IDX__SHIFT);
        uint32_t offset = fieldOf(dec, A6XX_VFD_DECODE_INSTR_OFFSET__MASK, A6XX_VFD_DECODE_INSTR_OFFSET__SHIFT);
        uint32_t elem = vertexElementBytes(fieldOf(dec, A6XX_VFD_DECODE_INSTR_FORMAT__MASK, A6XX_VFD_DECODE_INSTR_FORMAT__SHIFT));
        uint64_t base = reg64(regVfdFetchBase(b));
        uint32_t size = reg(vfdFetchSize(b));
        uint32_t stride = reg(regVfdFetchStride(b));
        // the last element fetched: per instance (INSTANCED) the last instance over the step rate; per vertex the last
        // vertex, which in an indexed draw is the highest index read above
        const bool instanced = (dec & A6XX_VFD_DECODE_INSTR_INSTANCED) != 0;
        const uint32_t stepRate = reg(regVfdDecodeStepRate(i));
        if (instanced && stepRate == 0) {
            fault(text("vertex attribute %llu is fetched per instance with a step rate of 0", i));
            return false;
        }
        uint64_t last = instanced ? uint64_t(d.numInstances - 1) / stepRate
                                  : uint64_t(reg(REG_A6XX_VFD_INDEX_OFFSET)) + (d.indexed ? highestIndex : uint64_t(d.numIndices - 1));
        if (elem == 0) {
            fault(text("vertex attribute %llu: VFD_DECODE_INSTR.FORMAT is not a vertex format this model knows", i));
            return false;
        }
        if (size == 0 || at(base, size) == nullptr) {
            fault(text("vertex attribute %llu: fetch %llu is %llu bytes", i, b, size) + text(" at %#llx, outside every buffer", base, 0, 0));
            return false;
        }
        if (last * stride + offset + elem > size) {
            fault(text("vertex attribute %llu: vertex %llu's element ends at byte %llu", i, last, last * stride + offset + elem) +
                  text(" of fetch %llu, whose size is %llu", b, size, 0));
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
    // RB_CCU_CNTL: written in this submission, right after a WFI, with the sysmem shape (colour cache after the depth cache at 0)
    {
        uint32_t ccu = reg(REG_A6XX_RB_CCU_CNTL);
        bool sysmem = fieldOf(ccu, A6XX_RB_CCU_CNTL_COLOR_OFFSET__MASK, A6XX_RB_CCU_CNTL_COLOR_OFFSET__SHIFT) != 0 &&
                      fieldOf(ccu, A6XX_RB_CCU_CNTL_DEPTH_OFFSET__MASK, A6XX_RB_CCU_CNTL_DEPTH_OFFSET__SHIFT) == 0;
        if (inTile_) {
            // the colour cache above every target's bin, and the bins of the targets the stage writes apart
            const uint32_t at = fieldOf(ccu, A6XX_RB_CCU_CNTL_COLOR_OFFSET__MASK, A6XX_RB_CCU_CNTL_COLOR_OFFSET__SHIFT) << A6XX_RB_CCU_CNTL_COLOR_OFFSET__SCALE |
                                fieldOf(ccu, A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI__MASK, A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI__SHIFT) << 21;
            if (!ccuCntlWritten_ || !ccuCntlAfterWfi_) {
                fault(text("RB_CCU_CNTL (%#llx) in a tile: not written after a WFI", ccu));
                return false;
            }
            for (uint32_t t = 0; t < d.mrtCount; t++) {
                if (gmemSpans[t][1] > at) {
                    fault(text("RB_MRT_BASE_GMEM[%llu]: a bin to %#llx, ", t, gmemSpans[t][1]) + text("into the CCU's colour cache at %#llx", at));
                    return false;
                }
                for (uint32_t u = 0; u < t; u++) {
                    if (((unwrittenMask >> t) & 1) == 0 && ((unwrittenMask >> u) & 1) == 0 && gmemSpans[t][0] < gmemSpans[u][1] && gmemSpans[u][0] < gmemSpans[t][1]) {
                        fault(text("RB_MRT_BASE_GMEM[%llu] and [%llu]: the two written targets' bins overlap", t, u));
                        return false;
                    }
                }
            }
        } else if (!ccuCntlWritten_ || !ccuCntlAfterWfi_ || !sysmem) {
            fault(text("RB_CCU_CNTL (%#llx) was not written in this submission right after a CP_WAIT_FOR_IDLE with the sysmem shape", ccu));
            return false;
        }
    }
    stats_.draws++;
    drawsInSubmission_++;
    drawsWithoutCcuFlush_++;
    drawsWithoutCacheFlush_++;
    samples_ += uint64_t(d.numIndices) * d.numInstances;
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
                written_.insert(h.reg + j);
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
                    inTile_ = false;
                    stats_.passes++;
                }
                // the binning pass: within a pass begun, before its tiles
                if (fieldOf(p[0], A6XX_CP_SET_MARKER_0_MODE__MASK, A6XX_CP_SET_MARKER_0_MODE__SHIFT) == RM6_BIN_VISIBILITY) {
                    if (!passBegun_ || inTile_) {
                        fault("RM6_BIN_VISIBILITY outside a pass or inside a tile");
                        return false;
                    }
                    binning_ = true;
                    stats_.binningPasses++;
                }
                // a GMEM tile: within a pass begun (the head's marker), ended by RM6_BIN_RENDER_END before the next
                if (fieldOf(p[0], A6XX_CP_SET_MARKER_0_MODE__MASK, A6XX_CP_SET_MARKER_0_MODE__SHIFT) == RM6_BIN_RENDER_START) {
                    if (!passBegun_ || inTile_ || (p[0] & A6XX_CP_SET_MARKER_0_USES_GMEM) == 0) {
                        fault("RM6_BIN_RENDER_START outside a pass, inside a tile, or without USES_GMEM");
                        return false;
                    }
                    inTile_ = true;
                    binning_ = false;
                    stats_.tiles++;
                    // (GMEM holds whatever the last tile left; a pattern no target here holds, so a store without its load shows)
                    std::fill(gmem_.begin(), gmem_.end(), uint8_t(0xcd));
                }
                if (fieldOf(p[0], A6XX_CP_SET_MARKER_0_MODE__MASK, A6XX_CP_SET_MARKER_0_MODE__SHIFT) == RM6_BIN_RENDER_END) {
                    if (!inTile_) {
                        fault("RM6_BIN_RENDER_END outside a tile");
                        return false;
                    }
                    inTile_ = false;
                }
                break;
            case CP_SET_BIN_DATA5_OFFSET: {
                // a tile's streams, at offsets from VSC_DRAW_STRM_ADDRESS, VSC_DRAW_STRM_SIZE_ADDRESS and VSC_PRIM_STRM_ADDRESS
                if (h.count != 4 || !inTile_ || at(reg64(REG_A6XX_VSC_DRAW_STRM_ADDRESS) + p[1], 4) == nullptr ||
                    at(reg64(REG_A6XX_VSC_DRAW_STRM_SIZE_ADDRESS) + p[2], 4) == nullptr || at(reg64(REG_A6XX_VSC_PRIM_STRM_ADDRESS) + p[3], 4) == nullptr) {
                    fault("CP_SET_BIN_DATA5_OFFSET not of 4 words, outside a tile, or its streams outside every buffer");
                    return false;
                }
                stats_.binData++;
                break;
            }
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
            case CP_DRAW_INDIRECT_MULTI: {
                // one draw (DRAW_COUNT 1), its words read from memory as the packet runs (tu_CmdDrawIndirect's layout:
                // Vulkan's draw commands, Metal's indirect arguments): the CP sets VFD_INDEX_OFFSET and
                // VFD_INSTANCE_START_OFFSET from them and the draw goes on as CP_DRAW_INDX_OFFSET's would, checked the same
                const uint32_t op = p[1] & 0xfu;
                const bool indexed = op == INDIRECT_OP_INDEXED;
                if ((op != INDIRECT_OP_NORMAL && !indexed) || h.count != (indexed ? 9u : 6u) || p[2] != 1) {
                    fault(text("CP_DRAW_INDIRECT_MULTI of %llu words, opcode %llu, draw count %llu: one draw, NORMAL in 6 words or INDEXED in 9", h.count, op, p[2]));
                    return false;
                }
                const uint64_t a = indexed ? (p[6] | uint64_t(p[7]) << 32) : (p[3] | uint64_t(p[4]) << 32);
                const uint8_t* m = at(a, indexed ? 20 : 16);
                if (m == nullptr || a % 4 != 0) {
                    fault(text("CP_DRAW_INDIRECT_MULTI's arguments at %#llx: outside every buffer or not 4-aligned", a));
                    return false;
                }
                uint32_t w[5] = {};
                memcpy(w, m, indexed ? 20 : 16);
                if (indexed) {
                    // indexCount, instanceCount, firstIndex, vertexOffset, firstInstance
                    const uint32_t size = fieldOf(p[0], CP_DRAW_INDX_OFFSET_0_INDEX_SIZE__MASK, CP_DRAW_INDX_OFFSET_0_INDEX_SIZE__SHIFT) == INDEX4_SIZE_32_BIT ? 4 : 2;
                    const uint64_t base = (p[3] | uint64_t(p[4]) << 32) + uint64_t(w[2]) * size;
                    regs_[REG_A6XX_VFD_INDEX_OFFSET] = w[3];
                    regs_[REG_A6XX_VFD_INSTANCE_START_OFFSET] = w[4];
                    uint32_t d[7] = {p[0], w[1], w[0], 0, uint32_t(base), uint32_t(base >> 32), w[2] <= p[5] ? p[5] - w[2] : 0};
                    if (!execDraw(d, 7)) return false;
                } else {
                    // vertexCount, instanceCount, firstVertex, firstInstance
                    regs_[REG_A6XX_VFD_INDEX_OFFSET] = w[2];
                    regs_[REG_A6XX_VFD_INSTANCE_START_OFFSET] = w[3];
                    uint32_t d[3] = {p[0], w[1], w[0]};
                    if (!execDraw(d, 3)) return false;
                }
                break;
            }
            case CP_DRAW_INDX_OFFSET:
                if (!execDraw(p, h.count)) {
                    return false;
                }
                break;
            case CP_BLIT:
                if (!execBlit(p, h.count)) {
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
                // ZPASS_DONE: the running sample count, 64 bits, where RB_SAMPLE_COUNT_ADDR points, when
                // RB_SAMPLE_COUNT_CONTROL.COPY asks for it (turnip's occlusion query, A6XX path)
                if (h.count == 1 && fieldOf(p[0], CP_EVENT_WRITE_0_EVENT__MASK, CP_EVENT_WRITE_0_EVENT__SHIFT) == BLIT && !eventBlit()) {
                    return false;
                }
                if (h.count == 1 && fieldOf(p[0], CP_EVENT_WRITE_0_EVENT__MASK, CP_EVENT_WRITE_0_EVENT__SHIFT) == ZPASS_DONE) {
                    if ((reg(REG_A6XX_RB_SAMPLE_COUNT_CONTROL) & A6XX_RB_SAMPLE_COUNT_CONTROL_COPY) == 0) {
                        fault("ZPASS_DONE without RB_SAMPLE_COUNT_CONTROL.COPY: no count is written");
                        return false;
                    }
                    const uint64_t to = reg64(REG_A6XX_RB_SAMPLE_COUNT_ADDR);
                    uint8_t* d = at(to, 8);
                    if (d == nullptr || to % 8 != 0) {
                        fault(text("ZPASS_DONE's sample count at %#llx: outside every buffer or not 8-aligned", to));
                        return false;
                    }
                    memcpy(d, &samples_, 8);
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
                    if (ev == PC_CCU_FLUSH_DEPTH_TS) drawsWithoutDepthFlush_ = 0;
                    if (ev == CACHE_FLUSH_TS) drawsWithoutCacheFlush_ = 0;
                }
                break;
            }
            case CP_MEM_TO_MEM: {
                // the five-word form with no flags: one 32-bit word from SRC to DST (an indirect dispatch's counts
                // into its table, as turnip copies them, tu_cmd_buffer.cc:6723-6728)
                if (h.count == 5 && p[0] == 0) {
                    const uint64_t dst = p[1] | uint64_t(p[2]) << 32, src = p[3] | uint64_t(p[4]) << 32;
                    const uint8_t* from = at(src, 4);
                    uint8_t* to = at(dst, 4);
                    if (from == nullptr || to == nullptr || src % 4 != 0 || dst % 4 != 0) {
                        fault(text("CP_MEM_TO_MEM of a word from %#llx to %#llx: outside every buffer or not 4-aligned", src, dst));
                        return false;
                    }
                    memmove(to, from, 4);
                    break;
                }
                // DST = A + B + C over 64-bit values, each of A, B, C negated by its NEG flag (an occlusion query's result
                // += end - begin); 32-bit sums are not what this driver emits, and are refused
                if (h.count != 9 || (p[0] & CP_MEM_TO_MEM_0_DOUBLE) == 0) {
                    fault(text("CP_MEM_TO_MEM of %llu words or without DOUBLE: the 64-bit three-source form is what this model runs", h.count));
                    return false;
                }
                uint64_t addr[4], v[3];
                for (int i = 0; i < 4; i++) addr[i] = p[1 + 2 * i] | uint64_t(p[2 + 2 * i]) << 32;
                for (int i = 0; i < 3; i++) {
                    const uint8_t* m = at(addr[1 + i], 8);
                    if (m == nullptr || addr[1 + i] % 8 != 0) {
                        fault(text("CP_MEM_TO_MEM's source at %#llx: outside every buffer or not 8-aligned", addr[1 + i]));
                        return false;
                    }
                    memcpy(&v[i], m, 8);
                    if ((p[0] >> i) & 1) v[i] = uint64_t(0) - v[i];   // NEG_A, NEG_B, NEG_C
                }
                uint8_t* out = at(addr[0], 8);
                if (out == nullptr || addr[0] % 8 != 0) {
                    fault(text("CP_MEM_TO_MEM's destination at %#llx: outside every buffer or not 8-aligned", addr[0]));
                    return false;
                }
                const uint64_t sum = v[0] + v[1] + v[2];
                memcpy(out, &sum, 8);
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
            case CP_REG_TO_MEM: {
                // what a profiled command buffer stamps: CP_ALWAYS_ON_COUNTER as one 64-bit value (CNT 2, 64B); the fake
                // writes a count of the stamps it has carried out, so that the order of two is the order they ran in
                if (h.count != 3) {
                    fault(text("CP_REG_TO_MEM of %llu words, not 3", h.count));
                    return false;
                }
                const uint32_t reg = p[0] & CP_REG_TO_MEM_0_REG__MASK;
                const uint64_t to = p[1] | uint64_t(p[2]) << 32;
                uint8_t* d = at(to, 8);
                if (reg != 0x0980 || !(p[0] & CP_REG_TO_MEM_0_64B) || d == nullptr || to % 8 != 0) {
                    fault(text("CP_REG_TO_MEM of register %#x to %#llx: the fake serves CP_ALWAYS_ON_COUNTER, 64-bit, into a buffer", reg, to));
                    return false;
                }
                const uint64_t now = ++alwaysOn_;
                memcpy(d, &now, 8);
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
            case CP_EXEC_CS_INDIRECT: {
                // the counts are read from memory as the packet runs, what is there then (written by the CPU or by an
                // earlier dispatch); the local size in word 3 must be NDRANGE_0's, which the dispatch also ran with
                if (h.count != 4) {
                    fault(text("CP_EXEC_CS_INDIRECT of %llu words, not 4", h.count));
                    return false;
                }
                const uint64_t a = p[1] | uint64_t(p[2]) << 32;
                const uint8_t* m = at(a, 12);
                if (m == nullptr || a % 4 != 0) {
                    fault(text("CP_EXEC_CS_INDIRECT's counts at %#llx: outside every buffer or not 4-aligned", a));
                    return false;
                }
                const uint32_t nd0 = reg(REG_A6XX_HLSQ_CS_NDRANGE_0);
                const uint32_t want = execCsIndirect3(fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__MASK, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__SHIFT) + 1,
                                                      fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__MASK, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__SHIFT) + 1,
                                                      fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__MASK, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__SHIFT) + 1);
                if (p[3] != want) {
                    fault(text("CP_EXEC_CS_INDIRECT's local size word %#llx is not HLSQ_CS_NDRANGE_0's (%#llx)", p[3], want));
                    return false;
                }
                uint32_t w[4] = {0, 0, 0, 0};
                memcpy(w + 1, m, 12);
                if (!execCs(w, 4, true)) {
                    return false;
                }
                break;
            }
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
    gfxIbosLoaded_ = false;
    gfxIbosFrom_ = 0;
    gfxIboUnits_ = 0;
    passBegun_ = ccuCntlWritten_ = ccuCntlAfterWfi_ = lastWasWfi_ = inTile_ = binning_ = false;
    drawsInSubmission_ = drawsWithoutCcuFlush_ = drawsWithoutCacheFlush_ = drawsWithoutDepthFlush_ = 0;
    written_.clear();
    bool ok = execute(job.address, job.words, 0);
    if (ok && drawsInSubmission_ != 0) {
        // the colour a draw wrote sits in the CCU until PC_CCU_FLUSH_COLOR_TS (the kernel's own end-of-submit fence
        // flushes the UCHE, not the CCU: hakios.adreno.c:3496-3516), and what the CCU flushed sits in the UCHE until
        // CACHE_FLUSH_TS: a stream that draws and ends without both would complete with the target unchanged
        if (drawsWithoutCcuFlush_ != 0) {
            fault("the colour stays in the CCU: no PC_CCU_FLUSH_COLOR_TS (4-word) after the draw");
            ok = false;
        } else if (drawsWithoutDepthFlush_ != 0) {
            fault("the depth stays in the CCU: no PC_CCU_FLUSH_DEPTH_TS (4-word) after a draw that writes depth");
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
        const uint32_t handle = static_cast<DrmGemClose*>(data)->handle;
        auto sb = bufferSurface_.find(handle);
        if (sb != bufferSurface_.end()) {
            // the surface keeps its pixels: the buffer was a view of them
            Surface& s = surfaces_.at(sb->second);
            s.bytes = std::move(buffers_.at(handle).bytes);
            s.handle = 0;
            bufferSurface_.erase(sb);
        }
        return buffers_.erase(handle) ? 0 : ENOENT;
    }
    case kHakiosSurface: {
        if (size != sizeof(HakiosSurface)) return EINVAL;
        auto* r = static_cast<HakiosSurface*>(data);
        auto it = surfaces_.find(r->surface);
        if (it == surfaces_.end()) return ENOENT;          // kIOReturnNotFound
        Surface& s = it->second;
        if (s.client) return ENOTSUP;                      // kIOReturnUnsupported: memory of a program's own
        r->size = (uint64_t(s.offset) + uint64_t(s.rowBytes) * s.height + (s.planes == 2 ? uint64_t(s.rowBytes) * ((s.height + 1) / 2) : 0) + 0xfff) &
                  ~0xfffull;
        r->offset = s.offset;
        r->rowBytes = s.rowBytes;
        r->width = s.width;
        r->height = s.height;
        r->pixelFormat = s.pixelFormat;
        r->planes = s.planes;
        if (r->handle == kHakiosDescribe) {
            r->handle = 0;
            return 0;
        }
        if (s.handle != 0) {
            r->handle = s.handle;                          // the same buffer again, as the kernel answers it
            return 0;
        }
        s.bytes.resize(r->size);
        Buffer b{nextIova_, std::move(s.bytes)};
        nextIova_ += (r->size + 0xffff) & ~0xffffull;
        r->handle = s.handle = nextHandle_++;
        buffers_.emplace(r->handle, std::move(b));
        bufferSurface_[r->handle] = r->surface;
        return 0;
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
