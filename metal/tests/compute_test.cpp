// The compute dispatch against the fake GPU, which checks that its state holds together and does NOT run the kernel: a
// test here proves the stream is well formed and names only memory the GPU was given; what the kernel writes is the
// phone's to prove (hakios-metal-probe). The second half shows the fake can fail, one fault class at a time, by
// mutating a dispatch it accepts into one it must refuse.
#include <cerrno>
#include <cstring>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/texture.hpp"
#include "fixtures/end_only.hpp"
#include "fixtures/fill_id.hpp"
#include "fixtures/fill_id_air.hpp"
#include "fixtures/chain160_air.hpp"
#include "fixtures/long_chain_air.hpp"
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
    Encoded(Rig& r, const Kernel& k, const uint32_t groups[3], const uint32_t* local, const std::vector<Binding>& buffers)
        : Encoded(r, k, groups, local, Bindings{buffers, {}, {}, {}}, compute::Options{}) {}
    Encoded(Rig& r, const Kernel& k, const uint32_t groups[3], const uint32_t* local, const Bindings& bindings,
            const compute::Options& options = compute::Options{}) {
        std::string why;
        program = newBuffer(*r.gpu, 4ull * k.numWords, &why);
        table = newBuffer(*r.gpu, compute::kTableBytes, &why);
        CHECK(program != nullptr && table != nullptr);
        memcpy(program->contents(), k.words, 4ull * k.numWords);
        uint32_t loc[3] = {k.localSize[0], k.localSize[1], k.localSize[2]};
        if (local) for (int i = 0; i < 3; i++) loc[i] = local[i];
        ok = compute::encode(stream, k, groups, loc, bindings, nullptr, program->gpuAddress(), table->gpuAddress(),
                             static_cast<uint8_t*>(table->contents()), &why, options);
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
    // the index of the VALUE word of the first type-4 write that covers register REG
    size_t findReg(uint32_t reg) const {
        const auto& w = stream.words();
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kRegisters && reg >= h.reg && reg < h.reg + h.count) return i + 1 + (reg - h.reg);
            i += 1 + h.count;
        }
        return size_t(-1);
    }
    // the index of the first CP_LOAD_STATE6_FRAG header whose word 0 names TYPE into BLOCK
    size_t findLoad(a6xx::a6xx_state_type type, a6xx::a6xx_state_block block) const {
        const auto& w = stream.words();
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kCommand && h.opcode == a6xx::CP_LOAD_STATE6_FRAG && h.count >= 1) {
                uint32_t w0 = w[i + 1];
                if (((w0 & a6xx::CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> a6xx::CP_LOAD_STATE6_0_STATE_TYPE__SHIFT) == type &&
                    ((w0 & a6xx::CP_LOAD_STATE6_0_STATE_BLOCK__MASK) >> a6xx::CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT) == block) {
                    return i;
                }
            }
            i += 1 + h.count;
        }
        return size_t(-1);
    }
    uint32_t* tableWords() const { return static_cast<uint32_t*>(table->contents()); }
    std::shared_ptr<Buffer> program, table;
    pm4::Stream stream;
    bool ok = false;
    std::string error;
};

const uint32_t kGroups16[3] = {16, 1, 1};
}  // namespace

// A kernel that spills (pvtmemSize) gets the queue's private memory in its CS registers as a draw's stage does, and
// threadgroup memory the encoder sets (Bindings::sharedBytes) raises SHARED_SIZE past the kernel's own. The twins: no
// private memory bound, or too little, and more threadgroup memory than a group has, are refused before any word.
TEST(a_dispatch_gets_private_memory_and_the_threadgroup_memory_its_encoder_sets) {
    using namespace a6xx;
    Rig r;
    auto out = buffer(r, 4096, 0);
    Kernel k = kernels::fill_id;
    k.pvtmemSize = 64;
    k.pvtmemPerWave = false;   // (the layout the queue's memory is asked for below; a wave layout is the twin of that)
    Bindings b{{Binding{out.get(), 0}}, {}, {}, {}};
    {
        Encoded e(r, k, kGroups16, nullptr, b);
        CHECK(!e.ok);
        CHECK(e.error.find("fill_id wants 64 bytes of private memory a fiber: the private memory bound has 0 at 0") != std::string::npos);
        Bindings small = b;
        small.privateMemory = PrivateMemory{0x100000, 32, 4096, false};
        Encoded f(r, k, kGroups16, nullptr, small);
        CHECK(!f.ok);
    }
    PrivateMemory pm;
    std::string why;
    auto scratch = r.queue->privateMemory(64, false, &pm, &why);
    CHECK(scratch != nullptr);
    Bindings withPm = b;
    withPm.privateMemory = pm;
    Encoded e(r, k, kGroups16, nullptr, withPm);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    auto at = [&](uint32_t reg) { size_t i = e.findReg(reg); return i == size_t(-1) ? 0xdeadbeefu : e.stream.words()[i]; };
    CHECK_EQ(at(REG_A6XX_SP_CS_PVT_MEM_PARAM), A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM(512));
    CHECK_EQ(at(REG_A6XX_SP_CS_PVT_MEM_PARAM + 1), uint32_t(scratch->gpuAddress()));
    CHECK_EQ(at(REG_A6XX_SP_CS_PVT_MEM_PARAM + 3), (512u * 4096u) >> 12);
    CHECK_EQ(at(REG_A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET), (512u * 4096u) >> 11);
    // through a command buffer the queue's own is bound, and the fake takes the dispatch
    auto cb = r.queue->commandBuffer();
    CHECK(cb->dispatch(k, kGroups16, nullptr, {Binding{out.get(), 0}}));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    // threadgroup memory: the kernel's own (none here: the 2 KiB floor, field 1), then 8 KiB set by the encoder (field
    // 7, (8192 - 1) / 1024) in both copies, then more than 32 KiB refused
    const auto sharedField = [](uint32_t v) { return (v & A6XX_SP_CS_CTRL_REG1_SHARED_SIZE__MASK) >> A6XX_SP_CS_CTRL_REG1_SHARED_SIZE__SHIFT; };
    Encoded own(r, kernels::fill_id, kGroups16, nullptr, b);
    CHECK_EQ(sharedField(own.stream.words()[own.findReg(REG_A6XX_SP_CS_CTRL_REG1)]), 1u);
    Bindings tg = b;
    tg.sharedBytes = 8192;
    Encoded big(r, kernels::fill_id, kGroups16, nullptr, tg);
    CHECK(big.ok);
    CHECK_EQ(sharedField(big.stream.words()[big.findReg(REG_A6XX_SP_CS_CTRL_REG1)]), 7u);
    CHECK_EQ(sharedField(big.stream.words()[big.findReg(REG_A6XX_HLSQ_CS_CTRL_REG1)]), 7u);
    tg.sharedBytes = 32 * 1024 + 1;
    Encoded over(r, kernels::fill_id, kGroups16, nullptr, tg);
    CHECK(!over.ok);
    CHECK(over.error.find("32769 bytes of threadgroup memory: a group has 32768") != std::string::npos);
}

