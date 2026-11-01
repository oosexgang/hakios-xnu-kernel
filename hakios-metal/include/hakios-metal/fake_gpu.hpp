// A GPU for the tests: the kernel driver's requests answered in this process, and the command streams run by an
// interpreter of the packets this driver writes. It is strict where the command processor is: a header whose parity
// is wrong, a count that runs past the end, an opcode it does not know, or an address outside every buffer stops the
// submission there and is counted as a fault, the way the real GPU would hang on it.
//
// What it proves and what it does not. A test that passes here has written packets that are well formed and name only
// memory the GPU was given, and a compute dispatch whose state holds together: the program preloaded from where
// SP_CS_OBJ_START points and no longer than the instruction cache (128 units hang this GPU; 127 is the limit), every
// constant load within the program's constlen, the IBO descriptors loaded from where SP_CS_IBO points, each a buffer
// view or a 2D image inside a buffer the GPU was given (a zero descriptor is a fault: an unaddressed slot gets a hole
// descriptor), the two copies of the id registers equal, the thread size the same in both places that state it, the
// grid's global size the local size times the group count, and the end-of-dispatch waits satisfiable by what the
// stream itself wrote (a wait on a word nothing writes is a hang, and is a fault here). When SP_CS_TEX_COUNT is not 0:
// the texture descriptors loaded from where SP_CS_TEX_CONST points (64-byte units, as many as the count, 64-aligned),
// at least one sampler descriptor from where SP_CS_TEX_SAMP points (16-byte units, 16-aligned), SP_CS_CONFIG's NTEX
// covering the count and equal to NSAMP (Gallium's stream, where ir3's num_samp counts images too), every non-zero
// TEX_CONST a 2D linear view of a known format whose rows lie inside a buffer and whose pitch is a multiple of
// 64 << PITCHALIGN, every TEX_SAMP with its last word 0, wrap modes the enum has, and a border clamp only with the
// border colour base registers written at a 128-aligned entry inside a buffer. What it does NOT do: run a single
// shader instruction (no ir3 interpreter: the memory a dispatch would write stays as it was), or judge the VALUE of a
// register beyond those checks. That is the phone's to prove, with hakios-metal-probe.
//
// A draw (CP_DRAW_INDX_OFFSET) is checked the same way, against the registers written before it and the loads into the
// two stages' blocks (SB6_VS_SHADER by CP_LOAD_STATE6_GEOM, SB6_FS_SHADER by _FRAG, as tu6_stage2opcode pairs them):
// the sysmem pass begun (the RM6_DIRECT_RENDER marker, both BIN_CONTROLs BUFFERS_IN_SYSMEM with no bins); the vertex
// and fragment stages enabled in both HLSQ_xS_CNTL and SP_xS_CONFIG and the geometry stages off; each program 128-byte
// aligned, instrlen * 128 bytes inside a buffer, preloaded from exactly there by the stage's opcode with at most
// instrlen (and 127) units, every constant load within the stage's decoded CONSTLEN; the FS thread size equal in
// SP_FS_CTRL_REG0 and HLSQ_FS_CNTL_0; RB_MRT[0] a linear target of a known format whose pitch holds the window's rows and
// whose rows (counted to 4) lie inside a buffer at a 64-aligned base, SP_FS_MRT_REG[0] and GRAS_LRZ_MRT_BUF_INFO_0 the
// same format, BASE_GMEM 0; one MRT in both FS_OUTPUT_CNTL1 copies, a colour register in SP_FS_OUTPUT_REG[0], all four
// components in both RENDER_COMPONENTS copies; the blend on or off alike in RB_MRT_CONTROL[0] (both BLEND bits),
// SP_BLEND_CNTL and RB_BLEND_CNTL, a blending target's factors and opcodes ones the enums have; the FS's textures and
// samplers as a dispatch's are judged (SB6_FS_TEX, SP_FS_TEX_*), and each prefetch command a plain sample of a loaded,
// non-zero texture with a loaded sampler; no depth or stencil state without a depth
// buffer; the screen and viewport scissors inside the window scissor; viewport scales finite and non-zero and the
// guardband written; the vertex fetch counts equal and every fetch inside a buffer; a position location, the VPC stride
// equal in VPC_VS_PACK and PC_VS_OUT_CNTL, SP_VS_PRIMITIVE_CNTL.OUT the outputs named; the VARYING flags agreeing across
// their four copies; the barycentric and coordinate regids agreeing across HLSQ_CONTROL_3/4, GRAS_CNTL and
// RB_RENDER_CONTROL0; MSAA off in the three DEST registers; RB_CCU_CNTL written in this submission right after a
// CP_WAIT_FOR_IDLE with the sysmem shape; and, at the submission's end, a 4-word PC_CCU_FLUSH_COLOR_TS and a 4-word
// CACHE_FLUSH_TS after the last draw ("the colour stays in the CCU" otherwise). It rasterizes nothing: coverage, the
// edge rule, interpolation, the y orientation and the bytes are the phone's.
//
// A CP_BLIT (a render pass's Clear) is the one thing it carries out: the 2D engine's solid fill, after checking that its
// registers hold together -- RB_2D_BLIT_CNTL equal to GRAS_2D_BLIT_CNTL with SOLID_COLOR and all four mask bits,
// the format the same there, in RB_2D_DST_INFO and in SP_2D_DST_FORMAT, the intermediate format the one turnip picks
// for it (format_to_ifmt), the sRGB bits agreeing, a linear destination whose rectangle lies inside a buffer -- then
// RB_2D_SRC_SOLID_C0..3 packed into every pixel of GRAS_2D_DST_TL..BR in the format's memory order. Counted as a write
// the CCU holds, like a draw. The packing is this model's reading of the registers; whether the GPU packs the same
// way is again the phone's to say.
//
// What it answers is what hakios's kernel driver answers (bsd/dev/hakios.adreno.c, hakios_gpu_parameter): GPU_ID 0,
// the chip id 0x06030500 with a speed bin above it, 512 KiB of GMEM, 550 MHz. And it fails as that kernel fails. A
// submission the GPU never finishes is a hang there, found by the wait's timeout alone (the GPU raises no fault for
// it, and none of the measured hangs set a fault register); then either the kernel resets the GPU and gives up what it
// had (boot-arg gpu_recover=1: the fence word is set to the last submission's number, so every wait ends, and FAULTS
// counts one more), or, without that, nothing is waited for again in that run. Both are here, RECOVERS choosing.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/gpu.hpp"

