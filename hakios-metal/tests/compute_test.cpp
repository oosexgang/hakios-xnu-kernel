// The compute dispatch against the fake GPU, which checks that its state holds together and does NOT run the kernel: a
// test here proves the stream is well formed and names only memory the GPU was given; what the kernel writes is the
// phone's to prove (hakios-metal-probe). The second half shows the fake can fail, one fault class at a time, by
// mutating a dispatch it accepts into one it must refuse.
#include <cerrno>
#include <cstring>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "fixtures/end_only.hpp"
#include "fixtures/fill_id.hpp"
#include "fixtures/fill_id_air.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {
struct Rig {
    Rig() {
        auto f = std::make_unique<FakeGpu>();
        fake = f.get();
        gpu = std::make_unique<Gpu>(std::move(f));
        std::string why;
        queue = CommandQueue::make(*gpu, &why);
    }
    FakeGpu* fake;
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<CommandQueue> queue;
};

std::shared_ptr<Buffer> buffer(Rig& r, uint64_t n, uint32_t fill) {
    std::string why;
    auto b = newBuffer(*r.gpu, n, &why);
    CHECK(b != nullptr);
    auto* w = static_cast<uint32_t*>(b->contents());
    for (uint64_t i = 0; i < n / 4; i++) w[i] = fill;
    return b;
}

bool allWords(const Buffer& b, uint32_t value) {
    auto* w = static_cast<const uint32_t*>(b.contents());
    for (uint64_t i = 0; i < b.length() / 4; i++) {
        if (w[i] != value) return false;
    }
    return true;
}

// a stream built by hand, run through the raw path: what the fake says about it
struct Raw {
    explicit Raw(Rig& r) : rig(r) {
        int e = 0;
        bo = r.gpu->newBo(65536, 0, &e);
        CHECK(bo != nullptr);
    }
    int submit(const std::vector<uint32_t>& words, const std::vector<BoUse>& uses = {}) {
        memcpy(bo->cpu(), words.data(), 4 * words.size());
        uint32_t fence = 0;
        int e = rig.gpu->submit(0, *bo, 0, uint32_t(words.size()), uses, &fence);
        if (e != 0) return e;
        return rig.gpu->wait(0, fence, 1000000000);
    }
    Rig& rig;
    std::unique_ptr<Bo> bo;
};

// the dispatch's own words, as compute::encode writes them, for a test to mutate
struct Encoded {
    Encoded(Rig& r, const Kernel& k, const uint32_t groups[3], const uint32_t* local, const std::vector<Binding>& buffers) {
        std::string why;
        program = newBuffer(*r.gpu, 4ull * k.numWords, &why);
        table = newBuffer(*r.gpu, compute::kTableBytes, &why);
        CHECK(program != nullptr && table != nullptr);
        memcpy(program->contents(), k.words, 4ull * k.numWords);
        uint32_t loc[3] = {k.localSize[0], k.localSize[1], k.localSize[2]};
        if (local) for (int i = 0; i < 3; i++) loc[i] = local[i];
        ok = compute::encode(stream, k, groups, loc, buffers, nullptr, program->gpuAddress(), table->gpuAddress(),
                             static_cast<uint8_t*>(table->contents()), &why);
        error = why;
    }
    // the index of the first word that is a type-7 header of OPCODE
    size_t find(uint32_t opcode) const {
        const auto& w = stream.words();
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kCommand && h.opcode == opcode) return i;
            i += 1 + h.count;
        }
        return size_t(-1);
    }
    std::shared_ptr<Buffer> program, table;
    pm4::Stream stream;
    bool ok = false;
    std::string error;
};

const uint32_t kGroups16[3] = {16, 1, 1};
}  // namespace