// dispatchThreadgroupsWithIndirectBuffer:: the counts are read from memory when the GPU runs the dispatch. The twin
// that shows it: they are changed after the dispatch is recorded and before it is committed, and the dispatch runs the
// new ones. The driver params' first vec4 is then loaded from the table's copy (the last load into vec4 0 is the
// indirect one), so a kernel reading its grid sees the same counts; an offset Metal does not allow is refused.
TEST(an_indirect_dispatch_reads_its_counts_when_it_runs) {
    Rig r;
    auto out = buffer(r, 4096, 0);
    auto args = buffer(r, 64, 0);
    uint32_t* a = static_cast<uint32_t*>(args->contents());
    a[1] = 5;   // at offset 4: off 16 bytes, so the copy is what makes the load possible
    a[2] = 2;
    a[3] = 1;
    const uint32_t local[3] = {8, 8, 1};
    auto c = buffer(r, 64, 0x1234);
    Bindings b{{Binding{out.get(), 0}, Binding{c.get(), 0}}, {}, {}, {}};
    auto cb = r.queue->commandBuffer();
    CHECK(cb->dispatchIndirect(kernels::fill_id_air, *args, 4, local, b));
    CHECK(cb->error().empty());
    if (!cb->error().empty()) std::printf("       %s\n", cb->error().c_str());
    a[1] = 7;   // after recording: the GPU reads this one
    a[2] = 3;
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    FakeGpu::Stats s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    if (s.faults != 0) std::printf("       %s\n", s.lastFault.c_str());
    CHECK_EQ(s.lastDispatch.groups[0], 7u);
    CHECK_EQ(s.lastDispatch.groups[1], 3u);
    CHECK_EQ(s.lastDispatch.groups[2], 1u);
    bool lastIsIndirect = false;
    for (const auto& l : s.lastDispatch.consts) {
        if (l.dstOffVec4 == 0) lastIsIndirect = !l.direct && l.units == 1;
    }
    CHECK(lastIsIndirect);
    // the refusals: an offset off 4, counts that run past the buffer
    auto bad = r.queue->commandBuffer();
    CHECK(!bad->dispatchIndirect(kernels::fill_id_air, *args, 2, local, b));
    auto past = r.queue->commandBuffer();
    CHECK(!past->dispatchIndirect(kernels::fill_id_air, *args, 56, local, b));
    CHECK(past->error().find("three words inside it") != std::string::npos);
}

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
    CHECK(cb->dispatch(kernels::end_only, one, nullptr, std::vector<Binding>{}));
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

TEST(a_constant_shorter_than_its_pushed_range_is_read_from_the_objects_pages) {
    // fill_id's constant is 4 bytes; ir3 pushes 64 (whole vec4, padded) and the GPU reads all of them. A 4-byte buffer
    // is still a whole page of GPU memory, so the read faults nothing and the dispatch goes: this is what Metal's
    // -setBytes:length:atIndex: binds, and the first refusal of it was measured on the phone, not on the GPU. The same
    // range at the last 32 bytes of a page would run off the object: that is still refused, and the message says
    // both numbers.
    Rig r;
    auto out = buffer(r, 4096, 0);
    auto c = buffer(r, 4, 0x1234);
    const uint32_t local[3] = {64, 1, 1};
    {
        auto cb = r.queue->commandBuffer();
        CHECK(cb->dispatch(kernels::fill_id_air, kGroups16, local, {Binding{out.get(), 0}, Binding{c.get(), 0}}));
        CHECK(cb->error().empty());
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        CHECK_EQ(r.fake->stats().faults, 0u);
    }
    {
        auto page = buffer(r, 16384, 0x1234);   // exactly one page of GPU memory
        auto cb = r.queue->commandBuffer();
        CHECK(!cb->dispatch(kernels::fill_id_air, kGroups16, local, {Binding{out.get(), 0}, Binding{page.get(), 16384 - 32}}));
        CHECK(cb->error().find("reads 64 bytes at 0 of constant buffer 1; 32 are bound and 32 mapped") != std::string::npos);
    }
}