namespace hakios::metal {

class FakeGpu : public Transport {
public:
    // one CP_LOAD_STATE6_FRAG of constants, as a dispatch left it
    struct ConstLoad {
        uint32_t dstOffVec4, units;
        bool direct;
    };
    struct Dispatch {
        uint32_t groups[3] = {0, 0, 0};
        uint32_t local[3] = {0, 0, 0};
        uint64_t program = 0;
        uint32_t instrlen = 0, constlen = 0, ibos = 0;
        std::vector<ConstLoad> consts;
        // the texture state: SP_CS_TEX_COUNT, the sampler load's NUM_UNIT, the three tables' addresses (SP_CS_IBO,
        // SP_CS_TEX_CONST, SP_CS_TEX_SAMP), and the descriptors as read from memory at CP_EXEC_CS, so a test can hold
        // a field of them against the texture it bound
        uint32_t textureCount = 0, samplerCount = 0;
        uint64_t iboTable = 0, texConstTable = 0, texSampTable = 0;
        std::vector<a6xx::TexConst> ibo;
        std::vector<a6xx::TexConst> tex;
        std::vector<a6xx::TexSamp> samp;
    };
    // one draw as CP_DRAW_INDX_OFFSET found the registers and the loads, for a test to hold against the Texture and the
    // Draw it came from (nothing of it was rasterized)
    struct Draw {
        uint32_t primType = 0, sourceSelect = 0, visCull = 0;   // CP_DRAW_INDX_OFFSET_0's fields
        uint32_t numInstances = 0, numIndices = 0;
        bool indexed = false;                       // the 7-word DMA form
        uint32_t indexSize = 0, maxIndices = 0;     // its INDEX_SIZE and MAX_INDICES
        uint64_t indexBase = 0;                     // INDX_BASE
        uint64_t vsProgram = 0, fsProgram = 0;      // SP_VS_OBJ_START / SP_FS_OBJ_START
        uint32_t vsInstrlen = 0, fsInstrlen = 0;    // SP_VS_INSTRLEN / SP_FS_INSTRLEN
        uint32_t vsConstlen = 0, fsConstlen = 0;    // HLSQ_VS_CNTL / HLSQ_FS_CNTL's CONSTLEN, decoded (<< 2), in vec4
        std::vector<ConstLoad> vsConsts, fsConsts;  // the ST6_CONSTANTS loads into each stage's block
        // the target as RB_MRT[0] names it: the format, swap and tile mode fields, the pitches in BYTES (decoded), the base
        uint32_t mrtFormat = 0, mrtSwap = 0, mrtTileMode = 0, mrtPitch = 0, mrtArrayPitch = 0;
        uint64_t mrtBase = 0;
        uint64_t mrtBases[8] = {};                  // every target's RB_MRT_BASE, 0 to mrtCount - 1 (mrtBase is [0])
        uint32_t mrtFormats[8] = {};
        uint32_t spFsMrtReg = 0;
        uint32_t renderComponents = 0, fsRenderComponents = 0;   // RB_RENDER_COMPONENTS, SP_FS_RENDER_COMPONENTS
        uint32_t mrtCount = 0;                      // RB_FS_OUTPUT_CNTL1.MRT (== SP_FS_OUTPUT_CNTL1.MRT)
        uint32_t samples = 1;                       // the DEST_MSAA_CNTL count: 1, 2 or 4
        uint32_t mrtControl = 0, mrtBlendControl = 0;   // RB_MRT_CONTROL[0] (blend bits, component enable), RB_MRT_BLEND_CONTROL[0]
        uint32_t fsTextureCount = 0, fsSamplerCount = 0;   // SP_FS_TEX_COUNT, the SB6_FS_TEX sampler load's NUM_UNIT
        uint64_t fsTexConst = 0;                          // SP_FS_TEX_CONST: where the texture descriptors were loaded from
        uint32_t vsTextureCount = 0, vsSamplerCount = 0;   // SP_VS_TEX_COUNT, the SB6_VS_TEX sampler load's NUM_UNIT
        uint64_t vsTexConst = 0;                          // SP_VS_TEX_CONST
        uint32_t prefetchCount = 0, prefetchCmd[4] = {};  // SP_FS_PREFETCH_CNTL.COUNT, SP_FS_PREFETCH_CMD[i]
        uint32_t depthCntl = 0;                           // RB_DEPTH_CNTL
        uint32_t vsUbos = 0, fsUbos = 0;                  // the UBO descriptors each stage loaded
        uint32_t gfxIbos = 0;                             // SP_IBO_COUNT: the 3D IBO table's size
        uint64_t depthBase = 0;                           // RB_DEPTH_BUFFER_BASE when a depth buffer is bound
        uint32_t windowScissor[4] = {};             // GRAS_SC_WINDOW_SCISSOR TL x, y, BR x, y (BR inclusive)
        uint32_t screenScissor[4] = {}, viewportScissor[4] = {};   // GRAS_SC_SCREEN_SCISSOR[0], GRAS_SC_VIEWPORT_SCISSOR[0]
        float viewport[6] = {};                     // GRAS_CL_VPORT[0]: xoffset, xscale, yoffset, yscale, zoffset, zscale
        uint32_t vertexIdRegid = 0;                 // VFD_CONTROL_1.REGID4VTX
        uint32_t vfdFetchCount = 0, vfdDecodeCount = 0;   // VFD_CONTROL_0
        uint32_t vertexIndexOffset = 0;             // VFD_INDEX_OFFSET
        uint32_t positionLoc = 0, strideInVpc = 0, vsOutCount = 0;   // VPC_VS_PACK.POSITIONLOC / .STRIDE_IN_VPC, SP_VS_PRIMITIVE_CNTL.OUT
        uint32_t numNonPosVar = 0;                  // VPC_CNTL_0.NUMNONPOSVAR
        uint32_t spFsOutputReg0 = 0;                // SP_FS_OUTPUT_REG[0]
        bool ccuFlushed = false, cacheFlushed = false;   // a 4-word PC_CCU_FLUSH_COLOR_TS / CACHE_FLUSH_TS followed it (set at the submission's end)
    };
    struct Stats {
        uint32_t submits = 0;
        uint32_t packets = 0;
        uint32_t faults = 0;        // submissions that stopped on a bad packet (each one a hang on the phone)
        uint32_t recoveries = 0;    // ... of which the GPU was reset after (what FAULTS answers)
        std::string lastFault;
        uint64_t wordsCopied = 0;
        uint32_t dispatches = 0;    // CP_EXEC_CS packets whose state held together (none of them run: see above)
        Dispatch lastDispatch;
        uint32_t draws = 0;         // CP_DRAW_INDX_OFFSET packets whose state held together (none of them rasterized)
        Draw lastDraw;
        uint32_t passes = 0;        // sysmem passes begun (CP_SET_MARKER RM6_DIRECT_RENDER)
        uint32_t fills = 0;         // CP_BLIT solid fills whose state held together: these ARE carried out, into the target's memory
        uint32_t copies = 0;        // CP_BLIT copies (render::encodeBlit), carried out texel for texel or bilinear
        uint32_t tiledFills = 0;    // ...of which into a TILE6_3 depth texture: judged, not written (the tiled order is not modelled)
        uint32_t tiles = 0;         // GMEM tiles begun (CP_SET_MARKER RM6_BIN_RENDER_START | USES_GMEM)
        uint32_t tileLoads = 0, tileStores = 0;   // event blits, carried out between memory and the model's GMEM
        uint32_t tileClears = 0;    // ...and clears in GMEM
        uint32_t binningPasses = 0, binnedDraws = 0, binData = 0;   // binning passes, the draws in them, tiles told their streams
        uint64_t storedPixels = 0;  // pixels the stores wrote: the whole target once, granule padding included, when the tiles tile it
    };