TEST(a_dispatch_of_fill_id_holds_together_and_the_fake_does_not_run_it) {
    Rig r;
    auto out = buffer(r, 4096, 0xcccccccc);
    uint32_t c[4] = {0x5678, 0, 0, 0};
    auto cb = r.queue->commandBuffer();
    CHECK(cb->dispatch(kernels::fill_id, kGroups16, nullptr, {Binding{out.get(), 0}}, c));
    CHECK(cb->error().empty());
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    FakeGpu::Stats s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    CHECK_EQ(s.dispatches, 1u);
    CHECK_EQ(s.lastDispatch.groups[0], 16u);
    CHECK_EQ(s.lastDispatch.groups[1], 1u);
    CHECK_EQ(s.lastDispatch.local[0], 64u);
    CHECK_EQ(s.lastDispatch.instrlen, 1u);
    CHECK_EQ(s.lastDispatch.constlen, 4u);
    CHECK_EQ(s.lastDispatch.ibos, 1u);
    CHECK_EQ(s.lastDispatch.consts.size(), size_t(1));   // the immediates, one vec4 at 0
    CHECK_EQ(s.lastDispatch.consts[0].dstOffVec4, 0u);
    CHECK_EQ(s.lastDispatch.consts[0].units, 1u);
    CHECK(s.lastDispatch.consts[0].direct);
    // the fake has no shader interpreter: it says so here, so that nobody reads "completed" as "computed"
    CHECK(allWords(*out, 0xcccccccc));
}

TEST(a_dispatch_of_end_only_needs_no_buffers) {
    Rig r;
    const uint32_t one[3] = {1, 1, 1};
    auto cb = r.queue->commandBuffer();
    CHECK(cb->dispatch(kernels::end_only, one, nullptr, {}));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().faults, 0u);
    CHECK_EQ(r.fake->stats().dispatches, 1u);
    CHECK_EQ(r.fake->stats().lastDispatch.ibos, 0u);
}

TEST(a_dispatch_of_the_air_kernel_uploads_what_ir3_asks_for) {
    // the same kernel compiled from AIR: its threadgroup size is a dispatch argument it reads from the driver params,
    // its constant c is a UBO range ir3 pushed into the constant file, and its own immediate (K) sits above both
    Rig r;
    auto out = buffer(r, 4096, 0xcccccccc);
    auto c = buffer(r, 64, 0x5678);
    const uint32_t local[3] = {64, 1, 1};
    auto cb = r.queue->commandBuffer();
    CHECK(cb->dispatch(kernels::fill_id_air, kGroups16, local, {Binding{out.get(), 0}, Binding{c.get(), 0}}));
    CHECK(cb->error().empty());
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    FakeGpu::Stats s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    CHECK_EQ(s.dispatches, 1u);
    CHECK_EQ(s.lastDispatch.local[0], 64u);
    CHECK_EQ(s.lastDispatch.constlen, 12u);
    CHECK_EQ(s.lastDispatch.ibos, 1u);
    CHECK_EQ(s.lastDispatch.consts.size(), size_t(3));
    bool immediates = false, params = false, range = false;
    for (const auto& l : s.lastDispatch.consts) {
        immediates = immediates || (l.dstOffVec4 == 8 && l.units == 1 && l.direct);
        params = params || (l.dstOffVec4 == 0 && l.units == 4 && l.direct);
        range = range || (l.dstOffVec4 == 4 && l.units == 4 && !l.direct);
    }
    CHECK(immediates);
    CHECK(params);
    CHECK(range);
    CHECK(allWords(*out, 0xcccccccc));
}