// With skipRefused a dispatch the encoder refuses is left out and the rest of the buffer runs: the refused one counted
// with its reason, the next one reaching the GPU, the buffer completed. The twin, the same two dispatches by default,
// ends in error and nothing of it is submitted.
TEST(a_refused_dispatch_is_left_out_when_the_buffer_skips_refusals) {
    Rig r;
    auto out = buffer(r, 4096, 0);
    for (bool skip : {true, false}) {
        auto cb = r.queue->commandBuffer();
        cb->setSkipRefused(skip);
        const uint32_t before = r.fake->stats().dispatches;
        CHECK(!cb->dispatch(kernels::fill_id, kGroups16, nullptr, std::vector<Binding>{}));
        const bool second = cb->dispatch(kernels::fill_id, kGroups16, nullptr, {Binding{out.get(), 0}});
        cb->commit();
        cb->waitUntilCompleted();
        if (skip) {
            CHECK(second);
            CHECK(cb->status() == CommandBuffer::kCompleted);
            CHECK_EQ(cb->skipped(), 1u);
            CHECK(cb->lastSkipped().find("no buffer bound at index 0") != std::string::npos);
            CHECK_EQ(r.fake->stats().dispatches - before, 1u);
        } else {
            CHECK(cb->status() == CommandBuffer::kError);
            CHECK_EQ(cb->skipped(), 0u);
            CHECK_EQ(r.fake->stats().dispatches - before, 0u);
        }
    }
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
        CHECK(!cb->dispatch(kernels::fill_id, kGroups16, nullptr, std::vector<Binding>{}));
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

TEST(an_event_with_the_timestamp_bit_writes_a_timestamp_not_the_value) {
    // the hang of 28 October: the dispatch's end waited for a value the event never wrote, because the TIMESTAMP bit
    // makes the GPU write a timestamp there instead. The fake does the same now, so that tail fails here.
    Rig r;
    auto out = buffer(r, 4096, 0);
    compute::Options bit;
    bit.tailTimestampBit = true;
    auto cb = r.queue->commandBuffer();
    CHECK(cb->dispatch(kernels::end_only, kGroups16, nullptr, std::vector<Binding>{}, nullptr, bit));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kError);
    CHECK(r.fake->stats().lastFault.find("would wait forever") != std::string::npos);
    // and the default form, the bit clear, completes
    Rig r2;
    auto cb2 = r2.queue->commandBuffer();
    CHECK(cb2->dispatch(kernels::end_only, kGroups16, nullptr, std::vector<Binding>{}));
    cb2->commit();
    cb2->waitUntilCompleted();
    CHECK(cb2->status() == CommandBuffer::kCompleted);
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

// --- textures and samplers in a dispatch -------------------------------------------------------------------------------------
// The fake runs no instruction, so a kernel here needs only a well-formed program and the layout fields of the kernel it
// stands for: fill_id_air's words carry the texture fields read off tools/kernels/tex_*.ll (the listings' numbers,
// compiler_test checks the compiler reads the same off the bitcode). With MESA=1 the bitcode itself is compiled too.

namespace {
Kernel kernelLike(const char* name, uint32_t numSsbos, uint32_t numIbos, uint32_t numSamp, uint32_t numTextureSlots, uint32_t samplerMask) {
    Kernel k = kernels::fill_id_air;
    k.name = name;
    k.numUboRanges = 0;   // no constant buffer to bind
    k.numSsbos = numSsbos;
    k.numIbos = numIbos;
    k.numSamp = numSamp;
    k.numTextureSlots = numTextureSlots;
    k.samplerMask = samplerMask;
    for (uint32_t i = 0; i < kMaxTextureSlots; i++) k.textures[i] = KernelTexture{};
    return k;
}
KernelTexture image2dUint() { return KernelTexture{kTexImage, kTexDim2D, 0, kElemUint, 32, 0, 1, 0, 0}; }
KernelTexture sampled2d(uint8_t element) { return KernelTexture{kTexSampled, kTexDim2D, 0, element, 0, 1, 0, 0, 0}; }
// tex_fill: image 0, nothing else (numSamp 1 = 0 textures + 1 image)
Kernel texFillLike() {
    Kernel k = kernelLike("tex_fill", 0, 1, 1, 0, 0);
    k.textures[0] = image2dUint();
    return k;
}
// tex_copy: in at texture 1 (sampled, isaml), out at texture 0 (image, IBO 0); numSamp 3 = texture 1 + 1, plus 1 image
Kernel texCopyLike() {
    Kernel k = kernelLike("tex_copy", 0, 1, 3, 2, 0);
    k.textures[0] = image2dUint();
    k.textures[1] = sampled2d(kElemUint);
    return k;
}
// tex_sample_gid: texture 0 sampled with sampler 0, out at buffer 0 (SSBO, IBO 0)
Kernel texSampleLike() {
    Kernel k = kernelLike("tex_sample_gid", 1, 1, 1, 1, 1);
    k.textures[0] = sampled2d(kElemFloat);
    return k;
}

std::shared_ptr<Texture> texture(Rig& r, PixelFormat f, uint32_t w, uint32_t h, uint32_t usage = kTextureUsageRead | kTextureUsageWrite) {
    TextureDesc d;
    d.format = f;
    d.width = w;
    d.height = h;
    d.usage = usage;
    std::string why;
    auto t = Texture::make(*r.gpu, d, &why);
    CHECK(t != nullptr);
    return t;
}

Sampler sampler(uint32_t filter = 0, uint32_t address = 0) {
    SamplerDesc d;
    d.minFilter = d.magFilter = filter;
    d.sAddress = d.tAddress = d.rAddress = address;
    Sampler s;
    std::string why;
    CHECK(makeSampler(d, &s, &why));
    return s;
}

Bindings bind(std::vector<Binding> buffers, std::vector<const Texture*> textures, std::vector<const Sampler*> samplers = {}) {
    Bindings b;
    b.buffers = std::move(buffers);
    b.textures = std::move(textures);
    b.samplers = std::move(samplers);
    return b;
}

// the dispatch refused, and what it said
std::string refused(Rig& r, const Kernel& k, const Bindings& b) {
    const uint32_t groups[3] = {8, 4, 1}, local[3] = {8, 8, 1};
    auto cb = r.queue->commandBuffer();
    CHECK(!cb->dispatch(k, groups, local, b));
    return cb->error();
}
bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
}  // namespace

TEST(a_texture_dispatch_that_does_not_fit_is_refused_before_anything_reaches_the_gpu) {
    Rig r;
    auto rgba = texture(r, PixelFormat::kRGBA8Unorm, 64, 32);
    auto u32 = texture(r, PixelFormat::kR32Uint, 64, 32);
    auto f32 = texture(r, PixelFormat::kR32Float, 64, 32);
    auto readOnly = texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageRead);
    auto writeOnly = texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageWrite);
    auto out = buffer(r, 4096, 0);
    Sampler s = sampler();
    // an image with nothing at its index, or something of the wrong class, usage or dimension
    CHECK(has(refused(r, texFillLike(), bind({}, {})), "tex_fill: no texture bound at index 0"));
    CHECK(has(refused(r, texFillLike(), bind({}, {nullptr})), "tex_fill: no texture bound at index 0"));
    CHECK(has(refused(r, texFillLike(), bind({}, {f32.get()})), "texture 0 is R32Float (float); tex_fill stores through it as texture2d<uint>"));
    CHECK(has(refused(r, texFillLike(), bind({}, {rgba.get()})), "texture 0 is RGBA8Unorm (float); tex_fill stores through it as texture2d<uint>"));
    CHECK(has(refused(r, texFillLike(), bind({}, {readOnly.get()})), "texture 0 was made without shader-write usage; tex_fill writes it"));
    {
        Kernel k = texFillLike();
        k.textures[0].dim = kTexDim3D;
        CHECK(has(refused(r, k, bind({}, {u32.get()})), "tex_fill addresses texture 0 as 3D image; texture 0 is of type 2"));
        k.textures[0].dim = kTexDim2D;
        k.textures[0].isArray = 1;
        CHECK(has(refused(r, k, bind({}, {u32.get()})), "tex_fill addresses texture 0 as 2D array image; texture 0 is of type 2"));
        k = texFillLike();
        k.textures[0].read = 1;   // read_write: a load needs read usage too
        CHECK(has(refused(r, k, bind({}, {writeOnly.get()})), "texture 0 was made without shader-read usage; tex_fill loads from it"));
    }
    // a sampled texture with nothing at its index, or the wrong class or usage; the image at 0 is fine
    CHECK(has(refused(r, texCopyLike(), bind({}, {u32.get()})), "tex_copy: no texture bound at index 1"));
    CHECK(has(refused(r, texCopyLike(), bind({}, {u32.get(), nullptr})), "tex_copy: no texture bound at index 1"));
    CHECK(has(refused(r, texCopyLike(), bind({}, {u32.get(), f32.get()})), "texture 1 is R32Float (float); tex_copy reads it as texture2d<uint>"));
    CHECK(has(refused(r, texCopyLike(), bind({}, {u32.get(), writeOnly.get()})), "texture 1 was made without shader-read usage; tex_copy reads it"));
    // a sampler the kernel names with nothing at its index
    CHECK(has(refused(r, texSampleLike(), bind({Binding{out.get(), 0}}, {rgba.get()})), "tex_sample_gid: no sampler bound at index 0"));
    CHECK(has(refused(r, texSampleLike(), bind({Binding{out.get(), 0}}, {rgba.get()}, {nullptr})), "tex_sample_gid: no sampler bound at index 0"));
    // ... and its buffer, as before
    CHECK(has(refused(r, texSampleLike(), bind({}, {rgba.get()}, {&s})), "tex_sample_gid: no buffer bound at index 0"));
    // the kernel's own counts: NSAMP's 5 bits, UBO descriptors nobody binds, indices past the tables
    {
        Kernel k = texFillLike();
        k.numSamp = 32;
        CHECK(has(refused(r, k, bind({}, {u32.get()})), "tex_fill: numSamp 32 does not fit SP_CS_CONFIG.NSAMP's 5 bits (31 at most)"));
        k = texFillLike();
        k.numUbos = 2;   // indexed constant buffers: UBO descriptors, the highest (1) refused for want of its buffer
        CHECK(has(refused(r, k, bind({}, {u32.get()})), "tex_fill: no buffer bound at index 1 (a constant buffer it indexes)"));
        k = texCopyLike();
        k.numTextureSlots = 17;
        CHECK(has(refused(r, k, bind({}, {u32.get(), u32.get()})), "tex_copy names texture or sampler indices past 15: the tables hold 16"));
        k = texSampleLike();
        k.samplerMask = 1u << 16;
        CHECK(has(refused(r, k, bind({Binding{out.get(), 0}}, {rgba.get()}, {&s})), "tex_sample_gid names texture or sampler indices past 15"));
    }
    CHECK_EQ(r.fake->stats().submits, 0u);
}