    // ASYNC: submissions run on a thread of their own, after DELAY_US each, as the real GPU runs them after the call
    // has returned; otherwise at once, inside the submit call. RECOVERS: the kernel resets the GPU after a hang
    // (gpu_recover=1); false is the kernel without it, where the first hang ends every later wait at once.
    explicit FakeGpu(bool async = false, uint32_t delayUs = 0, bool recovers = true);
    ~FakeGpu() override;

    int call(uint32_t request, void* data, size_t size) override;
    void* map(uint32_t handle, uint64_t size) override;
    void unmap(uint32_t handle, void* address) override;
    int64_t nowNs() override;

    Stats stats();
    uint32_t liveBuffers();
    // An IOSurface the kernel would know by ID: its pixels at OFFSET, ROW_BYTES apart, in a buffer of whole pages, which
    // kHakiosSurface hands out (the same handle while it is open, as the kernel does) and which outlives the buffer
    // (the surface owns the memory). CLIENT: a surface around a program's own memory, which the kernel refuses.
    void addSurface(uint32_t id, uint32_t width, uint32_t height, uint32_t rowBytes, uint32_t pixelFormat, uint32_t offset = 0, uint32_t planes = 1,
                    bool client = false);
    std::vector<uint8_t> surfaceBytes(uint32_t id);   // the surface's whole memory as it is now
    std::vector<uint8_t> memory(uint64_t iova, uint64_t length);   // LENGTH bytes at a GPU address as they are now; empty outside every buffer
    void writeSurface(uint32_t id, uint64_t at, const void* bytes, uint64_t length);   // what another program drew there
    // stop running submissions until resume(): to see what a program does while the GPU is behind
    void pause();
    void resume();