TEST(a_dispatch_that_does_not_fit_is_refused_before_anything_reaches_the_gpu) {
    Rig r;
    auto small = buffer(r, 2048, 0);
    auto out = buffer(r, 4096, 0);
    {
        auto cb = r.queue->commandBuffer();
        CHECK(!cb->dispatch(kernels::fill_id, kGroups16, nullptr, {Binding{small.get(), 0}}));
        CHECK(cb->error().find("4096 bytes; 2048 are bound") != std::string::npos);
    }
    {
        auto cb = r.queue->commandBuffer();
        CHECK(!cb->dispatch(kernels::fill_id, kGroups16, nullptr, {}));
        CHECK(cb->error().find("no buffer bound at index 0") != std::string::npos);
    }
    {
        const uint32_t other[3] = {32, 1, 1};
        auto cb = r.queue->commandBuffer();
        CHECK(!cb->dispatch(kernels::fill_id, kGroups16, other, {Binding{out.get(), 0}}));
        CHECK(cb->error().find("fixes its threadgroup at 64x1x1") != std::string::npos);
    }
    {
        const uint32_t none[3] = {0, 1, 1};
        auto cb = r.queue->commandBuffer();
        CHECK(!cb->dispatch(kernels::fill_id, none, nullptr, {Binding{out.get(), 0}}));
        CHECK(cb->error().find("0 work groups") != std::string::npos);
    }
    {
        const uint32_t tooMany[3] = {1025, 1, 1};
        auto cb = r.queue->commandBuffer();
        CHECK(!cb->dispatch(kernels::fill_id_air, kGroups16, tooMany, {Binding{out.get(), 0}, Binding{out.get(), 0}}));
        CHECK(cb->error().find("1 to 1024") != std::string::npos);
    }
    {
        const uint32_t local[3] = {64, 1, 1};
        auto cb = r.queue->commandBuffer();   // the AIR kernel's constant buffer is index 1: unbound
        CHECK(!cb->dispatch(kernels::fill_id_air, kGroups16, local, {Binding{out.get(), 0}}));
        CHECK(cb->error().find("no buffer bound at index 1") != std::string::npos);
    }
    CHECK_EQ(r.fake->stats().submits, 0u);
}

// --- the fake can fail: one class of fault at a time -------------------------------------------------------------------

TEST(the_fake_refuses_a_program_preload_of_128_units) {
    Rig r;
    Raw raw(r);
    using namespace a6xx;
    uint32_t w0 = loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_CS_SHADER, 128);
    pm4::Stream s;
    uint32_t words[3] = {w0, uint32_t(raw.bo->gpuAddress()), uint32_t(raw.bo->gpuAddress() >> 32)};
    s.packet(CP_LOAD_STATE6_FRAG, words, 3);
    CHECK_EQ(raw.submit(s.words()), ETIMEDOUT);
    CHECK_EQ(r.fake->stats().faults, 1u);
    CHECK(r.fake->stats().lastFault.find("128 hangs this GPU") != std::string::npos);
}

TEST(the_fake_refuses_exec_cs_without_a_program) {
    Rig r;
    Raw raw(r);
    pm4::Stream s;
    uint32_t words[4] = {0, 1, 1, 1};
    s.packet(a6xx::CP_EXEC_CS, words, 4);
    CHECK_EQ(raw.submit(s.words()), ETIMEDOUT);
    CHECK(r.fake->stats().lastFault.find("not enabled") != std::string::npos);
}

TEST(the_fake_refuses_a_grid_that_does_not_match_the_ndrange) {
    Rig r;
    auto out = buffer(r, 4096, 0);
    Encoded e(r, kernels::fill_id, kGroups16, nullptr, {Binding{out.get(), 0}});
    CHECK(e.ok);
    std::vector<uint32_t> words = e.stream.words();
    size_t exec = e.find(a6xx::CP_EXEC_CS);
    CHECK(exec != size_t(-1));
    words[exec + 2] = 8;   // NGROUPS_X: half the grid the NDRANGE registers were written for
    Raw raw(r);
    CHECK_EQ(raw.submit(words, {BoUse{&e.program->bo(), false}, BoUse{&e.table->bo(), true}, BoUse{&out->bo(), true}}), ETIMEDOUT);
    CHECK(r.fake->stats().lastFault.find("global size 1024 is not the local size 64 times the 8 groups") != std::string::npos);
}

TEST(the_fake_refuses_an_ibo_outside_every_buffer) {
    Rig r;
    auto out = buffer(r, 4096, 0);
    Encoded e(r, kernels::fill_id, kGroups16, nullptr, {Binding{out.get(), 0}});
    CHECK(e.ok);
    // the descriptor's base address, moved to where no buffer is
    auto* table = static_cast<uint32_t*>(e.table->contents());
    table[4] = 0xfff00000u;
    table[5] = 0x7u;
    Raw raw(r);
    CHECK_EQ(raw.submit(e.stream.words()), ETIMEDOUT);
    CHECK(r.fake->stats().lastFault.find("IBO 0") != std::string::npos);
    CHECK(r.fake->stats().lastFault.find("outside every buffer") != std::string::npos);
}

