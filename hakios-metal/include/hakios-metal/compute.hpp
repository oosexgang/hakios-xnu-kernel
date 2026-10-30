// The compute dispatch: one Kernel (kernel.hpp) run over a grid of work groups, as Mesa's computerator runs one on an
// A6XX without Gallium (src/freedreno/computerator/a6xx.cc), register by register from a6xx.hpp, plus what Gallium's
// a6xx/fd6_compute.cc adds for a kernel the AIR front end compiled: the driver params it reads its threadgroup size
// from, and the constant-buffer ranges ir3 pushed into the constant file.
//
// What one dispatch is made of:
//   - the program: the kernel's words in a buffer of their own, named by SP_CS_OBJ_START and preloaded with
//     CP_LOAD_STATE6_FRAG (ST6_SHADER), at most 127 units of it: 128 hangs this generation (instr_cache_size);
//   - the constants: the kernel's immediates (direct), the driver params (direct), the pushed UBO ranges (indirect,
//     straight out of the bound buffer), each a CP_LOAD_STATE6_FRAG (ST6_CONSTANTS) into the constant file, every one
//     within the kernel's constlen or the dispatch is refused;
//   - the buffers: one 16-dword descriptor per IBO slot (a6xx::bufferIbo) in a table the dispatch owns, loaded with
//     ST6_IBO and named by SP_CS_IBO. A Metal [[buffer(i)]] is slot i for a device buffer (an IBO) and block i for a
//     constant buffer (a UBO range), so one list of bindings by index serves both; a text kernel's @buf i is slot i;
//   - the grid: HLSQ_CS_NDRANGE_*, and CP_EXEC_CS with the group counts;
//   - the end: the caches flushed and the flush waited for, as computerator's cache_flush does, so that what the kernel
//     wrote is in memory when the fence passes.
// Before the program a CACHE_INVALIDATE event, so that the kernel reads what the CPU or a blit wrote last and not what
// the GPU's cache holds (Gallium's barrier; computerator has none, and here a blit may precede a dispatch).
//
// What the fake GPU checks of all this, and what it does not: fake_gpu.hpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hakios-metal/kernel.hpp"
#include "hakios-metal/pm4.hpp"