    // what the kernel says about this GPU, for the fake to say the same and for a test to check it does
    static constexpr uint64_t kChipId = 0x06030500;      // dt qcom,chipid: an A635-family part, "Adreno 7c+ Gen 3" to Mesa
    static constexpr uint64_t kSpeedBin = 0x75;          // the phone's fuse value above the chip id, as the probe read it
                                                         // (0x7506030500 on 28 October); Mesa's named row for this part
    static constexpr uint64_t kGmemBytes = 512 * 1024;
    static constexpr uint64_t kMaxFrequencyHz = 550000000;

private:
    struct UboDescriptor {
        uint64_t base;
        uint32_t vec4s;
    };
    struct Buffer {
        uint64_t iova;
        std::vector<uint8_t> bytes;
    };
    struct Job {
        uint32_t fence;
        uint64_t address;
        uint32_t words;
    };
    // what a dispatch has loaded so far, for CP_EXEC_CS to be checked against
    struct CsState {
        bool programLoaded = false;
        uint64_t programFrom = 0;
        uint32_t programUnits = 0;
        bool ibosLoaded = false;
        uint64_t ibosFrom = 0;
        uint32_t iboUnits = 0;
        std::vector<UboDescriptor> ubos;   // the ST6_UBO loads into SB6_CS_SHADER, by UBO index
        // the two loads into SB6_CS_TEX: ST6_SHADER is samplers (16 bytes a unit), ST6_CONSTANTS textures (64)
        bool sampLoaded = false;
        uint64_t sampFrom = 0;
        uint32_t sampUnits = 0;
        bool texLoaded = false;
        uint64_t texFrom = 0;
        uint32_t texUnits = 0;
        std::vector<ConstLoad> consts;
    };

