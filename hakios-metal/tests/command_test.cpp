// The Metal model against the fake GPU, which runs the packets: what a blit leaves in memory, the order of
// completion, the states, and the refusals.
#include <atomic>
#include <cstring>
#include <random>

#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {
struct Rig {
    explicit Rig(bool async = false, uint32_t delayUs = 0) {
        auto f = std::make_unique<FakeGpu>(async, delayUs);
        fake = f.get();
        gpu = std::make_unique<Gpu>(std::move(f));
        std::string why;
        queue = CommandQueue::make(*gpu, &why);
    }
    ~Rig() {
        queue.reset();
    }
    FakeGpu* fake;
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<CommandQueue> queue;
};

std::shared_ptr<Buffer> buffer(Rig& r, uint64_t n) {
    std::string why;
    auto b = newBuffer(*r.gpu, n, &why);
    CHECK(b != nullptr);
    return b;
}
}  // namespace

TEST(gpu_params) {
    Rig r;
    uint64_t id = 0;
    CHECK_EQ(r.gpu->param(kParamGpuId, &id), 0);
    CHECK_EQ(id, 660ull);
}

TEST(copy_moves_the_bytes) {
    Rig r;
    auto a = buffer(r, 1 << 20), b = buffer(r, 1 << 20);
    std::mt19937 rng(7);
    auto* pa = static_cast<uint32_t*>(a->contents());
    for (size_t i = 0; i < (1 << 18); i++) pa[i] = rng();
    memset(b->contents(), 0, 1 << 20);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->copy(*a, 4096, *b, 8192, 500000));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK(memcmp(static_cast<uint8_t*>(b->contents()) + 8192, static_cast<uint8_t*>(a->contents()) + 4096, 500000) == 0);
    // nothing before or after the copied range was touched
    auto* pb = static_cast<uint8_t*>(b->contents());
    bool clean = true;
    for (size_t i = 0; i < 8192; i++) clean = clean && pb[i] == 0;
    for (size_t i = 8192 + 500000; i < (1 << 20); i++) clean = clean && pb[i] == 0;
    CHECK(clean);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

TEST(fill_any_length) {
    Rig r;
    auto b = buffer(r, 1 << 20);
    for (uint64_t len : {4ull, 64ull, 256ull, 260ull, 4096ull, 65540ull, 999996ull}) {
        memset(b->contents(), 0x11, 1 << 20);
        auto cb = r.queue->commandBuffer();
        CHECK(cb->fill(*b, 16, len, 0xa5));
        cb->commit();
        cb->waitUntilCompleted();
        auto* p = static_cast<uint8_t*>(b->contents());
        bool ok = p[15] == 0x11 && p[16 + len] == 0x11;
        for (uint64_t i = 16; i < 16 + len; i++) ok = ok && p[i] == 0xa5;
        CHECK(ok);
    }
    CHECK_EQ(r.fake->stats().faults, 0u);
}

TEST(copy_then_fill_in_one_buffer_keeps_order) {
    Rig r;
    auto a = buffer(r, 4096), b = buffer(r, 4096);
    memset(a->contents(), 0x5a, 4096);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->fill(*b, 0, 4096, 0x00));
    CHECK(cb->copy(*a, 0, *b, 1024, 1024));
    CHECK(cb->fill(*b, 1536, 256, 0xff));
    cb->commit();
    cb->waitUntilCompleted();
    auto* p = static_cast<uint8_t*>(b->contents());
    CHECK_EQ(p[1023], 0x00);
    CHECK_EQ(p[1024], 0x5a);
    CHECK_EQ(p[1535], 0x5a);
    CHECK_EQ(p[1536], 0xff);
    CHECK_EQ(p[1791], 0xff);
    CHECK_EQ(p[1792], 0x5a);
    CHECK_EQ(p[2048], 0x00);
}

