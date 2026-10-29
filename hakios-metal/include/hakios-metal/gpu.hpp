// The GPU as the kernel's driver (HakiosGPU, drivers/gpu/hakios.gpu.cpp in hakios) offers it: Linux's msm DRM requests
// (include/uapi/drm/msm_drm.h), each one a call of the driver's IOKit user client whose selector is the request's
// number and whose structure goes in and comes back as it is. freedreno reaches the same driver the same way, through
// hakios's stand-in for libdrm (tools/mesa/hakios-drm); this is the Metal driver's own way there, without Mesa.
//
// What it does: buffers (GEM objects) with their GPU address and a CPU mapping, submissions of command streams, and
// waits on the fences the driver hands back. The Transport under it is the only part that knows IOKit, so that the
// rest is tested on any machine against a fake GPU (fake_gpu.hpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hakios::metal {

// --- the requests, as the kernel's driver numbers and lays them out ---------------------------------------------------

enum Request : uint32_t {
    kGemClose = 0x09,
    kGetParam = 0x40,
    kGemNew = 0x42,
    kGemInfo = 0x43,
    kGemCpuPrep = 0x44,
    kGemCpuFini = 0x45,
    kGemSubmit = 0x46,
    kWaitFence = 0x47,
    kSubmitQueueNew = 0x4a,
    kSubmitQueueClose = 0x4b,
};

struct MsmTimespec { int64_t seconds, nanoseconds; };
struct MsmParam { uint32_t pipe, param; uint64_t value; uint32_t len, pad; };
struct MsmGemNew { uint64_t size; uint32_t flags, handle; };
struct MsmGemInfo { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct MsmGemCpuPrep { uint32_t handle, op; MsmTimespec timeout; };
struct MsmGemCpuFini { uint32_t handle; };
struct MsmSubmitCmd { uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs; uint64_t relocs; };
struct MsmSubmitBo { uint32_t flags, handle; uint64_t presumed; };
struct MsmGemSubmit {
    uint32_t flags, fence, nr_bos, nr_cmds;
    uint64_t bos, cmds;
    int32_t fence_fd;
    uint32_t queueid;
    uint64_t in_syncobjs, out_syncobjs;
    uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct MsmWaitFence { uint32_t fence, flags; MsmTimespec timeout; uint32_t queueid; };
struct MsmSubmitQueue { uint32_t flags, prio, id; };
struct DrmGemClose { uint32_t handle, pad; };

static_assert(sizeof(MsmParam) == 24 && sizeof(MsmGemNew) == 16 && sizeof(MsmGemInfo) == 24, "msm_drm.h layout");
static_assert(sizeof(MsmSubmitCmd) == 32 && sizeof(MsmSubmitBo) == 16 && sizeof(MsmGemSubmit) == 72, "msm_drm.h layout");
static_assert(sizeof(MsmWaitFence) == 32, "msm_drm.h layout");

enum : uint32_t {
    kPipe3D = 1,
    kParamGpuId = 1,
    kParamGmemSize = 2,
    kParamChipId = 3,
    kParamMaxFreq = 4,
    kInfoGetOffset = 0,
    kInfoGetIova = 1,
    kBoGpuReadOnly = 0x2,
    kBoWriteCombined = 0x00020000,
    kSubmitBoRead = 0x1,
    kSubmitBoWrite = 0x2,
    kSubmitCmdBuf = 1,
};

// --- under it: the one part that talks to the kernel --------------------------------------------------------------------

class Transport {
public:
    virtual ~Transport() = default;
    // the request with its structure in and out; 0, or an errno (ENOMEM, EINVAL, ETIMEDOUT, ...)
    virtual int call(uint32_t request, void* data, size_t size) = 0;
    // the buffer's memory in this process (the driver's client memory of the type that is the handle); nullptr if not
    virtual void* map(uint32_t handle, uint64_t size) = 0;
    virtual void unmap(uint32_t handle, void* address) = 0;
    // now, in nanoseconds of the clock the driver's deadlines are in (mach_continuous_time on the phone)
    virtual int64_t nowNs() = 0;
};

// HakiosGPU's user client (Darwin only: src/iokit_transport.cpp). nullptr when there is no such service: no gpu=1,
// no firmware, QEMU, or not the phone.
std::unique_ptr<Transport> openKernelGpu(std::string* why);

// --- the GPU --------------------------------------------------------------------------------------------------------

class Gpu;

// One GEM object. It is the GPU's for as long as it lives; the destructor gives it back.
class Bo {
public:
    ~Bo();
    Bo(const Bo&) = delete;
    Bo& operator=(const Bo&) = delete;

    uint32_t handle() const { return handle_; }
    uint64_t size() const { return size_; }
    uint64_t gpuAddress() const { return iova_; }
    void* cpu() const { return cpu_; }      // write-combined: write it in order, read it rarely

private:
    friend class Gpu;
    Bo(Gpu& gpu, uint32_t handle, uint64_t size, uint64_t iova, void* cpu)
        : gpu_(gpu), handle_(handle), size_(size), iova_(iova), cpu_(cpu) {}
    Gpu& gpu_;
    uint32_t handle_;
    uint64_t size_;
    uint64_t iova_;
    void* cpu_;
};

struct BoUse {
    const Bo* bo;
    bool write;
};

class Gpu {
public:
    explicit Gpu(std::unique_ptr<Transport> transport) : t_(std::move(transport)) {}

    // what the driver says about the GPU (GPU_ID 660 on the SC7280): 0 and *value, or an errno
    int param(uint32_t which, uint64_t* value);

    // a buffer of SIZE bytes (rounded up to a page), its GPU address and its mapping; nullptr with *error set
    std::unique_ptr<Bo> newBo(uint64_t size, uint32_t flags, int* error);

    // a submission queue of its own (the driver keeps an order per queue); 0 is the default one
    int newQueue(uint32_t priority, uint32_t* id);
    void closeQueue(uint32_t id);

    // the words [offset, offset + words) of COMMANDS run by the GPU, with the buffers it reads and writes;
    // 0 and the fence, or an errno
    int submit(uint32_t queue, const Bo& commands, uint32_t offsetBytes, uint32_t words, const std::vector<BoUse>& uses,
               uint32_t* fence);

    // until the fence has passed, or TIMEOUT_NS: 0, ETIMEDOUT, or another errno
    int wait(uint32_t queue, uint32_t fence, int64_t timeoutNs);

    // until the GPU is done with the buffer (as Metal's -waitUntilCompleted needs for a buffer the CPU reads next)
    int cpuPrep(const Bo& bo, uint32_t op, int64_t timeoutNs);
    void cpuFini(const Bo& bo);

    Transport& transport() { return *t_; }

private:
    friend class Bo;
    void release(Bo& bo);
    std::unique_ptr<Transport> t_;
};

}  // namespace hakios::metal