// --- positive dispatches: the descriptors the fake reads back are the textures' ----------------------------------------------

namespace {
const uint32_t kGroups8x4[3] = {8, 4, 1}, kLocal8x8[3] = {8, 8, 1};

// a dispatch through the command buffer, completed on the fake, no faults
FakeGpu::Stats dispatched(Rig& r, const Kernel& k, const Bindings& b, const compute::Options& o = compute::Options{}) {
    auto cb = r.queue->commandBuffer();
    CHECK(cb->dispatch(k, kGroups8x4, kLocal8x8, b, nullptr, o));
    CHECK(cb->error().empty());
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    FakeGpu::Stats s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    if (s.faults != 0) std::fprintf(stderr, "       %s\n", s.lastFault.c_str());
    return s;
}

uint32_t fieldOf(uint32_t word, uint32_t mask, uint32_t shift) { return (word & mask) >> shift; }

// the fields of a 2D descriptor against the texture it was made from
void checkIs2d(const a6xx::TexConst& d, const Texture& t, bool sampled) {
    using namespace a6xx;
    CHECK_EQ(fieldOf(d.dword[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT), uint32_t(A6XX_TEX_2D));
    CHECK_EQ(fieldOf(d.dword[0], A6XX_TEX_CONST_0_FMT__MASK, A6XX_TEX_CONST_0_FMT__SHIFT), uint32_t(t.info().fmt6));
    CHECK_EQ(fieldOf(d.dword[1], A6XX_TEX_CONST_1_WIDTH__MASK, A6XX_TEX_CONST_1_WIDTH__SHIFT), t.width());
    CHECK_EQ(fieldOf(d.dword[1], A6XX_TEX_CONST_1_HEIGHT__MASK, A6XX_TEX_CONST_1_HEIGHT__SHIFT), t.height());
    CHECK_EQ(fieldOf(d.dword[2], A6XX_TEX_CONST_2_PITCH__MASK, A6XX_TEX_CONST_2_PITCH__SHIFT), uint32_t(t.bytesPerRow()));
    CHECK_EQ(d.dword[4] | uint64_t(fieldOf(d.dword[5], A6XX_TEX_CONST_5_BASE_HI__MASK, A6XX_TEX_CONST_5_BASE_HI__SHIFT)) << 32, t.gpuAddress());
    CHECK_EQ(fieldOf(d.dword[5], A6XX_TEX_CONST_5_DEPTH__MASK, A6XX_TEX_CONST_5_DEPTH__SHIFT), 1u);
    // the sampled view carries the pitch alignment the layout chose, the storage view does not
    uint32_t cpp = t.info().bytesPerPixel, log2cpp = 0;
    while ((1u << (log2cpp + 1)) <= cpp) log2cpp++;
    CHECK_EQ(fieldOf(d.dword[2], A6XX_TEX_CONST_2_PITCHALIGN__MASK, A6XX_TEX_CONST_2_PITCHALIGN__SHIFT), sampled ? log2cpp : 0u);
}

bool allZero(const uint32_t* w, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (w[i] != 0) return false;
    }
    return true;
}
}  // namespace

TEST(a_dispatch_of_an_image_only_kernel_puts_a_storage_descriptor_at_ibo_0_and_no_texture_state) {
    // tex_fill's shape: the image at [[texture(0)]] of a kernel with no device buffer is IBO slot 0; no sampled texture,
    // so no CS_TEX group at all, as Gallium emits for such a shader (SP_CS_TEX_COUNT is never written: it reads 0)
    Rig r;
    auto out = texture(r, PixelFormat::kR32Uint, 64, 32);
    memset(out->contents(), 0xcc, size_t(out->buffer()->length()));
    FakeGpu::Stats s = dispatched(r, texFillLike(), bind({}, {out.get()}));
    CHECK_EQ(s.dispatches, 1u);
    CHECK_EQ(s.lastDispatch.ibos, 1u);
    CHECK_EQ(s.lastDispatch.ibo.size(), size_t(1));
    if (!s.lastDispatch.ibo.empty()) checkIs2d(s.lastDispatch.ibo[0], *out, false);
    CHECK_EQ(s.lastDispatch.textureCount, 0u);
    CHECK_EQ(s.lastDispatch.samplerCount, 0u);
    CHECK_EQ(s.lastDispatch.texConstTable, 0u);
    CHECK_EQ(s.lastDispatch.tex.size(), size_t(0));
    CHECK_EQ(s.lastDispatch.iboTable % 64, 0u);
    // the fake runs no instruction: the texture is as the test left it
    CHECK(allWords(*out->buffer(), 0xcccccccc));
    // and the same with RGBA32Uint, the four-channel format tex_fill's probe uses second
    auto wide = texture(r, PixelFormat::kRGBA32Uint, 64, 32);
    s = dispatched(r, texFillLike(), bind({}, {wide.get()}));
    CHECK_EQ(s.dispatches, 2u);
    if (!s.lastDispatch.ibo.empty()) checkIs2d(s.lastDispatch.ibo[0], *wide, false);
}