    // what a graphics stage has loaded so far (its SB6_xS_SHADER and SB6_xS_TEX blocks), for CP_DRAW_INDX_OFFSET
    struct StageLoads {
        bool programLoaded = false;
        uint64_t programFrom = 0;
        uint32_t programUnits = 0;
        std::vector<ConstLoad> consts;
        bool sampLoaded = false, texLoaded = false;
        uint64_t sampFrom = 0, texFrom = 0;
        uint32_t sampUnits = 0, texUnits = 0;
        std::vector<UboDescriptor> ubos;   // the ST6_UBO loads into the stage's block, by UBO index
    };

    uint8_t* at(uint64_t iova, uint64_t length);
    void run(const Job& job);
    bool execute(uint64_t address, uint32_t words, int depth);
    // CP_LOAD_STATE6_FRAG, _GEOM or the plain form (OPCODE), keyed on its state block
    bool loadState(uint32_t opcode, const uint32_t* p, uint32_t count);
    bool execCs(const uint32_t* p, uint32_t count, bool indirect = false);
    bool execDraw(const uint32_t* p, uint32_t count);
    // CP_BLIT: a solid fill only, judged against the 2D registers and then written into memory as the 2D engine would
    bool execBlit(const uint32_t* p, uint32_t count);
    bool execCopy(uint32_t fmt, uint32_t bpp, uint32_t ifmt, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint64_t base, uint64_t pitch, bool tiled);
    // one graphics stage's program and constants at the draw: the rules CP_EXEC_CS applies to the compute stage's
    bool checkStageProgram(const char* what, const StageLoads& xs, uint32_t instrlenReg, uint32_t objStartReg, uint32_t hlsqCntl, uint32_t spConfig,
                           uint32_t* instrlen, uint64_t* program, uint32_t* constlen);
    // a 2D descriptor's rule (a texture of the texture table when SAMPLED, an image at an IBO slot otherwise)
    bool check2d(const uint32_t* d, const char* what, uint32_t i, bool sampled);
    bool checkIboDescriptors(uint64_t table, uint32_t nibo);
    bool checkSampler(const uint32_t* d, uint32_t i);
    uint32_t reg(uint32_t r) const;
    uint64_t reg64(uint32_t r) const;
    void fault(const std::string& why);
    void hung();
    void worker();

