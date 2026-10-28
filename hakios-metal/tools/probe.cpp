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
#include "../tests/fixtures/end_only.hpp"
#include "../tests/fixtures/fill_id.hpp"
#include "../tests/fixtures/fill_id_air.hpp"

using namespace hakios::metal;

static int failures = 0;

static void report(bool ok, const std::string& what) {
    std::printf("probe: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    failures += !ok;
}

// fill_id's answer for one invocation: gid * K + c, modulo 2^32 (K odd: no two invocations write the same word)
static uint32_t expectedWord(uint32_t gid, uint32_t c) { return gid * 0x9E3779B1u + c; }

// A value no invocation of a grid of WORDS can write with this C, so that a word still holding it was not written: a
// dispatch that did not run then reads as "did not run" and not as a result. (Checked, not assumed: K is a bijection,
// so the candidate collides with exactly one gid of all 2^32, and that gid may be inside the grid.)
static uint32_t canaryFor(uint32_t words, uint32_t c) {
    uint32_t canary = 0xa5a5a5a5u;
    for (;;) {
        bool clash = false;
        for (uint32_t gid = 0; gid < words && !clash; gid++) clash = expectedWord(gid, c) == canary;
        if (!clash) return canary;
        canary += 0x01010101u;
    }
}

// What a buffer holds after a dispatch, against the kernel's answer for WRITTEN words and the canary beyond them
struct Readback {
    uint32_t right = 0, canary = 0, wrong = 0;   // of the WRITTEN words
    uint32_t beyondCanary = 0, beyondOther = 0;  // of the words after them
    uint32_t firstWrongAt = 0, firstWrongValue = 0;
};

static Readback readBack(const Buffer& b, uint32_t written, uint32_t c, uint32_t canaryValue) {
    std::vector<uint32_t> back(b.length() / 4);
    memcpy(back.data(), b.contents(), b.length());   // one read of the write-combined memory, in order
    Readback r;
    for (uint32_t i = 0; i < back.size(); i++) {
        if (i < written) {
            if (back[i] == expectedWord(i, c)) {
                r.right++;
            } else if (back[i] == canaryValue) {
                r.canary++;
            } else {
                if (r.wrong == 0) {
                    r.firstWrongAt = i;
                    r.firstWrongValue = back[i];
                }
                r.wrong++;
            }
        } else {
            (back[i] == canaryValue ? r.beyondCanary : r.beyondOther)++;
        }
    }
    return r;
}

static std::string describe(const Readback& r, uint32_t written, uint32_t beyond) {
    char text[200];
    snprintf(text, sizeof text, "%u of %u words as the kernel computes them, %u still canary, %u wrong%s; beyond them %u of %u canary", r.right,
             written, r.canary, r.wrong, r.wrong ? (" (first at " + std::to_string(r.firstWrongAt) + ": 0x" + [&] {
                                                        char h[12];
                                                        snprintf(h, sizeof h, "%08x", r.firstWrongValue);
                                                        return std::string(h);
                                                    }() + ")")
                                                           .c_str()
                                                     : "",
             r.beyondCanary, beyond);
    return text;
}

static std::string status(const CommandBuffer& cb, Gpu& gpu) {
    return (cb.error().empty() ? "" : " (" + cb.error() + ")") + ", " + std::to_string(cb.gpuNanoseconds() / 1000) + " us, FAULTS " +
           std::to_string(gpu.faults());
}

int main(int argc, char** argv) {
    bool recover = false, bisect = false;
    compute::Options chosen;   // what the dispatches of stage 5 are encoded with
    for (int i = 1; i < argc; i++) {
        recover = recover || strcmp(argv[i], "--recover") == 0;
        bisect = bisect || strcmp(argv[i], "--bisect") == 0;
        // --notail: no end-of-dispatch events and waits of our own; the kernel's own CACHE_FLUSH_TS fence closes the
        // submission (28 October: with them every dispatch hung, without them end_only completed in 48 us)
        if (strcmp(argv[i], "--notail") == 0) chosen.tail = false;
    }
    std::string why;
    auto transport = openKernelGpu(&why);
    if (transport == nullptr) {
        std::printf("probe: no GPU: %s\n", why.c_str());
        return 100;
    }
    Gpu gpu(std::move(transport));
    uint64_t id = 0, chip = 0, freq = 0;
    gpu.param(kParamGpuId, &id);
    gpu.param(kParamChipId, &chip);
    gpu.param(kParamMaxFreq, &freq);
    // (the chip id with the speed bin above it; FAULTS is how many times the kernel has reset a hung GPU in this run)
    std::printf("probe: GPU %llu, chip id 0x%llx, %llu MHz, FAULTS %llu%s\n", (unsigned long long)id, (unsigned long long)chip,
                (unsigned long long)(freq / 1000000), (unsigned long long)gpu.faults(),
                recover ? "; the GPU will be hung on purpose at the end (--recover)" : "");
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

    // 5. compute, in order of hazard: the first shader instructions this driver has ever had the GPU run. The fake GPU
    // checked that the dispatch holds together; only this says whether the GPU runs it, and what it writes.
    {
        const uint32_t words = 1024, c = 0x00001234;
        const uint32_t canary = canaryFor(words, c);
        auto out = newBuffer(gpu, 4 * words, &why);
        auto cbuf = newBuffer(gpu, 64, &why);
        if (out == nullptr || cbuf == nullptr) {
            report(false, "buffers for the dispatches: " + why);
        } else {
            auto prefill = [&] {
                auto* w = static_cast<uint32_t*>(out->contents());
                for (uint32_t i = 0; i < words; i++) w[i] = canary;
            };
            auto run = [&](const Kernel& k, uint32_t groups, const uint32_t* local, const std::vector<Binding>& bufs,
                           const uint32_t* imm) -> std::shared_ptr<CommandBuffer> {
                const uint32_t g[3] = {groups, 1, 1};
                auto cb = queue->commandBuffer();
                cb->dispatch(k, g, local, bufs, imm, chosen);
                cb->commit();
                cb->waitUntilCompleted();
                return cb;
            };
            // 5a. a kernel of one `end`: the smallest CP_EXEC_CS there is. The fence must pass and nothing may be written.
            prefill();
            {
                auto cb = run(kernels::end_only, 1, nullptr, {}, nullptr);
                Readback r = readBack(*out, 0, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.beyondCanary == words,
                       "dispatch of end_only, 1 group: " + std::string(cb->status() == CommandBuffer::kCompleted ? "completed" : "did NOT complete") +
                           ", " + std::to_string(r.beyondCanary) + " of " + std::to_string(words) + " words untouched" + status(*cb, gpu));
            }
            // 5b. fill_id (the text kernel) over 16 groups of 64: every word says which invocation wrote it
            prefill();
            uint32_t imm[4] = {c, 0, 0, 0};
            {
                auto cb = run(kernels::fill_id, 16, nullptr, {Binding{out.get(), 0}}, imm);
                Readback r = readBack(*out, words, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.right == words,
                       "dispatch of fill_id, 16 groups of 64, c = 0x1234: " + describe(r, words, 0) + status(*cb, gpu));
            }
            // 5c. the controls, in the same run: another c moves every word by the same amount; half the grid leaves half
            // the canary; end_only over the same buffer leaves all of it
            {
                uint32_t c2 = c + 0x1000;
                uint32_t imm2[4] = {c2, 0, 0, 0};
                prefill();
                auto cb = run(kernels::fill_id, 16, nullptr, {Binding{out.get(), 0}}, imm2);
                Readback r = readBack(*out, words, c2, canaryFor(words, c2));
                report(cb->status() == CommandBuffer::kCompleted && r.right == words,
                       "control, c = 0x2234: " + describe(r, words, 0) + status(*cb, gpu));
            }
            {
                prefill();
                auto cb = run(kernels::fill_id, 8, nullptr, {Binding{out.get(), 0}}, imm);
                Readback r = readBack(*out, 512, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.right == 512 && r.beyondCanary == 512,
                       "control, 8 groups of the same 1024-word buffer: " + describe(r, 512, 512) + status(*cb, gpu));
            }
            {
                prefill();
                auto cb = run(kernels::end_only, 16, nullptr, {}, nullptr);
                Readback r = readBack(*out, 0, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.beyondCanary == words,
                       "control, end_only over 16 groups: " + std::to_string(r.beyondCanary) + " of " + std::to_string(words) + " words canary" +
                           status(*cb, gpu));
            }
            // 5d. the same kernel compiled from AIR (tools/kernels/fill_id.ll): its threadgroup size comes from the driver
            // params and c from a constant buffer; the buffer must come out the same as 5b's
            {
                const uint32_t local[3] = {64, 1, 1};
                static_cast<uint32_t*>(cbuf->contents())[0] = c;
                prefill();
                auto cb = run(kernels::fill_id_air, 16, local, {Binding{out.get(), 0}, Binding{cbuf.get(), 0}}, nullptr);
                Readback r = readBack(*out, words, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.right == words,
                       "dispatch of fill_id_air (AIR -> NIR -> ir3), 16 groups of 64, c from a constant buffer: " + describe(r, words, 0) +
                           status(*cb, gpu));
                // and its half-grid control, which here changes only the group count: the descriptor covers the whole buffer
                prefill();
                auto cb2 = run(kernels::fill_id_air, 8, local, {Binding{out.get(), 0}, Binding{cbuf.get(), 0}}, nullptr);
                Readback r2 = readBack(*out, 512, c, canary);
                report(cb2->status() == CommandBuffer::kCompleted && r2.right == 512 && r2.beyondCanary == 512,
                       "control, fill_id_air over 8 groups: " + describe(r2, 512, 512) + status(*cb2, gpu));
            }
            // 5e. how long a dispatch takes as the grid grows (fill_id_air: its descriptor is the buffer's size). Sizes that
            // complete, never a search by hanging: the number for a size that did not complete is the error, not a time.
            for (uint32_t groups : {64u, 256u, 1024u, 4096u}) {
                uint32_t n = 64 * groups;
                auto big = newBuffer(gpu, 4ull * n, &why);
                if (big == nullptr) {
                    report(false, "a buffer of " + std::to_string(4ull * n) + " bytes for " + std::to_string(groups) + " groups: " + why);
                    break;
                }
                uint32_t cv = canaryFor(n, c);
                auto* w = static_cast<uint32_t*>(big->contents());
                for (uint32_t i = 0; i < n; i++) w[i] = cv;
                const uint32_t local[3] = {64, 1, 1};
                auto cb = run(kernels::fill_id_air, groups, local, {Binding{big.get(), 0}, Binding{cbuf.get(), 0}}, nullptr);
                Readback r = readBack(*big, n, c, cv);
                bool ok = cb->status() == CommandBuffer::kCompleted && r.right == n;
                report(ok, "fill_id_air over " + std::to_string(groups) + " groups (" + std::to_string(n) + " words): " + describe(r, n, 0) + status(*cb, gpu));
                if (!ok) break;
            }
        }
    }

    // 6. (--bisect) end_only under each part of the dispatch stream switched off in turn, and under none of them: when
    // every dispatch hangs (28 October: HLSQ busy, SP idle, the CP through its IB), this says which part the hang needs,
    // one boot for all of it, each hang costing the kernel one reset. A variant that completes is the finding.
    if (bisect) {
        struct Variant {
            const char* name;
            compute::Options o;
        };
        compute::Options minimal;
        minimal.prelude = minimal.invalidateCmd = minimal.hlsqCopies = minimal.pvtmemZero = minimal.invalidate = minimal.marker = minimal.tail = false;
        std::vector<Variant> variants = {{"the whole stream", compute::Options{}}};
        auto without = [&](const char* name, void (*off)(compute::Options&)) {
            compute::Options o;
            off(o);
            variants.push_back(Variant{name, o});
        };
        // 28 October, first round: every variant with the end hung, both without it completed. So the end it is; now its parts.
        without("without the end (RB_DONE_TS, CACHE_FLUSH_TS and their waits)", [](compute::Options& o) { o.tail = false; });
        without("with the end, the TIMESTAMP bit off (computerator's form)", [](compute::Options& o) { o.tailTimestampBit = false; });
        without("with only RB_DONE_TS and its wait", [](compute::Options& o) { o.tailCacheFlush = false; });
        without("with only CACHE_FLUSH_TS and its wait", [](compute::Options& o) { o.tailRbDone = false; });
        without("with only CACHE_FLUSH_TS and its wait, TIMESTAMP bit off", [](compute::Options& o) {
            o.tailRbDone = false;
            o.tailTimestampBit = false;
        });
        without("with only RB_DONE_TS and its wait, TIMESTAMP bit off", [](compute::Options& o) {
            o.tailCacheFlush = false;
            o.tailTimestampBit = false;
        });
        variants.push_back(Variant{"with none of those: the registers, the loads, NDRANGE, CP_EXEC_CS, CP_WAIT_FOR_IDLE", minimal});
        const uint32_t one[3] = {1, 1, 1};
        for (const Variant& v : variants) {
            auto cb = queue->commandBuffer();
            cb->dispatch(kernels::end_only, one, nullptr, {}, nullptr, v.o);
            cb->commit();
            cb->waitUntilCompleted();
            bool ok = cb->status() == CommandBuffer::kCompleted;
            report(ok, std::string("bisect: end_only ") + v.name + ": " + (ok ? "COMPLETED" : "hung") + status(*cb, gpu));
        }
    }

    // 7. (--recover) the GPU is hung on purpose, and the kernel must bring it back. The hang is the measured one:
    // 4 Mi words in one CP_MEMCPY stopped the command processor on 27 October (125 000 took 4.2 ms), and the run and
    // the system with it. With boot-arg gpu_recover=1 the kernel resets the core when a wait of a second or more
    // comes to nothing, FAULTS rises by one, and the next command buffer is the core's own again. This goes last,
    // because without that boot-arg it is the end of the run; the line says which it was.
    if (recover) {
        auto big1 = newBuffer(gpu, 16 * mb, &why), big2 = newBuffer(gpu, 16 * mb, &why);
        if (big1 && big2) {
            memset(b->contents(), 0x33, 4096);
            uint64_t before = gpu.faults();
            pm4::Stream s;
            s.memcpy(big2->gpuAddress(), big1->gpuAddress(), 4u << 20);
            s.waitForIdle();
            int e = 0;
            auto commands = gpu.newBo(4096, kBoGpuReadOnly | kBoWriteCombined, &e);
            memcpy(commands->cpu(), s.words().data(), 4 * s.words().size());
            uint32_t fence = 0;
            int64_t began = gpu.transport().nowNs();
            int sub = gpu.submit(0, *commands, 0, uint32_t(s.words().size()), {BoUse{&big1->bo(), false}, BoUse{&big2->bo(), true}},
                                 &fence);
            int w = sub == 0 ? gpu.wait(0, fence, 2000000000) : -1;   // 2 s: over the kernel's one-second line
            int64_t took = (gpu.transport().nowNs() - began) / 1000000;
            uint64_t after = gpu.faults();
            if (sub != 0) {
                report(false, "hang on purpose: the submission was refused: " + std::string(strerror(sub)));
            } else if (w == 0) {
                report(false, "hang on purpose: 4 Mi words in one CP_MEMCPY COMPLETED in " + std::to_string(took) +
                                  " ms; the 27 October hang did not reproduce, so nothing here tests the recovery");
            } else {
                report(after == before + 1, "hang on purpose: the wait ended with " + std::string(strerror(w)) + " after " +
                                                std::to_string(took) + " ms, FAULTS " + std::to_string(before) + " -> " +
                                                std::to_string(after) +
                                                (after == before + 1 ? " (the kernel reset the GPU)"
                                                 : after == before ? " (NO reset: gpu_recover=1 missing, or recovery failed)"
                                                                   : " (MORE than one reset)"));
            }
            // and after it: a command buffer committed now must run, and its bytes must land
            auto cb = queue->commandBuffer();
            cb->fill(*b, 0, 4096, 0xc3);
            cb->commit();
            cb->waitUntilCompleted();
            std::vector<uint8_t> back(4096);
            memcpy(back.data(), b->contents(), 4096);
            bool ok = cb->status() == CommandBuffer::kCompleted;
            for (size_t i = 0; i < 4096; i++) ok = ok && back[i] == 0xc3;
            report(ok, "after the hang: a fill of 4096 bytes " + std::string(ok ? "ran and landed" : "did NOT land") +
                           (cb->error().empty() ? "" : " (" + cb->error() + ")") + ", FAULTS now " + std::to_string(gpu.faults()));
        } else {
            report(false, "two buffers of 16 MB for the hang: " + why);
        }
    }

    std::printf("probe: %d failure%s\n", failures, failures == 1 ? "" : "s");
    return failures;
}
