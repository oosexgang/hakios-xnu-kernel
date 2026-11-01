#include "hakios-metal/fake_gpu.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>

#include "hakios-metal/pm4.hpp"

namespace hakios::metal {

FakeGpu::FakeGpu(bool async, uint32_t delayUs) : async_(async), delayUs_(delayUs) {
    submitted_[0] = completed_[0] = 0;
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
        pm4::Header h = pm4::decode(w[i]);
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
                // as the phone's GPU: 4 Mi words in one packet hung it (27 October); the fake one refuses past 64 Ki
                if (n > 64 * 1024) {
                    fault("CP_MEMCPY of more words in one packet than the GPU was seen to take");
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

void FakeGpu::run(const Job& job) {
    stats_.submits++;
    execute(job.address, job.words, 0);
    // a faulting submission is still over: the real driver recovers the GPU and signals the fence
    completed_[job.queue] = job.fence;
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
        switch (p->param) {
        case kParamGpuId: p->value = 660; return 0;
        case kParamChipId: p->value = 0x06060001; return 0;
        case kParamGmemSize: p->value = 512 * 1024; return 0;
        default: return EINVAL;
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
        submitted_[q->id] = completed_[q->id] = 0;
        return 0;
    }
    case kSubmitQueueClose:
        return 0;
    case kGemSubmit: {
        if (size != sizeof(MsmGemSubmit)) return EINVAL;
        auto* s = static_cast<MsmGemSubmit*>(data);
        if (s->nr_cmds != 1 || s->nr_bos == 0 || submitted_.count(s->queueid) == 0) return EINVAL;
        auto* bos = reinterpret_cast<const MsmSubmitBo*>(uintptr_t(s->bos));
        auto* cmd = reinterpret_cast<const MsmSubmitCmd*>(uintptr_t(s->cmds));
        if (cmd->submit_idx >= s->nr_bos || cmd->type != kSubmitCmdBuf || cmd->size % 4 != 0) return EINVAL;
        for (uint32_t i = 0; i < s->nr_bos; i++) {
            if (buffers_.count(bos[i].handle) == 0) return ENOENT;   // a buffer the GPU would not know: refused
        }
        Buffer& c = buffers_.at(bos[cmd->submit_idx].handle);
        if (uint64_t(cmd->submit_offset) + cmd->size > c.bytes.size()) return EINVAL;
        Job job{s->queueid, ++submitted_[s->queueid], c.iova + cmd->submit_offset, cmd->size / 4};
        s->fence = job.fence;
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
        if (completed_.count(w->queueid) == 0) return EINVAL;
        if (w->fence > submitted_[w->queueid]) return EINVAL;     // a fence never handed out
        int64_t deadline = w->timeout.seconds * 1000000000 + w->timeout.nanoseconds;
        while (completed_[w->queueid] < w->fence) {
            int64_t left = deadline - nowNs();
            if (left <= 0) return ETIMEDOUT;
            changed_.wait_for(g, std::chrono::nanoseconds(left));
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
