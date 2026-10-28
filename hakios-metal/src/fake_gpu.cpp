#include "hakios-metal/fake_gpu.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>

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

// the packets from ADDRESS on, WORDS of them; false on a fault (the submission stops there)
bool FakeGpu::execute(uint64_t address, uint32_t words, int depth) {
    const uint32_t* w = reinterpret_cast<const uint32_t*>(at(address, 4ull * words));
    if (w == nullptr) {
        fault("the command stream is not inside a buffer");
        return false;
    }
    for (uint32_t i = 0; i < words;) {
        pm4::Header h = readHeader(w[i]);
        if (h.kind == pm4::Header::kBad) {
            char text[80];
            snprintf(text, sizeof(text), "word %u (0x%08x): not a packet, or its parity is wrong", i, w[i]);
            fault(text);
            return false;
        }
        if (h.count > words - i - 1) {
            fault("a packet runs past the end of the stream");
            return false;
        }
        const uint32_t* p = w + i + 1;
        stats_.packets++;
        if (h.kind == pm4::Header::kRegisters) {
            // registers are not modelled: what a test checks is memory
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
            default: {
                char text[64];
                snprintf(text, sizeof(text), "opcode 0x%02x is not one this GPU model runs", h.opcode);
                fault(text);
                return false;
            }
            }
        }
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
    if (execute(job.address, job.words, 0)) {
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
