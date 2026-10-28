// hakios-metal-probe: the driver's core on the phone's real GPU. What the tests check against the fake GPU, done by
// the Adreno: a copy of a megabyte, fills of every awkward length, a chain of command buffers whose handlers must run
// in order, and how fast a copy goes. One line per check, "probe: ok ..." or "probe: FAIL ...", and a last line with
// the count; the exit status is the number of failures. Run it from a job list (hakios: RC_JOBS, EXTRA=).
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <vector>

#include "hakios-metal/command.hpp"

using namespace hakios::metal;

static int failures = 0;

static void report(bool ok, const std::string& what) {
    std::printf("probe: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    failures += !ok;
}

int main() {
    std::string why;
    auto transport = openKernelGpu(&why);
    if (transport == nullptr) {
        std::printf("probe: no GPU: %s\n", why.c_str());
        return 100;
    }
    Gpu gpu(std::move(transport));
    uint64_t id = 0, chip = 0;
    gpu.param(kParamGpuId, &id);
    gpu.param(kParamChipId, &chip);
    std::printf("probe: GPU %llu, chip id 0x%llx\n", (unsigned long long)id, (unsigned long long)chip);
    auto queue = CommandQueue::make(gpu, &why);
    if (queue == nullptr) {
        report(false, "a queue: " + why);
        return 1;
    }

    // 1. a copy of a megabyte, checked byte for byte and around its edges
    const uint64_t mb = 1 << 20;
    auto a = newBuffer(gpu, mb, &why), b = newBuffer(gpu, mb, &why);
    if (a == nullptr || b == nullptr) {
        report(false, "two buffers of a megabyte: " + why);
        return 1;
    }
    std::vector<uint32_t> pattern(mb / 4);
    std::mt19937 rng(1);
    for (auto& w : pattern) w = rng();
    memcpy(a->contents(), pattern.data(), mb);
    memset(b->contents(), 0, mb);
    {
        auto cb = queue->commandBuffer();
        bool recorded = cb->copy(*a, 4096, *b, 8192, 500000);
        cb->commit();
        cb->waitUntilCompleted();
        std::vector<uint8_t> back(mb);
        memcpy(back.data(), b->contents(), mb);   // one read of the write-combined memory, in order
        bool same = memcmp(back.data() + 8192, reinterpret_cast<uint8_t*>(pattern.data()) + 4096, 500000) == 0;
        bool edges = true;
        for (size_t i = 0; i < 8192; i++) edges = edges && back[i] == 0;
        for (size_t i = 8192 + 500000; i < mb; i++) edges = edges && back[i] == 0;
        report(recorded && cb->status() == CommandBuffer::kCompleted && same && edges,
               "CP_MEMCPY of 500000 bytes: " + std::string(same ? "the bytes are the same" : "the bytes DIFFER") +
                   (edges ? ", nothing around them touched" : ", bytes around them CHANGED") +
                   (cb->error().empty() ? "" : " (" + cb->error() + ")") + ", " +
                   std::to_string(cb->gpuNanoseconds() / 1000) + " us");
    }

    // 2. fills of lengths that end inside the doubling, at an offset
    for (uint64_t len : {4ull, 256ull, 260ull, 65540ull, 999996ull}) {
        memset(b->contents(), 0x11, mb);
        auto cb = queue->commandBuffer();
        cb->fill(*b, 16, len, 0xa5);
        cb->commit();
        cb->waitUntilCompleted();
        std::vector<uint8_t> back(mb);
        memcpy(back.data(), b->contents(), mb);
        bool ok = back[15] == 0x11 && (16 + len >= mb || back[16 + len] == 0x11);
        for (uint64_t i = 16; i < 16 + len; i++) ok = ok && back[i] == 0xa5;
        report(ok && cb->status() == CommandBuffer::kCompleted, "fill of " + std::to_string(len) + " bytes at 16");
    }

    // 3. forty command buffers in flight at once: their handlers in commit order, and the memory as the last left it
    {
        std::vector<int> order;
        std::mutex m;
        std::vector<std::shared_ptr<CommandBuffer>> all;
        for (int i = 0; i < 40; i++) {
            auto cb = queue->commandBuffer();
            cb->fill(*b, 4 * uint64_t(i), 4, uint8_t(i + 1));
            cb->addCompletedHandler([&, i](CommandBuffer&) {
                std::lock_guard<std::mutex> g(m);
                order.push_back(i);
            });
            cb->commit();
            all.push_back(cb);
        }
        for (auto& cb : all) cb->waitUntilCompleted();
        bool inOrder = order.size() == 40;
        for (int i = 0; inOrder && i < 40; i++) inOrder = order[i] == i;
        std::vector<uint8_t> back(160);
        memcpy(back.data(), b->contents(), 160);
        bool mem = true;
        for (int i = 0; i < 40; i++) mem = mem && back[4 * i] == i + 1;
        report(inOrder && mem, "40 command buffers in flight: handlers " + std::string(inOrder ? "in order" : "OUT OF ORDER") +
                                   ", memory " + (mem ? "as written" : "WRONG"));
    }

    // 4. how fast the command processor copies: 16 MB, five times
    {
        auto big1 = newBuffer(gpu, 16 * mb, &why), big2 = newBuffer(gpu, 16 * mb, &why);
        if (big1 && big2) {
            int64_t best = 0;
            bool ok = true;
            std::string why;
            for (int i = 0; i < 5 && ok; i++) {
                auto cb = queue->commandBuffer();
                cb->copy(*big1, 0, *big2, 0, 16 * mb);
                cb->commit();
                cb->waitUntilCompleted();
                ok = cb->status() == CommandBuffer::kCompleted;
                why = cb->error();
                if (ok && (best == 0 || cb->gpuNanoseconds() < best)) best = cb->gpuNanoseconds();
            }
            // (a speed is only a speed of a copy that completed)
            report(ok, ok ? "16 MB by CP_MEMCPY in " + std::to_string(best / 1000) + " us at best (" +
                                std::to_string(int(16.0 * mb / best * 1000)) + " MB/s, commit to fence)"
                          : "16 MB by CP_MEMCPY: " + why);
        } else {
            report(false, "two buffers of 16 MB: " + why);
        }
    }

    std::printf("probe: %d failure%s\n", failures, failures == 1 ? "" : "s");
    return failures;
}
