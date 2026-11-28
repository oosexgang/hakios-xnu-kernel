// hakios-metal-bench: where the time goes in the Metal layer on the phone, in microseconds (run beside the window
// server: RC_JOBS=rc.jobs.metalbench). Everything it prints is a line "bench: NAME  N  mean/min/p50 ...".
// Buffer allocation and mapping, a submit that does nothing, a fill, a copy and a compute dispatch with their round
// trip (commit to waitUntilCompleted) and pipelined (many commits, one wait), the CPU's own copies in GPU memory.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <sys/sysctl.h>
#include <unistd.h>

#include "hakios-metal/command.hpp"
#include "hakios-metal/hakios_metal.h"
#include "../tests/fixtures/fill_id_air.hpp"

using namespace hakios::metal;
using Clock = std::chrono::steady_clock;

static double us(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::micro>(b - a).count(); }

static void echoIntoKernelLog() {
    int fds[2];
    if (pipe(fds) != 0) return;
    const int console = dup(1);
    if (console < 0 || dup2(fds[1], 1) < 0) return;
    close(fds[1]);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::thread([reader = fds[0], console] {
        std::string line;
        char buf[512];
        ssize_t n;
        while ((n = read(reader, buf, sizeof(buf))) > 0) {
            (void)!write(console, buf, size_t(n));
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] != '\n') { line += buf[i]; continue; }
                sysctlbyname("kern.hakios_say", nullptr, nullptr, (void*)line.c_str(), line.size() > 250 ? 250 : line.size());
                line.clear();
            }
        }
    }).detach();
    atexit([] { fflush(stdout); close(1); usleep(300 * 1000); });
}

static void stats(const char* name, std::vector<double> v, const char* unit = "us") {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    std::printf("bench: %-44s n=%-4zu mean %9.1f  p50 %9.1f  p90 %9.1f  min %9.1f %s\n", name, v.size(), sum / v.size(), v[v.size() / 2],
                v[v.size() * 9 / 10], v[0], unit);
}