    std::mutex lock_;
    std::condition_variable changed_;
    std::map<uint32_t, Buffer> buffers_;
    struct Surface {
        uint32_t width, height, rowBytes, pixelFormat, offset, planes;
        bool client;
        std::vector<uint8_t> bytes;   // while no buffer is open over it
        uint32_t handle = 0;          // the open buffer's, 0 when none
    };
    std::map<uint32_t, Surface> surfaces_;
    std::map<uint32_t, uint32_t> bufferSurface_;   // an open surface buffer's handle -> the surface's ID
    std::set<uint32_t> queues_;
    // one counter for every queue, as the kernel keeps it (it reads no queue id): the last fence handed out, the last done
    uint32_t submitted_ = 0, completed_ = 0;
    std::set<uint32_t> hungFences_;             // the submissions that stopped: their own waits fail, later ones pass
    bool wedged_ = false;                       // (RECOVERS false) after a hang: every wait fails at once
    std::deque<Job> jobs_;
    std::map<uint32_t, uint32_t> regs_;         // every register written, as written (type-4 packets)
    std::set<uint32_t> written_;                // ...of which those written in the submission running now
    CsState cs_;
    StageLoads vs_, fs_;
    bool gfxIbosLoaded_ = false;   // the 3D IBO table's load into SB6_IBO in this submission
    uint64_t gfxIbosFrom_ = 0;
    uint32_t gfxIboUnits_ = 0;
    // what a draw needs of the submission so far: the marker that begins a sysmem pass, RB_CCU_CNTL written (right after a
    // CP_WAIT_FOR_IDLE), and whether the last packet was that WFI; and what the submission's end needs of a draw: the
    // two 4-word flushes after the last one
    bool passBegun_ = false, ccuCntlWritten_ = false, ccuCntlAfterWfi_ = false, lastWasWfi_ = false;
    // inside a GMEM tile (between RM6_BIN_RENDER_START | USES_GMEM and RM6_BIN_RENDER_END), and GMEM itself: what the
    // event blits load and store (the draws rasterize nothing, so a tile holds what its load put there)
    bool inTile_ = false;
    bool binning_ = false;   // between RM6_BIN_VISIBILITY and the first tile: the draws only bin
    std::vector<uint8_t> gmem_ = std::vector<uint8_t>(a6xx::kGmemBytes);
    bool tileState(uint32_t* binW, uint32_t* binH, uint32_t win[4]);
    bool eventBlit();
    uint64_t alwaysOn_ = 0;     // CP_ALWAYS_ON_COUNTER as the fake keeps it: one more at each CP_REG_TO_MEM of it
    uint32_t drawsInSubmission_ = 0, drawsWithoutCcuFlush_ = 0, drawsWithoutCacheFlush_ = 0, drawsWithoutDepthFlush_ = 0;
    // the running sample count ZPASS_DONE copies out (an occlusion query's snapshots). The model has no rasterizer, so
    // a draw counts one sample per index it submits, times its instances: enough to tell begin from end and the
    // query's arithmetic from a wrong one; how many samples a real draw passes is the phone's (probe 9q)
    uint64_t samples_ = 0;
    uint32_t nextHandle_ = 1, nextQueue_ = 1;
    uint64_t nextIova_ = 0x100000000ull;
    Stats stats_;
    bool async_, paused_ = false, quit_ = false, recovers_;
    uint32_t delayUs_;
    std::thread thread_;
};

}  // namespace hakios::metal