TEST(an_image_kernel_stores_into_a_2d_array_or_a_3d_texture_through_a_layered_descriptor) {
    // ir3 stores to both as stib.3d with the layer as z (tex_fill_arr, tex_fill_3d): what keeps layer z apart from layer
    // 0 is the descriptor's ARRAY_PITCH and DEPTH alone, so those are what is checked -- against the texture's own slice
    using namespace a6xx;
    Rig r;
    auto layered = [&](TextureType type, uint32_t depth) {
        TextureDesc d;
        d.type = type;
        d.format = PixelFormat::kRGBA32Uint;
        d.width = 16;
        d.height = 8;
        d.depth = depth;
        d.usage = kTextureUsageRead | kTextureUsageWrite;
        std::string why;
        auto t = Texture::make(*r.gpu, d, &why);
        CHECK(t != nullptr);
        if (t == nullptr) std::fprintf(stderr, "       %s\n", why.c_str());
        return t;
    };
    auto arr = layered(TextureType::k2DArray, 4);
    auto vol = layered(TextureType::k3D, 3);
    auto flat = texture(r, PixelFormat::kRGBA32Uint, 16, 8);
    Kernel arrK = texFillLike(), volK = texFillLike();
    arrK.textures[0].isArray = 1;
    volK.textures[0].dim = kTexDim3D;
    const struct {
        const Kernel* k;
        const Texture* t;
        uint32_t type, depth;
    } cases[] = {{&arrK, arr.get(), A6XX_TEX_2D, 4}, {&volK, vol.get(), A6XX_TEX_3D, 3}};
    for (const auto& c : cases) {
        FakeGpu::Stats s = dispatched(r, *c.k, bind({}, {c.t}));
        CHECK_EQ(s.lastDispatch.ibo.size(), size_t(1));
        if (s.lastDispatch.ibo.empty()) continue;
        const TexConst& d = s.lastDispatch.ibo[0];
        CHECK_EQ(fieldOf(d.dword[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT), c.type);
        CHECK_EQ(fieldOf(d.dword[5], A6XX_TEX_CONST_5_DEPTH__MASK, A6XX_TEX_CONST_5_DEPTH__SHIFT), c.depth);
        CHECK_EQ(uint64_t(fieldOf(d.dword[3], A6XX_TEX_CONST_3_ARRAY_PITCH__MASK, A6XX_TEX_CONST_3_ARRAY_PITCH__SHIFT)) << A6XX_TEX_CONST_3_ARRAY_PITCH__SCALE,
                 c.t->sliceBytes());
        CHECK(c.t->sliceBytes() >= uint64_t(c.t->bytesPerRow()) * 8);
    }
    // the twins: each kernel over the other shapes, and a plain 2D texture under either, is refused -- a layered store
    // through a one-layer descriptor would land every layer on the first
    CHECK(has(refused(r, arrK, bind({}, {flat.get()})), "tex_fill addresses texture 0 as 2D array image; texture 0 is of type 2"));
    CHECK(has(refused(r, arrK, bind({}, {vol.get()})), "tex_fill addresses texture 0 as 2D array image; texture 0 is of type 7"));
    CHECK(has(refused(r, volK, bind({}, {arr.get()})), "tex_fill addresses texture 0 as 3D image; texture 0 is of type 3"));
    CHECK(has(refused(r, texFillLike(), bind({}, {arr.get()})), "tex_fill addresses texture 0 as 2D image; texture 0 is of type 3"));
}

TEST(a_dispatch_of_tex_copy_loads_the_texture_table_with_a_zero_slot_0_and_one_zero_sampler) {
    // in at [[texture(1)]]: TEX_CONST slot 1, slot 0 Gallium's zero dummy; out at [[texture(0)]]: IBO slot 0; no
    // sampler named, one zero TEX_SAMP loaded all the same for the isaml's s#0; NTEX = NSAMP = 3 > TEX_COUNT 2
    Rig r;
    auto in = texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageRead);
    auto out = texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageWrite);
    FakeGpu::Stats s = dispatched(r, texCopyLike(), bind({}, {out.get(), in.get()}));
    CHECK_EQ(s.lastDispatch.ibos, 1u);
    if (!s.lastDispatch.ibo.empty()) checkIs2d(s.lastDispatch.ibo[0], *out, false);
    CHECK_EQ(s.lastDispatch.textureCount, 2u);
    CHECK_EQ(s.lastDispatch.samplerCount, 1u);
    CHECK_EQ(s.lastDispatch.tex.size(), size_t(2));
    CHECK_EQ(s.lastDispatch.samp.size(), size_t(1));
    if (s.lastDispatch.tex.size() == 2) {
        CHECK(allZero(s.lastDispatch.tex[0].dword, 16));
        checkIs2d(s.lastDispatch.tex[1], *in, true);
    }
    if (!s.lastDispatch.samp.empty()) CHECK(allZero(s.lastDispatch.samp[0].dword, 4));
    CHECK_EQ(s.lastDispatch.texConstTable % 64, 0u);
    CHECK_EQ(s.lastDispatch.texSampTable % 16, 0u);
    CHECK_EQ(s.lastDispatch.texSampTable, s.lastDispatch.texConstTable + (compute::kTableTexSampAt - compute::kTableTexConstAt));
    CHECK_EQ(s.lastDispatch.iboTable, s.lastDispatch.texConstTable - compute::kTableTexConstAt);
}

TEST(a_dispatch_of_tex_sample_gid_loads_the_sampler_it_was_given) {
    // texture 0 sampled with sampler 0, out at [[buffer(0)]] (IBO 0 a buffer descriptor); the sampler's four words land
    // at TEX_SAMP slot 0 as makeSampler made them
    Rig r;
    auto tex = texture(r, PixelFormat::kRGBA8Unorm, 8, 8, kTextureUsageRead);
    auto out = buffer(r, 4096, 0x7fc00001);
    Sampler linear = sampler(1, 0);
    FakeGpu::Stats s = dispatched(r, texSampleLike(), bind({Binding{out.get(), 0}}, {tex.get()}, {&linear}));
    CHECK_EQ(s.lastDispatch.ibos, 1u);
    if (!s.lastDispatch.ibo.empty()) {
        CHECK_EQ(fieldOf(s.lastDispatch.ibo[0].dword[2], a6xx::A6XX_TEX_CONST_2_TYPE__MASK, a6xx::A6XX_TEX_CONST_2_TYPE__SHIFT), uint32_t(a6xx::A6XX_TEX_BUFFER));
    }
    CHECK_EQ(s.lastDispatch.textureCount, 1u);
    CHECK_EQ(s.lastDispatch.samplerCount, 1u);
    if (!s.lastDispatch.tex.empty()) checkIs2d(s.lastDispatch.tex[0], *tex, true);
    if (!s.lastDispatch.samp.empty()) {
        CHECK_EQ(s.lastDispatch.samp[0].dword[0], 0x92au);
        CHECK_EQ(s.lastDispatch.samp[0].dword[1], 0x000fff40u);
        CHECK_EQ(s.lastDispatch.samp[0].dword[2], 0x20u);
        CHECK_EQ(s.lastDispatch.samp[0].dword[3], 0u);
    }
    CHECK(allWords(*out, 0x7fc00001));
    // a clamp-to-zero sampler: CLAMP_TO_BORDER with the border registers pointing at the table's zero entry
    Sampler zero = sampler(0, 4);
    s = dispatched(r, texSampleLike(), bind({Binding{out.get(), 0}}, {tex.get()}, {&zero}));
    CHECK_EQ(s.dispatches, 2u);
    if (!s.lastDispatch.samp.empty()) CHECK_EQ(s.lastDispatch.samp[0].dword[0], 0x1b60u);
}

TEST(an_image_slot_the_kernel_never_addresses_gets_a_hole_descriptor_over_the_table) {
    // in at [[texture(0)]] sampled, out at [[texture(1)]] an image: IBO 1 is the image and IBO 0, which ir3 counts and
    // nothing addresses, is a one-element buffer descriptor at the table's own kTableHoleAt bytes
    Rig r;
    Kernel k = kernelLike("tex_copy_swapped", 0, 2, 3, 1, 0);
    k.textures[0] = sampled2d(kElemUint);
    k.textures[1] = image2dUint();
    auto in = texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageRead);
    auto out = texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageWrite);
    Encoded e(r, k, kGroups8x4, kLocal8x8, bind({}, {in.get(), out.get()}));
    CHECK(e.ok);
    if (!e.ok) std::fprintf(stderr, "       %s\n", e.error.c_str());
    Raw raw(r);
    CHECK_EQ(raw.submit(e.stream.words()), 0);
    FakeGpu::Stats s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    CHECK_EQ(s.lastDispatch.ibos, 2u);
    CHECK_EQ(s.lastDispatch.ibo.size(), size_t(2));
    if (s.lastDispatch.ibo.size() == 2) {
        using namespace a6xx;
        const TexConst& hole = s.lastDispatch.ibo[0];
        CHECK_EQ(fieldOf(hole.dword[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT), uint32_t(A6XX_TEX_BUFFER));
        CHECK_EQ(fieldOf(hole.dword[1], A6XX_TEX_CONST_1_WIDTH__MASK, A6XX_TEX_CONST_1_WIDTH__SHIFT), 1u);
        CHECK_EQ((hole.dword[4] & A6XX_TEX_CONST_4_BASE_LO__MASK) | uint64_t(fieldOf(hole.dword[5], A6XX_TEX_CONST_5_BASE_HI__MASK, A6XX_TEX_CONST_5_BASE_HI__SHIFT)) << 32,
                 e.table->gpuAddress() + compute::kTableHoleAt);
        checkIs2d(s.lastDispatch.ibo[1], *out, false);
    }
    CHECK_EQ(s.lastDispatch.textureCount, 1u);
    if (!s.lastDispatch.tex.empty()) checkIs2d(s.lastDispatch.tex[0], *in, true);
    // the tables' addresses are the dispatch's own
    CHECK_EQ(s.lastDispatch.iboTable, e.table->gpuAddress() + compute::kTableIbosAt);
    CHECK_EQ(s.lastDispatch.texConstTable, e.table->gpuAddress() + compute::kTableTexConstAt);
    CHECK_EQ(s.lastDispatch.texSampTable, e.table->gpuAddress() + compute::kTableTexSampAt);
    // and the TP registers point at the zero entry
    size_t tp = e.findReg(a6xx::REG_A6XX_SP_TP_MODE_CNTL);
    CHECK(tp != size_t(-1));
    if (tp != size_t(-1)) CHECK_EQ(e.stream.words()[tp], 0xa2u);
    size_t border = e.findReg(a6xx::REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR);
    CHECK(border != size_t(-1));
    if (border != size_t(-1)) CHECK_EQ(e.stream.words()[border], uint32_t(e.table->gpuAddress() + compute::kTableBorderColorAt));
}

