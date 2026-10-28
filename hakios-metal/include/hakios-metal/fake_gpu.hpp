// A GPU for the tests: the kernel driver's requests answered in this process, and the command streams run by an
// interpreter of the packets this driver writes. It is strict where the command processor is: a header whose parity
// is wrong, a count that runs past the end, an opcode it does not know, or an address outside every buffer stops the
// submission there and is counted as a fault, the way the real GPU would hang on it.
//
// What it proves and what it does not. A test that passes here has written packets that are well formed and name only
// memory the GPU was given, and a compute dispatch whose state holds together: the program preloaded from where
// SP_CS_OBJ_START points and no longer than the instruction cache (128 units hang this GPU; 127 is the limit), every
// constant load within the program's constlen, the IBO descriptors loaded from where SP_CS_IBO points, each a buffer
// view inside a buffer the GPU was given, the two copies of the id registers equal, the thread size the same in both
// places that state it, the grid's global size the local size times the group count, and the end-of-dispatch waits
// satisfiable by what the stream itself wrote (a wait on a word nothing writes is a hang, and is a fault here). What it
// does NOT do: run a single shader instruction (no ir3 interpreter: the memory a dispatch would write stays as it was),
// or judge the VALUE of a register beyond those checks. That is the phone's to prove, with hakios-metal-probe.
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
        std::vector<ConstLoad> consts;
    };

    uint8_t* at(uint64_t iova, uint64_t length);
    void run(const Job& job);
    bool execute(uint64_t address, uint32_t words, int depth);
    bool loadState(const uint32_t* p, uint32_t count);
    bool execCs(const uint32_t* p, uint32_t count);
    uint32_t reg(uint32_t r) const;
    uint64_t reg64(uint32_t r) const;
    void fault(const std::string& why);
    void hung();
    void worker();

    std::mutex lock_;
    std::condition_variable changed_;
    std::map<uint32_t, Buffer> buffers_;
    std::set<uint32_t> queues_;
    // one counter for every queue, as the kernel keeps it (it reads no queue id): the last fence handed out, the last done
    uint32_t submitted_ = 0, completed_ = 0;
    std::set<uint32_t> hungFences_;             // the submissions that stopped: their own waits fail, later ones pass
    bool wedged_ = false;                       // (RECOVERS false) after a hang: every wait fails at once
    std::deque<Job> jobs_;
    std::map<uint32_t, uint32_t> regs_;         // every register written, as written (type-4 packets)
    CsState cs_;
    uint32_t nextHandle_ = 1, nextQueue_ = 1;
    uint64_t nextIova_ = 0x100000000ull;
    Stats stats_;
    bool async_, paused_ = false, quit_ = false, recovers_;
    uint32_t delayUs_;
    std::thread thread_;
};

}  // namespace hakios::metal
