#include "hakios-metal/gpu.hpp"

#include <cerrno>
#include <cstring>

namespace hakios::metal {

namespace {
constexpr uint64_t kPage = 0x4000;  // Darwin's page; the driver rounds to its own (4 KB) inside it

MsmTimespec deadline(Transport& t, int64_t ns) {
    // the driver takes a deadline, not a length: a time of the clock that goes on while the machine sleeps
    // (mach_continuous_time, which is Darwin's CLOCK_MONOTONIC), as Linux's msm takes one of CLOCK_MONOTONIC
    if (ns < 0) {
        ns = 0;
    }
    int64_t at = t.nowNs() + ns;
    return MsmTimespec{at / 1000000000, at % 1000000000};
}
}  // namespace

int Gpu::param(uint32_t which, uint64_t* value) {
    MsmParam p{};
    p.pipe = kPipe3D;
    p.param = which;
    int e = t_->call(kGetParam, &p, sizeof(p));
    if (e == 0) {
        *value = p.value;
    }
    return e;
}

std::unique_ptr<Bo> Gpu::newBo(uint64_t size, uint32_t flags, int* error) {
    *error = 0;
    if (size == 0) {
        *error = EINVAL;
        return nullptr;
    }
    MsmGemNew n{};
    n.size = (size + kPage - 1) & ~(kPage - 1);
    n.flags = flags;
    if ((*error = t_->call(kGemNew, &n, sizeof(n))) != 0) {
        return nullptr;
    }
    MsmGemInfo info{};
    info.handle = n.handle;
    info.info = kInfoGetIova;
    if ((*error = t_->call(kGemInfo, &info, sizeof(info))) != 0) {
        DrmGemClose c{n.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        return nullptr;
    }
    void* cpu = t_->map(n.handle, n.size);
    if (cpu == nullptr) {
        DrmGemClose c{n.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        *error = ENOMEM;
        return nullptr;
    }
    return std::unique_ptr<Bo>(new Bo(*this, n.handle, n.size, info.value, cpu));
}

Bo::~Bo() { gpu_.release(*this); }

void Gpu::release(Bo& bo) {
    if (bo.cpu_ != nullptr) {
        t_->unmap(bo.handle_, bo.cpu_);
    }
    DrmGemClose c{bo.handle_, 0};
    t_->call(kGemClose, &c, sizeof(c));
}

int Gpu::newQueue(uint32_t priority, uint32_t* id) {
    MsmSubmitQueue q{0, priority, 0};
    int e = t_->call(kSubmitQueueNew, &q, sizeof(q));
    if (e == 0) {
        *id = q.id;
    }
    return e;
}

void Gpu::closeQueue(uint32_t id) {
    if (id != 0) {
        uint32_t q = id;
        t_->call(kSubmitQueueClose, &q, sizeof(q));
    }
}

int Gpu::submit(uint32_t queue, const Bo& commands, uint32_t offsetBytes, uint32_t words, const std::vector<BoUse>& uses,
                uint32_t* fence) {
    if (words == 0 || offsetBytes % 4 != 0 || offsetBytes + 4ull * words > commands.size()) {
        return EINVAL;
    }
    // the command buffer is the first of the buffers; the others follow it once each, read or written
    std::vector<MsmSubmitBo> bos;
    bos.push_back(MsmSubmitBo{kSubmitBoRead, commands.handle(), commands.gpuAddress()});
    for (const BoUse& u : uses) {
        if (u.bo == &commands) {
            continue;
        }
        bool seen = false;
        for (MsmSubmitBo& b : bos) {
            if (b.handle == u.bo->handle()) {
                b.flags |= u.write ? kSubmitBoWrite : kSubmitBoRead;
                seen = true;
            }
        }
        if (!seen) {
            bos.push_back(MsmSubmitBo{u.write ? kSubmitBoWrite : kSubmitBoRead, u.bo->handle(), u.bo->gpuAddress()});
        }
    }
    MsmSubmitCmd cmd{};
    cmd.type = kSubmitCmdBuf;
    cmd.submit_idx = 0;
    cmd.submit_offset = offsetBytes;
    cmd.size = words * 4;
    MsmGemSubmit s{};
    s.flags = kPipe3D;
    s.nr_bos = uint32_t(bos.size());
    s.nr_cmds = 1;
    s.bos = uint64_t(uintptr_t(bos.data()));
    s.cmds = uint64_t(uintptr_t(&cmd));
    s.fence_fd = -1;
    s.queueid = queue;
    int e = t_->call(kGemSubmit, &s, sizeof(s));
    if (e == 0) {
        *fence = s.fence;
    }
    return e;
}

int Gpu::wait(uint32_t queue, uint32_t fence, int64_t timeoutNs) {
    MsmWaitFence w{};
    w.fence = fence;
    w.timeout = deadline(*t_, timeoutNs);
    w.queueid = queue;
    return t_->call(kWaitFence, &w, sizeof(w));
}

int Gpu::cpuPrep(const Bo& bo, uint32_t op, int64_t timeoutNs) {
    MsmGemCpuPrep p{bo.handle(), op, deadline(*t_, timeoutNs)};
    return t_->call(kGemCpuPrep, &p, sizeof(p));
}

void Gpu::cpuFini(const Bo& bo) {
    MsmGemCpuFini f{bo.handle()};
    t_->call(kGemCpuFini, &f, sizeof(f));
}

}  // namespace hakios::metal