TEST(the_fake_refuses_constants_past_constlen) {
    Rig r;
    auto out = buffer(r, 4096, 0);
    Encoded e(r, kernels::fill_id, kGroups16, nullptr, {Binding{out.get(), 0}});
    CHECK(e.ok);
    std::vector<uint32_t> words = e.stream.words();
    // the second CP_LOAD_STATE6_FRAG is the immediates (the first is the program): move them to vec4 4 of a constlen of 4
    size_t first = e.find(a6xx::CP_LOAD_STATE6_FRAG);
    size_t consts = first + 1 + pm4::decode(words[first]).count;
    while (pm4::decode(words[consts]).kind != pm4::Header::kCommand || pm4::decode(words[consts]).opcode != a6xx::CP_LOAD_STATE6_FRAG) {
        consts += 1 + pm4::decode(words[consts]).count;
    }
    words[consts + 1] = (words[consts + 1] & ~a6xx::CP_LOAD_STATE6_0_DST_OFF__MASK) | a6xx::CP_LOAD_STATE6_0_DST_OFF(4);
    Raw raw(r);
    CHECK_EQ(raw.submit(words), ETIMEDOUT);
    CHECK(r.fake->stats().lastFault.find("past the program's constlen of 4") != std::string::npos);
}

TEST(the_fake_refuses_a_wait_that_nothing_satisfies) {
    Rig r;
    Raw raw(r);
    using namespace a6xx;
    uint64_t word = raw.bo->gpuAddress() + 8192;   // a zero word in the command buffer's own memory
    pm4::Stream s;
    uint32_t w[6] = {waitRegMem0(WRITE_EQ, POLL_MEMORY), uint32_t(word), uint32_t(word >> 32), 1, ~0u, kDelayLoopCycles};
    s.packet(CP_WAIT_REG_MEM, w, 6);
    CHECK_EQ(raw.submit(s.words()), ETIMEDOUT);
    CHECK(r.fake->stats().lastFault.find("would wait forever") != std::string::npos);
    // and the same wait after the event that writes the word: satisfied
    Rig r2;
    Raw raw2(r2);
    uint64_t word2 = raw2.bo->gpuAddress() + 8192;
    pm4::Stream s2;
    uint32_t ev[4] = {eventWrite0(RB_DONE_TS, true), uint32_t(word2), uint32_t(word2 >> 32), 1};
    s2.packet(CP_EVENT_WRITE, ev, 4);
    uint32_t w2[6] = {waitRegMem0(WRITE_EQ, POLL_MEMORY), uint32_t(word2), uint32_t(word2 >> 32), 1, ~0u, kDelayLoopCycles};
    s2.packet(CP_WAIT_REG_MEM, w2, 6);
    CHECK_EQ(raw2.submit(s2.words()), 0);
    CHECK_EQ(r2.fake->stats().faults, 0u);
}

TEST(the_fake_refuses_id_registers_whose_two_copies_differ) {
    Rig r;
    auto out = buffer(r, 4096, 0);
    Encoded e(r, kernels::fill_id, kGroups16, nullptr, {Binding{out.get(), 0}});
    CHECK(e.ok);
    std::vector<uint32_t> words = e.stream.words();
    // find the SP_CS_CNTL_0 write and change its local id register
    bool changed = false;
    for (size_t i = 0; i < words.size();) {
        pm4::Header h = pm4::decode(words[i]);
        if (h.kind == pm4::Header::kRegisters && h.reg == a6xx::REG_A6XX_SP_CS_CNTL_0) {
            words[i + 1] ^= a6xx::A6XX_SP_CS_CNTL_0_LOCALIDREGID(0x4);
            changed = true;
        }
        i += 1 + h.count;
    }
    CHECK(changed);
    Raw raw(r);
    CHECK_EQ(raw.submit(words), ETIMEDOUT);
    CHECK(r.fake->stats().lastFault.find("copies of the id registers") != std::string::npos);
}