TEST(unaligned_blits_are_refused_and_the_buffer_errs) {
    Rig r;
    auto a = buffer(r, 4096), b = buffer(r, 4096);
    auto cb = r.queue->commandBuffer();
    CHECK(!cb->copy(*a, 1, *b, 0, 16));
    std::atomic<int> handled{0};
    cb->addCompletedHandler([&](CommandBuffer& c) { handled += c.status() == CommandBuffer::kError ? 1 : 100; });
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kError);
    CHECK(!cb->error().empty());
    CHECK_EQ(handled.load(), 1);
    CHECK_EQ(r.fake->stats().submits, 0u);    // nothing of it reached the GPU
    auto cb2 = r.queue->commandBuffer();
    CHECK(!cb2->copy(*a, 0, *b, 4000, 200));  // past the end
    CHECK(!cb2->fill(*b, 2, 4, 0));
}

TEST(handlers_run_in_commit_order_while_the_gpu_is_behind) {
    Rig r(true, 200);
    auto b = buffer(r, 65536);
    std::vector<int> order;
    std::mutex m;
    std::vector<std::shared_ptr<CommandBuffer>> all;
    r.fake->pause();
    for (int i = 0; i < 40; i++) {
        auto cb = r.queue->commandBuffer();
        cb->fill(*b, 4 * i, 4, uint8_t(i));
        cb->addCompletedHandler([&, i](CommandBuffer&) {
            std::lock_guard<std::mutex> g(m);
            order.push_back(i);
        });
        cb->commit();
        CHECK(cb->status() == CommandBuffer::kScheduled);   // committed and handed over, the GPU not done
        all.push_back(cb);
    }
    r.fake->resume();
    all.back()->waitUntilCompleted();
    // the last one done means every one before it is done, with its handler run
    for (auto& cb : all) cb->waitUntilCompleted();
    CHECK_EQ(order.size(), size_t(40));
    bool inOrder = true;
    for (int i = 0; i < int(order.size()); i++) inOrder = inOrder && order[i] == i;
    CHECK(inOrder);
    auto* p = static_cast<uint8_t*>(b->contents());
    CHECK_EQ(p[4 * 39], 39);
}

TEST(command_memory_is_used_again) {
    Rig r(true);
    auto b = buffer(r, 4096);
    for (int i = 0; i < 100; i++) {
        auto cb = r.queue->commandBuffer();
        cb->fill(*b, 0, 4096, uint8_t(i));
        cb->commit();
        cb->waitUntilCompleted();
    }
    // one command stream's buffer, taken back after every completion: not a hundred of them
    CHECK(r.queue->idleCommandBuffers() <= 2);
    CHECK(r.fake->liveBuffers() <= 4);
}

TEST(buffers_are_given_back) {
    Rig r;
    {
        auto a = buffer(r, 1 << 16);
        auto b = buffer(r, 1 << 16);
        CHECK(r.fake->liveBuffers() >= 2);
    }
    CHECK_EQ(r.fake->liveBuffers(), 0u);
}

TEST(a_bad_stream_is_a_fault_not_a_crash) {
    // what the fake GPU does with a packet the real one would hang on: counted, and the fence still passes
    Rig r;
    int e = 0;
    auto bo = r.gpu->newBo(4096, 0, &e);
    auto* w = static_cast<uint32_t*>(bo->cpu());
    w[0] = hakios::metal::pm4::type7(0x7f, 0) ^ (1u << 15);   // parity broken
    uint32_t fence = 0;
    CHECK_EQ(r.gpu->submit(0, *bo, 0, 1, {}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(r.fake->stats().faults, 1u);
}

TEST(big_copies_are_cut_into_packets_the_gpu_takes) {
    Rig r;
    auto a = buffer(r, 16 << 20), b = buffer(r, 16 << 20);
    auto* pa = static_cast<uint32_t*>(a->contents());
    for (size_t i = 0; i < (4u << 20); i++) pa[i] = uint32_t(i * 2654435761u);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->copy(*a, 0, *b, 0, 16 << 20));
    CHECK(cb->fill(*a, 0, 16 << 20, 0x77));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(static_cast<uint32_t*>(b->contents())[(4u << 20) - 1], uint32_t(((4u << 20) - 1) * 2654435761u));
    CHECK_EQ(static_cast<uint8_t*>(a->contents())[(16 << 20) - 1], 0x77);
    CHECK_EQ(r.fake->stats().faults, 0u);
}
