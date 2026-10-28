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

// a buffer bound at an index: Metal's [[buffer(i)]], a text kernel's @buf i
struct Binding {
    const Buffer* buffer = nullptr;
    uint64_t offset = 0;      // bytes into it; a multiple of 4 (the descriptors address elements)
};

namespace compute {

constexpr uint32_t kMaxLocalThreads = 1024;     // the hardware's threadgroup limit, and the NDRANGE fields' (10 bits)
// what a dispatch owns in GPU memory besides the program: the IBO descriptors, then the end-of-dispatch timestamps
constexpr uint32_t kTableBytes = 4096;
constexpr uint32_t kTableTimestampsAt = 2048;   // the two timestamp words the cache flush writes and waits for
constexpr uint32_t kMaxIbos = kTableTimestampsAt / 64;

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
};

// The whole dispatch into STREAM: KERNEL over GROUPS work groups of LOCAL threads each, BUFFERS bound by index,
// IMMEDIATES in place of the kernel's own (kernel.numImmediates dwords; nullptr keeps the kernel's). PROGRAM is the GPU
// address of the kernel's words, TABLE and TABLE_CPU the kTableBytes the dispatch owns, whose descriptors this writes.
// False with *why when the kernel and the bindings do not fit together: then nothing was added to the stream.
bool encode(pm4::Stream& stream, const Kernel& kernel, const uint32_t groups[3], const uint32_t local[3],
            const std::vector<Binding>& buffers, const uint32_t* immediates, uint64_t program, uint64_t table,
            uint8_t* tableCpu, std::string* why, const Options& options = Options{});

}  // namespace compute

}  // namespace hakios::metal