namespace hakios::metal {

class Buffer;
class Texture;
struct Sampler;

// a buffer bound at an index: Metal's [[buffer(i)]], a text kernel's @buf i
struct Binding {
    const Buffer* buffer = nullptr;
    uint64_t offset = 0;      // bytes into it; a multiple of 4 (the descriptors address elements)
};

// Everything a dispatch binds, each by Metal's own index: buffers[i] is [[buffer(i)]], textures[n] is [[texture(n)]]
// (a sampled texture or an image, whichever the kernel declared at n: Kernel::textures[n].kind decides, so the plug-in's
// -setTexture:atIndex: stores by index and never looks at the kernel), samplers[m] is [[sampler(m)]]. A nullptr entry
// is nothing bound there; an entry past what the kernel declares is ignored, as Metal ignores it.
// Private memory, the scratch a stage spills to and keeps its dynamically indexed arrays in (Kernel::pvtmemSize a
// fiber): one buffer of the queue's (CommandQueue::privateMemory) and its layout, address 0 when none is bound.
struct PrivateMemory {
    uint64_t address = 0;
    uint32_t perFiber = 0;   // bytes a fiber, a power of two of 512 or more (SP_xS_PVT_MEM_PARAM counts them >> 9)
    uint32_t perSp = 0;      // bytes an SP core, perFiber times its fibers on a 4 KiB boundary: the stride between cores
    bool perWave = false;    // ir3's pvtmem_per_wave: the layout PERWAVEMEMLAYOUT names
};

struct Bindings {
    std::vector<Binding> buffers;
    std::vector<const Texture*> textures;
    std::vector<const Sampler*> samplers;
    PrivateMemory privateMemory;
    // a dispatch's threadgroup memory in bytes when more than the kernel's own (Kernel::sharedSize): the encoder's
    // setThreadgroupMemoryLength: past the static arrays (CompiledKernel::threadgroupArgBase); 0 is the kernel's own
    uint32_t sharedBytes = 0;
    // texture slots holding the queue's null texture because the application bound nothing there (a draw's own doing,
    // CommandBuffer::draw): sampled as the null slot is, in whatever dimension and class the shader declares
    uint32_t nullMask = 0;
};

namespace compute {

constexpr uint32_t kMaxLocalThreads = 1024;     // the hardware's threadgroup limit, and the NDRANGE fields' (10 bits)
constexpr uint32_t kMaxSharedBytes = 32 * 1024; // threadgroup memory a group can have (freedreno_devices.py: the A635's cs_shared_mem_size)
// What a dispatch owns in GPU memory besides the program, one 16 KiB page on the phone whatever the size (gpu.cpp rounds
// to it): the IBO descriptors (64 bytes each; NIBO is 7 bits and SSBO slots go up to Metal's 31 buffers plus 16 images),
// the texture table (TEX_CONST, 64 bytes each, 64-aligned: SP_CS_TEX_CONST's alignment), the sampler table (TEX_SAMP,
// 16 bytes each, 16-aligned), a border colour entry of zeros (128 bytes, 128-aligned: SP_TP_BORDER_COLOR_BASE_ADDR's
// entry 0, what a CLAMP_TO_BORDER sampler reads for Metal's clamp_to_zero), a hole descriptor (see below), and the two
// end-of-dispatch timestamp words.
constexpr uint32_t kTableBytes = 8192;
constexpr uint32_t kTableIbosAt = 0;
constexpr uint32_t kMaxIbos = 64;
constexpr uint32_t kTableTexConstAt = 4096;     // kMaxTextureSlots * 64 = 1024 bytes
constexpr uint32_t kTableTexSampAt = 5120;      // kMaxTextureSlots * 16 = 256 bytes
constexpr uint32_t kTableBorderColorAt = 6144;  // Metal's three border colours (texture.hpp: writeBorderColors)
constexpr uint32_t kTableHoleAt = 5504;         // 64 bytes nobody addresses: what the hole descriptors point at
constexpr uint32_t kTableTimestampsAt = 5632;   // the two timestamp words the cache flush writes and waits for
// an indirect dispatch's group counts, copied in by the CP from the caller's buffer (which Metal aligns to 4 bytes,
// and CP_LOAD_STATE6 reads from 16): two vec4s, one under the kernel's own count slot, one under the driver params,
// each with the word its fourth lane would otherwise overwrite
constexpr uint32_t kTableIndirectAt = 5648;
static_assert(kTableTexConstAt % 64 == 0 && kTableTexSampAt % 16 == 0 && kTableBorderColorAt % 128 == 0, "the tables' alignments");
static_assert(kTableIbosAt + kMaxIbos * 64 <= kTableTexConstAt && kTableTexConstAt + kMaxTextureSlots * 64 <= kTableTexSampAt &&
              kTableTexSampAt + kMaxTextureSlots * 16 <= kTableHoleAt && kTableIndirectAt + 32 <= kTableBorderColorAt &&
              kTableBorderColorAt + 3 * 128 <= kTableBytes &&
              kTableHoleAt + 64 <= kTableTimestampsAt && kTableTimestampsAt + 16 <= kTableIndirectAt && kTableIndirectAt % 16 == 0 &&
              kTableIndirectAt + 32 <= kTableBytes, "the table's parts fit");
// An IBO slot ir3 counts but the kernel never addresses (a device buffer at [[buffer(5)]] alone leaves slots 0-4; a
// sampled texture at [[texture(0)]] beside an image at [[texture(1)]] leaves IBO slot 0) gets a valid one-element buffer
// descriptor over the table's own kTableHoleAt words rather than zeros: real memory the kernel never names, so a wrong
// slot cannot read as a fault. (Gallium's zero dummy_view is blessed for the TEXTURE table only; no Mesa driver runs a
// bound IBO table with a zero entry. The texture table's unused slots ARE zeros, as Gallium's are.)

// What the dispatch stream is made of, each part switchable. The defaults are the whole sequence; a part switched off
// is for finding, on the phone, which part a hang needs (hakios-metal-probe --bisect runs end_only under each), since
// the fake GPU cannot tell: on 28 October every dispatch, end_only included, left the HLSQ busy and the SP idle.
struct Options {
    bool prelude = true;        // SP_MODE_CONTROL, SP_PERFCTR_ENABLE, SP_FLOAT_CNTL, as computerator opens with
    bool invalidateCmd = true;  // HLSQ_INVALIDATE_CMD before the program
    bool hlsqCopies = true;     // the gen4 (has_lpac) copies: HLSQ_CS_CTRL_REG1, SP_CS_CNTL_0/1
    bool pvtmemZero = true;     // the private-memory registers written as zero (Gallium does; computerator leaves them)
    bool invalidate = true;     // CP_EVENT_WRITE CACHE_INVALIDATE before the dispatch (Gallium's barrier)
    bool marker = true;         // CP_SET_MARKER RM6_COMPUTE
    bool tail = true;           // the end: RB_DONE_TS, a wait for it, CACHE_FLUSH_TS, a wait for it
    bool tailRbDone = true;     // ... the RB_DONE_TS half of it
    bool tailCacheFlush = true; // ... the CACHE_FLUSH_TS half of it
    // CP_EVENT_WRITE_0_TIMESTAMP on the two events. Off: with it the GPU writes a timestamp where the wait expects the
    // value (the XML says "write back timestamp instead of value from packet", and the phone said so on 28 October: every
    // dispatch hung on its own tail, each half of it, until the bit was cleared; computerator leaves it clear, and so does
    // the kernel's own fence write). True is here for the fake GPU's test of that, nothing else.
    bool tailTimestampBit = false;
    // The texture state: the TEX_SAMP and TEX_CONST loads into SB6_CS_TEX, SP_CS_TEX_SAMP/CONST/COUNT (Gallium's
    // fd6_texture.cc:593-687), emitted only for a kernel with numTextureSlots > 0, as Gallium builds the CS_TEX group
    // only for a shader with textures. Off for the bisect: an image-only kernel never has it.
    bool texState = true;
    // SP_TP_MODE_CNTL (the TP's own ISAMMODE copy, beside SP_MODE_CONTROL's that the prelude writes) and the two border
    // colour base registers pointing at the table's zero entry, in the prelude. Gallium writes all three at every batch
    // start (fd6_emit.cc:977-981, 1035-1038); nothing wrote a TP register here before textures.
    bool tpState = true;
    // A cap below a6xx::kInstrCacheSize (Mesa's instr_cache_size for this chip) on the program units CP_LOAD_STATE6
    // preloads; 0 is none. For a probe that has the GPU fetch the rest past the preload of a program that would fit.
    uint32_t preloadCap = 0;
};

// The whole dispatch into STREAM: KERNEL over GROUPS work groups of LOCAL threads each, BUFFERS bound by index,
// IMMEDIATES in place of the kernel's own (kernel.numImmediates dwords; nullptr keeps the kernel's). PROGRAM is the GPU
// address of the kernel's words, TABLE and TABLE_CPU the kTableBytes the dispatch owns, whose descriptors this writes.
// False with *why when the kernel and the bindings do not fit together: then nothing was added to the stream.
bool encode(pm4::Stream& stream, const Kernel& kernel, const uint32_t groups[3], const uint32_t local[3],
            const std::vector<Binding>& buffers, const uint32_t* immediates, uint64_t program, uint64_t table,
            uint8_t* tableCpu, std::string* why, const Options& options = Options{});
// The same with textures and samplers bound too (the form above is this with none). Refused by name, with nothing
// added to the stream: a declared texture or sampler index with nothing bound; a texture bound to an index the kernel
// addresses as another dimension or element class than the texture's format has (texture.hpp: FormatInfo::element);
// a texture made without write usage at an index the kernel writes; a kernel whose numSamp does not fit NSAMP's 5 bits;
// a constant buffer it indexes (numUbos > 0: an ldc through a UBO descriptor) with nothing bound there or bound off a
// vec4. A kernel asking for the front end's null texture or a constexpr sampler (not served yet) is
// refused by name where it is compiled (compiler.cpp), since a Kernel does not carry those two facts.
bool encode(pm4::Stream& stream, const Kernel& kernel, const uint32_t groups[3], const uint32_t local[3],
            const Bindings& bindings, const uint32_t* immediates, uint64_t program, uint64_t table, uint8_t* tableCpu,
            std::string* why, const Options& options = Options{});
// dispatchThreadgroupsWithIndirectBuffer:: the group counts are three words at INDIRECT, read by the GPU when it runs
// the dispatch (written by an earlier one, as like as not), so nothing here can judge them; GROUPS is ignored.
// Refused besides what the direct form refuses: a kernel whose own count slot is not a vec4 of its own start.
bool encodeIndirect(pm4::Stream& stream, const Kernel& kernel, uint64_t indirect, const uint32_t local[3], const Bindings& bindings,
                    const uint32_t* immediates, uint64_t program, uint64_t table, uint8_t* tableCpu, std::string* why,
                    const Options& options = Options{});

}  // namespace compute

}  // namespace hakios::metal