#ifdef HAKIOS_METAL_MESA
#include "hakios-metal/compiler.hpp"
#include <cstdio>
namespace {
std::unique_ptr<CompiledKernel> compiledKernel(const char* name) {
    std::vector<uint8_t> bytes;
    FILE* f = fopen((std::string("out/host/kernels/") + name + ".bc").c_str(), "rb");
    CHECK(f != nullptr);
    if (f == nullptr) return nullptr;
    uint8_t buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    fclose(f);
    std::string why;
    auto k = compileAir(bytes.data(), bytes.size(), &why);
    CHECK(k != nullptr);
    if (k == nullptr) std::fprintf(stderr, "       %s\n", why.c_str());
    return k;
}
}  // namespace

TEST(the_three_texture_kernels_compiled_in_process_dispatch_on_the_fake) {
    // the real kernels, not the hand-built likenesses: what the compiler read off the NIR drives the dispatch
    Rig r;
    auto fill = compiledKernel("tex_fill");
    auto copy = compiledKernel("tex_copy");
    auto sample = compiledKernel("tex_sample_gid");
    if (fill == nullptr || copy == nullptr || sample == nullptr) return;
    auto in = texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageRead);
    auto out = texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageWrite);
    auto c = buffer(r, 64, 0x5678);
    auto rgba = texture(r, PixelFormat::kRGBA8Unorm, 8, 8, kTextureUsageRead);
    auto result = buffer(r, 4096, 0);
    Sampler s0 = sampler();
    FakeGpu::Stats s = dispatched(r, fill->kernel, bind({Binding{c.get(), 0}}, {out.get()}));
    CHECK_EQ(s.lastDispatch.ibos, 1u);
    CHECK_EQ(s.lastDispatch.textureCount, 0u);
    if (!s.lastDispatch.ibo.empty()) checkIs2d(s.lastDispatch.ibo[0], *out, false);
    s = dispatched(r, copy->kernel, bind({}, {out.get(), in.get()}));
    CHECK_EQ(s.lastDispatch.ibos, 1u);
    CHECK_EQ(s.lastDispatch.textureCount, 2u);
    CHECK_EQ(s.lastDispatch.samplerCount, 1u);
    if (s.lastDispatch.tex.size() == 2) checkIs2d(s.lastDispatch.tex[1], *in, true);
    s = dispatched(r, sample->kernel, bind({Binding{result.get(), 0}}, {rgba.get()}, {&s0}));
    CHECK_EQ(s.lastDispatch.textureCount, 1u);
    CHECK_EQ(s.lastDispatch.samplerCount, 1u);
    if (!s.lastDispatch.samp.empty()) CHECK_EQ(s.lastDispatch.samp[0].dword[0], 0x920u);
    CHECK_EQ(s.dispatches, 3u);
    // the refusals hold for the real kernels too
    CHECK(has(refused(r, copy->kernel, bind({}, {out.get()})), "tex_copy: no texture bound at index 1"));
    CHECK(has(refused(r, sample->kernel, bind({Binding{result.get(), 0}}, {rgba.get()})), "tex_sample_gid: no sampler bound at index 0"));
    CHECK(has(refused(r, fill->kernel, bind({Binding{c.get(), 0}}, {in.get()})), "texture 0 was made without shader-write usage; tex_fill writes it"));
}
#endif

// --- the fake can fail on texture state: one class of fault at a time ------------------------------------------------------

namespace {
// an Encoded dispatch of tex_copy's shape over two 64x32 R32Uint textures, for the mutations below
struct CopyCase {
    explicit CopyCase(Rig& r, const compute::Options& o = compute::Options{})
        : in(texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageRead)),
          out(texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageWrite)),
          e(r, texCopyLike(), kGroups8x4, kLocal8x8, bind({}, {out.get(), in.get()}), o) {
        CHECK(e.ok);
    }
    std::shared_ptr<Texture> in, out;
    Encoded e;
};
std::string faultOf(Rig& r, const std::vector<uint32_t>& words) {
    Raw raw(r);
    CHECK_EQ(raw.submit(words), ETIMEDOUT);
    return r.fake->stats().lastFault;
}
}  // namespace

TEST(the_fake_refuses_a_texture_count_the_load_does_not_match) {
    Rig r;
    CopyCase c(r);
    std::vector<uint32_t> words = c.e.stream.words();
    size_t at = c.e.findReg(a6xx::REG_A6XX_SP_CS_TEX_COUNT);
    CHECK(at != size_t(-1));
    CHECK_EQ(words[at], 2u);
    words[at] = 1;
    CHECK(has(faultOf(r, words), "not loaded from where SP_CS_TEX_CONST points, or not as many as SP_CS_TEX_COUNT says"));
}

TEST(the_fake_refuses_a_texture_outside_every_buffer) {
    Rig r;
    CopyCase c(r);
    uint32_t* table = c.e.tableWords();
    table[compute::kTableTexConstAt / 4 + 16 * 1 + 4] = 0xfff00000u;   // texture 1's base, moved where no buffer is
    table[compute::kTableTexConstAt / 4 + 16 * 1 + 5] = (table[compute::kTableTexConstAt / 4 + 16 * 1 + 5] & ~a6xx::A6XX_TEX_CONST_5_BASE_HI__MASK) | 0x7u;
    std::string f = faultOf(r, c.e.stream.words());
    CHECK(has(f, "texture 1"));
    CHECK(has(f, "outside every buffer"));
}