int main() {
    echoIntoKernelLog();
    std::string why;
    auto transport = openKernelGpu(&why);
    if (transport == nullptr) { std::printf("bench: no GPU: %s\n", why.c_str()); return 100; }
    Gpu gpu(std::move(transport));
    auto queue = CommandQueue::make(gpu, &why);
    if (queue == nullptr) { std::printf("bench: no queue: %s\n", why.c_str()); return 1; }
    std::printf("bench: begin chip %#x\n", gpu.chipId());

    // 1. buffer allocation and release (kernel object, its address, the mapping; then the unmap and close)
    for (uint64_t size : {4096ull, 65536ull, 1ull << 20, 16ull << 20}) {
        std::vector<double> alloc, release, touch;
        for (int i = 0; i < 40; i++) {
            auto t0 = Clock::now();
            auto b = newBuffer(gpu, size, &why);
            auto t1 = Clock::now();
            if (b == nullptr) { std::printf("bench: alloc %llu: %s\n", (unsigned long long)size, why.c_str()); break; }
            memset(b->contents(), 1, size < 4096 ? size : 4096);
            auto t2 = Clock::now();
            b.reset();
            auto t3 = Clock::now();
            alloc.push_back(us(t0, t1));
            touch.push_back(us(t1, t2));
            release.push_back(us(t2, t3));
        }
        char n[64];
        snprintf(n, sizeof n, "buffer new %lluK", (unsigned long long)(size >> 10));
        stats(n, alloc);
        snprintf(n, sizeof n, "buffer free %lluK", (unsigned long long)(size >> 10));
        stats(n, release);
        snprintf(n, sizeof n, "buffer first 4K touch %lluK", (unsigned long long)(size >> 10));
        stats(n, touch);
    }

    // 1b. the same through the C interface the plug-in uses (newBufferWithLength: / release), which keeps buffers given back
    if (hakios_metal_device* dev = hakios_metal_open()) {
        for (uint64_t size : {4096ull, 65536ull, 1ull << 20, 16ull << 20}) {
            std::vector<double> both;
            for (int i = 0; i < 40; i++) {
                auto t0 = Clock::now();
                hakios_metal_buffer* b = hakios_metal_buffer_new(dev, size);
                if (b == nullptr) break;
                memset(hakios_metal_buffer_contents(b), 1, 64);
                hakios_metal_buffer_free(b);
                both.push_back(us(t0, Clock::now()));
            }
            char n[64];
            snprintf(n, sizeof n, "plugin buffer new+touch+free %lluK", (unsigned long long)(size >> 10));
            stats(n, both);
        }
        hakios_metal_close(dev);
    } else {
        std::printf("bench: no C interface device: %s\n", hakios_metal_error());
    }

    auto a = newBuffer(gpu, 16 << 20, &why), b = newBuffer(gpu, 16 << 20, &why), cbuf = newBuffer(gpu, 64, &why);
    if (!a || !b || !cbuf) { std::printf("bench: buffers: %s\n", why.c_str()); return 1; }
    memset(a->contents(), 0, 16 << 20);
    static_cast<uint32_t*>(cbuf->contents())[0] = 0x1234;

    // 2. CPU against GPU memory (write-combined mappings)
    {
        std::vector<uint8_t> host(1 << 20);
        std::vector<double> w, r, cc;
        for (int i = 0; i < 8; i++) {
            auto t0 = Clock::now();
            memset(a->contents(), i, 1 << 20);
            auto t1 = Clock::now();
            volatile uint32_t sink = 0;
            uint32_t acc = 0;
            const uint32_t* q = static_cast<const uint32_t*>(a->contents());
            for (int k = 0; k < (1 << 18); k += 16) acc += q[k];
            sink = acc;
            (void)sink;
            auto t2 = Clock::now();
            memcpy(b->contents(), host.data(), 1 << 20);
            auto t3 = Clock::now();
            w.push_back(us(t0, t1));
            r.push_back(us(t1, t2));
            cc.push_back(us(t2, t3));
        }
        {
            std::vector<double> rb, rb4;
            for (int i = 0; i < 8; i++) {
                auto t0 = Clock::now();
                memcpy(host.data(), a->contents(), 1 << 20);
                auto t1 = Clock::now();
                memcpy(host.data(), a->contents(), 4096);
                auto t2 = Clock::now();
                rb.push_back(us(t0, t1));
                rb4.push_back(us(t1, t2));
            }
            stats("cpu memcpy 1MB GPU buffer -> host (readback)", rb);
            stats("cpu memcpy 4KB GPU buffer -> host (readback)", rb4);
        }
        stats("cpu memset 1MB into GPU buffer", w);
        stats("cpu read 1 of 16 words of 1MB GPU buffer", r);
        stats("cpu memcpy 1MB host -> GPU buffer", cc);
    }

    // 3. a submit that does a word: round trip, and 64 in flight
    auto rtt = [&](const char* name, auto&& record, int rounds) {
        std::vector<double> v;
        for (int i = 0; i < rounds; i++) {
            auto t0 = Clock::now();
            auto cb = queue->commandBuffer();
            record(*cb);
            cb->commit();
            cb->waitUntilCompleted();
            auto t1 = Clock::now();
            if (cb->status() != CommandBuffer::kCompleted) { std::printf("bench: %s failed: %s\n", name, cb->error().c_str()); return; }
            v.push_back(us(t0, t1));
        }
        stats(name, v);
    };
    auto pipelined = [&](const char* name, auto&& record, int count) {
        std::vector<double> v;
        for (int round = 0; round < 5; round++) {
            auto t0 = Clock::now();
            std::shared_ptr<CommandBuffer> last;
            for (int i = 0; i < count; i++) {
                auto cb = queue->commandBuffer();
                record(*cb);
                cb->commit();
                last = cb;
            }
            last->waitUntilCompleted();
            auto t1 = Clock::now();
            if (last->status() != CommandBuffer::kCompleted) { std::printf("bench: %s failed: %s\n", name, last->error().c_str()); return; }
            v.push_back(us(t0, t1) / count);
        }
        stats(name, v, "us per command buffer");
    };
    const uint32_t local[3] = {64, 1, 1}, groups[3] = {16, 1, 1};
    auto small = newBuffer(gpu, 4096, &why);
    auto fill4 = [&](CommandBuffer& cb) { cb.fill(*a, 0, 4, 1); };
    auto copy4k = [&](CommandBuffer& cb) { cb.copy(*a, 0, *b, 0, 4096); };
    auto copy64k = [&](CommandBuffer& cb) { cb.copy(*a, 0, *b, 0, 65536); };
    auto dispatch = [&](CommandBuffer& cb) { cb.dispatch(kernels::fill_id_air, groups, local, {Binding{small.get(), 0}, Binding{cbuf.get(), 0}}); };
    // (the first of each is warm-up: the kernel's program upload, the queue's buffers)
    for (int i = 0; i < 3; i++) { rtt("warm", fill4, 1); rtt("warm", dispatch, 1); }
    rtt("submit+wait: fill 4 bytes", fill4, 200);
    rtt("submit+wait: copy 4 KB", copy4k, 200);
    rtt("submit+wait: copy 64 KB", copy64k, 100);
    rtt("submit+wait: copy 1 MB", [&](CommandBuffer& cb) { cb.copy(*a, 0, *b, 0, 1 << 20); }, 50);
    rtt("submit+wait: copy 16 MB", [&](CommandBuffer& cb) { cb.copy(*a, 0, *b, 0, 16 << 20); }, 10);
    pipelined("pipelined x8: copy 1 MB (CP_MEMCPY)", [&](CommandBuffer& cb) { cb.copy(*a, 0, *b, 0, 1 << 20); }, 8);
    rtt("submit+wait: dispatch fill_id 1024 words", dispatch, 200);
    pipelined("pipelined x64: fill 4 bytes", fill4, 64);
    pipelined("pipelined x64: copy 4 KB", copy4k, 64);
    pipelined("pipelined x64: dispatch fill_id", dispatch, 64);
    // a dispatch then a copy of its result in one command buffer (what a filter does between passes)
    rtt("submit+wait: dispatch + copy 4 KB (1 CB)", [&](CommandBuffer& cb) { dispatch(cb); cb.copy(*small, 0, *b, 0, 4096); }, 100);
    rtt("submit+wait: 4 dispatches (1 CB)", [&](CommandBuffer& cb) { for (int i = 0; i < 4; i++) dispatch(cb); }, 100);
    // only CPU: record a command buffer and do not commit (encode cost)
    {
        std::vector<double> v;
        for (int i = 0; i < 200; i++) {
            auto t0 = Clock::now();
            auto cb = queue->commandBuffer();
            dispatch(*cb);
            auto t1 = Clock::now();
            v.push_back(us(t0, t1));
        }
        stats("encode only: dispatch (no commit)", v);
    }
    // a buffer made, filled by the CPU, used by a dispatch, read back: the whole of a small job
    {
        std::vector<double> v;
        for (int i = 0; i < 100; i++) {
            auto t0 = Clock::now();
            auto out = newBuffer(gpu, 4096, &why);
            auto cb = queue->commandBuffer();
            cb->dispatch(kernels::fill_id_air, groups, local, {Binding{out.get(), 0}, Binding{cbuf.get(), 0}});
            cb->commit();
            cb->waitUntilCompleted();
            volatile uint32_t x = static_cast<const uint32_t*>(out->contents())[5];
            (void)x;
            out.reset();
            cb.reset();
            auto t1 = Clock::now();
            v.push_back(us(t0, t1));
        }
        stats("whole job: new buffer+dispatch+wait+read+free", v);
    }
    std::printf("bench: done, FAULTS %llu\n", (unsigned long long)gpu.faults());
    return 0;
}