TEST(the_fake_refuses_a_texture_pitch_shorter_than_a_row) {
    Rig r;
    CopyCase c(r);
    uint32_t* table = c.e.tableWords();
    uint32_t& d2 = table[compute::kTableTexConstAt / 4 + 16 * 1 + 2];
    d2 = (d2 & ~a6xx::A6XX_TEX_CONST_2_PITCH__MASK) | a6xx::A6XX_TEX_CONST_2_PITCH(64 * 4 - 64);   // 192 < 256
    std::string f = faultOf(r, c.e.stream.words());
    CHECK(has(f, "texture 1: pitch 192 for rows of 64 pixels of 4 bytes: shorter than a row"));
    // and a pitch that holds the row but is not a multiple of 64 << PITCHALIGN (2): 320 is a multiple of 64, not of 256
    Rig r2;
    CopyCase c2(r2);
    uint32_t& d2b = c2.e.tableWords()[compute::kTableTexConstAt / 4 + 16 * 1 + 2];
    d2b = (d2b & ~a6xx::A6XX_TEX_CONST_2_PITCH__MASK) | a6xx::A6XX_TEX_CONST_2_PITCH(320);
    CHECK(has(faultOf(r2, c2.e.stream.words()), "pitch 320 for rows of 64 pixels of 4 bytes: shorter than a row or not a multiple of 256 bytes (64 << PITCHALIGN 2)"));
}

TEST(the_fake_refuses_a_texture_of_a_format_it_does_not_know) {
    Rig r;
    CopyCase c(r);
    uint32_t& d0 = c.e.tableWords()[compute::kTableTexConstAt / 4 + 16 * 1 + 0];
    d0 = (d0 & ~a6xx::A6XX_TEX_CONST_0_FMT__MASK) | a6xx::A6XX_TEX_CONST_0_FMT(a6xx::FMT6_NONE);
    CHECK(has(faultOf(r, c.e.stream.words()), "texture 1: FMT 255 is not one this GPU model knows"));
}

TEST(the_fake_refuses_a_texture_load_without_a_sampler) {
    Rig r;
    CopyCase c(r);
    std::vector<uint32_t> words = c.e.stream.words();
    size_t load = c.e.findLoad(a6xx::ST6_SHADER, a6xx::SB6_CS_TEX);
    CHECK(load != size_t(-1));
    words[load + 1] &= ~a6xx::CP_LOAD_STATE6_0_NUM_UNIT__MASK;   // NUM_UNIT 0: a load of no samplers
    CHECK(has(faultOf(r, words), "sampler descriptors were not loaded from where SP_CS_TEX_SAMP points (a texture load needs at least one)"));
}

TEST(the_fake_refuses_a_border_clamp_with_the_border_registers_unwritten) {
    // the same dispatch with tpState off: the clamp-to-zero sampler's CLAMP_TO_BORDER has no table to read
    Rig r;
    auto tex = texture(r, PixelFormat::kRGBA8Unorm, 8, 8, kTextureUsageRead);
    auto out = buffer(r, 4096, 0);
    Sampler zero = sampler(0, 4);
    compute::Options noTp;
    noTp.tpState = false;
    Encoded e(r, texSampleLike(), kGroups8x4, kLocal8x8, bind({Binding{out.get(), 0}}, {tex.get()}, {&zero}), noTp);
    CHECK(e.ok);
    CHECK(has(faultOf(r, e.stream.words()), "sampler 0: a border-clamping sampler with no border colour table (SP_TP_BORDER_COLOR_BASE_ADDR was never written)"));
    // and with the registers written (the default) the same words pass
    Rig r2;
    auto tex2 = texture(r2, PixelFormat::kRGBA8Unorm, 8, 8, kTextureUsageRead);
    auto out2 = buffer(r2, 4096, 0);
    Encoded e2(r2, texSampleLike(), kGroups8x4, kLocal8x8, bind({Binding{out2.get(), 0}}, {tex2.get()}, {&zero}));
    Raw raw(r2);
    CHECK_EQ(raw.submit(e2.stream.words()), 0);
    CHECK_EQ(r2.fake->stats().faults, 0u);
    // a wrap mode the enum does not have, and a sampler with its last word set
    Rig r3;
    CopyCase c(r3);
    c.e.tableWords()[compute::kTableTexSampAt / 4 + 0] = a6xx::A6XX_TEX_SAMP_0_WRAP_S__MASK;   // WRAP_S 7
    CHECK(has(faultOf(r3, c.e.stream.words()), "sampler 0: wrap mode 7 is not one a6xx_tex_clamp names"));
    Rig r4;
    CopyCase c4(r4);
    c4.e.tableWords()[compute::kTableTexSampAt / 4 + 3] = 1;
    CHECK(has(faultOf(r4, c4.e.stream.words()), "sampler 0: dword 3 is 0x1, and it has no fields"));
}

TEST(the_fake_refuses_an_image_whose_rows_run_off_its_buffer) {
    // a 64x64 R32Uint texture is exactly one 16 KiB object; HEIGHT 65 reaches past it
    Rig r;
    auto out = texture(r, PixelFormat::kR32Uint, 64, 64, kTextureUsageWrite);
    CHECK_EQ(out->buffer()->length(), 16384u);
    Encoded e(r, texFillLike(), kGroups8x4, kLocal8x8, bind({}, {out.get()}));
    CHECK(e.ok);
    uint32_t& d1 = e.tableWords()[compute::kTableIbosAt / 4 + 1];
    d1 = (d1 & ~a6xx::A6XX_TEX_CONST_1_HEIGHT__MASK) | a6xx::A6XX_TEX_CONST_1_HEIGHT(65);
    std::string f = faultOf(r, e.stream.words());
    CHECK(has(f, "IBO 0: 65 rows (68 counted) of 256 bytes"));
    CHECK(has(f, "outside every buffer"));
    // the storage descriptor's pitch rule: a multiple of 64 but short of the row
    Rig r2;
    auto out2 = texture(r2, PixelFormat::kR32Uint, 64, 64, kTextureUsageWrite);
    Encoded e2(r2, texFillLike(), kGroups8x4, kLocal8x8, bind({}, {out2.get()}));
    uint32_t& d2 = e2.tableWords()[compute::kTableIbosAt / 4 + 2];
    d2 = (d2 & ~a6xx::A6XX_TEX_CONST_2_PITCH__MASK) | a6xx::A6XX_TEX_CONST_2_PITCH(128);
    CHECK(has(faultOf(r2, e2.stream.words()), "IBO 0: pitch 128 for rows of 64 pixels of 4 bytes: shorter than a row or not a multiple of 64 bytes (64 << PITCHALIGN 0)"));
}

TEST(the_fake_refuses_a_load_of_ibos_into_the_texture_block) {
    Rig r;
    CopyCase c(r);
    std::vector<uint32_t> words = c.e.stream.words();
    size_t load = c.e.findLoad(a6xx::ST6_SHADER, a6xx::SB6_CS_TEX);
    CHECK(load != size_t(-1));
    words[load + 1] = (words[load + 1] & ~a6xx::CP_LOAD_STATE6_0_STATE_TYPE__MASK) | a6xx::CP_LOAD_STATE6_0_STATE_TYPE(a6xx::ST6_IBO);
    CHECK(has(faultOf(r, words), "a load of type 3 into the compute texture block (SB6_CS_TEX): not samplers (ST6_SHADER) or textures (ST6_CONSTANTS)"));
}

TEST(the_fake_refuses_a_zero_ibo_descriptor) {
    Rig r;
    auto out = texture(r, PixelFormat::kR32Uint, 64, 32, kTextureUsageWrite);
    Encoded e(r, texFillLike(), kGroups8x4, kLocal8x8, bind({}, {out.get()}));
    CHECK(e.ok);
    memset(e.tableWords() + compute::kTableIbosAt / 4, 0, 64);
    CHECK(has(faultOf(r, e.stream.words()), "IBO 0 is all zeros: an unaddressed slot gets a hole descriptor, not zeros"));
    // a texture table slot of zeros, by contrast, is Gallium's dummy and passes (tex_copy's slot 0 above); NTEX below
    // the count does not
    Rig r2;
    CopyCase c(r2);
    std::vector<uint32_t> words = c.e.stream.words();
    size_t cfg = c.e.findReg(a6xx::REG_A6XX_SP_CS_CONFIG);
    CHECK(cfg != size_t(-1));
    words[cfg] = (words[cfg] & ~(a6xx::A6XX_SP_CS_CONFIG_NTEX__MASK | a6xx::A6XX_SP_CS_CONFIG_NSAMP__MASK)) | a6xx::A6XX_SP_CS_CONFIG_NTEX(1) | a6xx::A6XX_SP_CS_CONFIG_NSAMP(1);
    CHECK(has(faultOf(r2, words), "SP_CS_CONFIG names 1 textures and 1 samplers; SP_CS_TEX_COUNT says 2 (NTEX must cover it and equal NSAMP)"));
}

// An indexed constant buffer in a dispatch: one UBO descriptor per UBO ir3 counts, from the packet into SB6_CS_SHADER,
// the buffer past its offset and its length in vec4; the fake judges each descriptor and faults on one past every buffer
TEST(a_dispatch_loads_the_ubo_descriptors_an_ldc_reads_through) {
    using namespace a6xx;
    Rig r;
    auto out = buffer(r, 4096, 0);
    auto u0 = buffer(r, 64, 0);
    Kernel k = kernels::fill_id;
    k.numUbos = 2;   // (fill_id reads none: the descriptors are what is checked, not what the kernel does with them)
    Encoded e(r, k, kGroups16, nullptr, {Binding{out.get(), 0}, Binding{u0.get(), 16}});
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    bool found = false;
    const auto& w = e.stream.words();
    for (size_t i = 0; i < w.size();) {
        pm4::Header h = pm4::decode(w[i]);
        if (h.kind == pm4::Header::kCommand && h.opcode == CP_LOAD_STATE6_FRAG &&
            ((w[i + 1] & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> CP_LOAD_STATE6_0_STATE_TYPE__SHIFT) == ST6_UBO) {
            found = true;
            CHECK_EQ((w[i + 1] & CP_LOAD_STATE6_0_NUM_UNIT__MASK) >> CP_LOAD_STATE6_0_NUM_UNIT__SHIFT, 2u);
            CHECK_EQ(w[i + 4], uint32_t(out->gpuAddress()));
            CHECK_EQ(w[i + 5], uint32_t(out->gpuAddress() >> 32) | 256u << 17);   // 4096 bytes: 256 vec4
            CHECK_EQ(w[i + 6], uint32_t(u0->gpuAddress() + 16));
            CHECK_EQ(w[i + 7], uint32_t((u0->gpuAddress() + 16) >> 32) | 3u << 17);   // 64 - 16 bytes: 3 vec4
        }
        i += 1 + h.count;
    }
    CHECK(found);
    Raw raw(r);
    CHECK_EQ(raw.submit(e.stream.words()), 0);
    // misaligned, and the fake's twin
    Encoded bad(r, k, kGroups16, nullptr, {Binding{out.get(), 0}, Binding{u0.get(), 6}});
    CHECK(!bad.ok);
    CHECK(has(bad.error, "fill_id: buffer 1: offset 6 is not a multiple of 4 inside it"));
    std::vector<uint32_t> words = e.stream.words();
    for (size_t i = 0; i < words.size();) {
        pm4::Header h = pm4::decode(words[i]);
        if (h.kind == pm4::Header::kCommand && h.opcode == CP_LOAD_STATE6_FRAG &&
            ((words[i + 1] & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> CP_LOAD_STATE6_0_STATE_TYPE__SHIFT) == ST6_UBO) {
            words[i + 7] |= A6XX_UBO_1_SIZE__MASK;
        }
        i += 1 + h.count;
    }
    CHECK_EQ(raw.submit(words), ETIMEDOUT);
    CHECK(has(r.fake->stats().lastFault, "UBO descriptor 1 of the compute stage: 32767 vec4 at"));
}

// A kernel longer than the instruction cache gets Mesa's workaround for the A6xx fetch that bounds-checks against the
// other context's SP_FS_INSTRLEN (fd6_compute.cc, tu_dispatch): SP_FS_INSTRLEN = its own length and a LABEL event before
// CP_EXEC_CS, another LABEL after it. One that fits gets none of it, nor does one preloaded whole. On the phone, without
// it, a 134-unit kernel wrote every word wrong and a 492-unit one hung (probe 5d2); with it both are exact.
TEST(a_kernel_longer_than_the_instruction_cache_gets_the_instrlen_workaround) {
    Rig r;
    auto out = buffer(r, 4096, 0);
    auto c = buffer(r, 64, 0);
    const uint32_t local[3] = {64, 1, 1};
    const Bindings b{{Binding{out.get(), 0}, Binding{c.get(), 0}}, {}, {}, {}};
    auto labels = [](const Encoded& e, size_t* before, size_t* after) {
        const auto& w = e.stream.words();
        const size_t exec = e.find(a6xx::CP_EXEC_CS);
        *before = *after = 0;
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kCommand && h.opcode == a6xx::CP_EVENT_WRITE && h.count >= 1 && (w[i + 1] & 0x7f) == a6xx::LABEL) {
                (i < exec ? *before : *after) += 1;
            }
            i += 1 + h.count;
        }
    };
    {
        Encoded e(r, kernels::long_chain_air, kGroups16, local, b);
        CHECK(e.ok);
        size_t fs = e.findReg(a6xx::REG_A6XX_SP_FS_INSTRLEN);
        CHECK(fs != size_t(-1));
        if (fs != size_t(-1)) CHECK_EQ(e.stream.words()[fs], kernels::long_chain_air.instrlen);
        CHECK(fs < e.find(a6xx::CP_EXEC_CS));
        size_t before = 0, after = 0;
        labels(e, &before, &after);
        CHECK_EQ(before, size_t(1));
        CHECK_EQ(after, size_t(1));
        // and it still runs on the fake, its preload capped at 127
        auto cb = r.queue->commandBuffer();
        CHECK(cb->dispatch(kernels::long_chain_air, kGroups16, local, {Binding{out.get(), 0}, Binding{c.get(), 0}}));
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
    }
    {
        Encoded e(r, kernels::chain160_air, kGroups16, local, b);
        CHECK(e.ok);
        CHECK(e.findReg(a6xx::REG_A6XX_SP_FS_INSTRLEN) == size_t(-1));
        size_t before = 0, after = 0;
        labels(e, &before, &after);
        CHECK_EQ(before + after, size_t(0));
        // the same kernel with its preload cut below its length is fetched past the preload, and gets it
        compute::Options cut;
        cut.preloadCap = 64;
        Encoded f(r, kernels::chain160_air, kGroups16, local, b, cut);
        CHECK(f.ok);
        CHECK(f.findReg(a6xx::REG_A6XX_SP_FS_INSTRLEN) != size_t(-1));
    }
}
