// drawbench: what recording a compositor's frame costs this driver's CPU, on the fake GPU -- and, word for word, what
// it records (the golden streams, tests/golden.hpp).
//
//   drawbench [--runs N] [--cold KB] [--scene LETTERS] [--stages DIR] [--hist] [--csv]     the measurements
//   drawbench --golden                                                                     each scene's sums, nothing else
//
// The scenes are frames a window server draws, at the phone's size (750 x 1624):
//   A B C D E F P Q R G H   the core's own interface (CommandBuffer::draw) with hand-made Programs
//   J K L                   the C interface the plug-in drives (hakios_metal_cb_draw3 / _draw_indexed) with linked
//                           programs: vertex buffers and an index buffer, uniforms bound at multiples of 8, a stage that
//                           declares ten texture slots and binds one, framebuffer fetches, two colour outputs, the
//                           programs changing from draw to draw as a compositor's layers change them
//   S T                     with --stages DIR: the same through the C interface with the stages of a stage cache
//                           (<DIR>/*.stage, compiler.cpp's stageCacheSave: the window server's functions as the driver
//                           compiled them on a device), every vertex stage linked with every fragment stage that links.
//                           Such a cache is made from the system's own shaders: it stays on the machine it was taken
//                           from, and nothing of it is in this tree (tests/golden/ has no line of S or T).
// Each scene is recorded once to warm the queue's pools, then N times measured (--runs, 5): the time inside the draw
// calls alone, a draw; the heap allocations in them; the words the command processor is given.
// --cold KB touches KB kilobytes before each draw, so that a draw starts with as little in the processor's caches as
// the window server's own work between two draws leaves it (the host's caches hold the whole driver otherwise, which
// is not what the phone's do).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "golden.hpp"
#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/hakios_metal.h"
#include "hakios-metal/render.hpp"
#include "hakios-metal/testing.hpp"
#include "hakios-metal/texture.hpp"
#include "fixtures/tri_attr_vs_air.hpp"
#include "fixtures/tri_cflat_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_fb_fs_air.hpp"
#include "fixtures/tri_flat_fs_air.hpp"
#include "fixtures/tri_flat_vs_air.hpp"
#include "fixtures/tri_mrt_fs_air.hpp"
#include "fixtures/tri_tex39_fs_air.hpp"
#include "fixtures/tri_tex_fs_air.hpp"

using namespace hakios::metal;
namespace kernels = hakios::metal::kernels;

// every heap allocation of the process, counted (operator new is what std::vector, std::string and make_shared use)
static std::atomic<uint64_t> gAllocs{0}, gAllocBytes{0};
void* operator new(size_t n) {
    gAllocs.fetch_add(1, std::memory_order_relaxed);
    gAllocBytes.fetch_add(n, std::memory_order_relaxed);
    void* p = malloc(n ? n : 1);
    if (p == nullptr) abort();
    return p;
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

namespace {

// --- what is asked -------------------------------------------------------------------------------------------------
struct Asked {
    bool golden = false, hist = false, csv = false;
    uint32_t runs = 5;
    size_t coldBytes = 0;
    std::string scenes, stages;
} asked;

uint64_t nowNs() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

// --cold: the caches emptied of the driver before a draw, by touching this much of something else
std::vector<uint8_t> gCold;
void chill() {
    if (asked.coldBytes == 0) return;
    if (gCold.size() != asked.coldBytes) gCold.assign(asked.coldBytes, 1);
    uint8_t* p = gCold.data();
    for (size_t i = 0; i < gCold.size(); i += 64) p[i]++;
}

// Between two frames: the queue's thread puts a completed command buffer's stream memory back after the waiters are
// woken (CommandQueue::waiter), and a frame that commits before it has makes a new one -- which moves every address
// handed out after it. A frame of the window server comes milliseconds later; here it is waited for, so that the
// streams are the same every run.
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(asked.golden ? 4 : 1)); }

// one frame's draw calls, timed one by one (the chill between them is not theirs)
struct Clock {
    uint64_t ns = 0, allocs = 0, allocBytes = 0;
    uint32_t draws = 0;
    uint64_t t0 = 0, a0 = 0, b0 = 0;
    void begin() {
        chill();
        a0 = gAllocs.load(std::memory_order_relaxed);
        b0 = gAllocBytes.load(std::memory_order_relaxed);
        t0 = nowNs();
    }
    void end() {
        ns += nowNs() - t0;
        allocs += gAllocs.load(std::memory_order_relaxed) - a0;
        allocBytes += gAllocBytes.load(std::memory_order_relaxed) - b0;
        draws++;
    }
};
// what two readings of the clock around nothing cost, taken off every draw's time
double clockNs() {
    static double cost = -1;
    if (cost < 0) {
        uint64_t best = ~0ull;
        for (int k = 0; k < 2000; k++) {
            const uint64_t a = nowNs(), b = nowNs();
            best = std::min(best, b - a);
        }
        cost = double(best);
    }
    return cost;
}

struct Result {
    std::string letter, name;
    uint32_t draws = 0;
    std::vector<double> usPerDraw;       // one a measured frame
    double allocsPerDraw = 0, allocBytesPerDraw = 0;
    std::vector<double> commitUs;        // closing the last pass, the submission
    golden::Sums sums;                   // the last measured frame's submissions
    uint32_t refused = 0;
    std::string error;
};
std::vector<Result> gResults;

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

bool wanted(const char* letter) { return asked.scenes.empty() || asked.scenes.find(letter) != std::string::npos; }

// --- the packets of a frame, by kind (--hist) -----------------------------------------------------------------------
struct Histogram {
    std::map<std::string, uint64_t> n;
    uint64_t words = 0, regPackets = 0, regWords = 0;
};
Histogram gHist;
const char* opcodeName(uint32_t op) {
    using namespace a6xx;
    switch (op) {
    case pm4::kNop: return "CP_NOP";
    case pm4::kWaitMemWrites: return "CP_WAIT_MEM_WRITES";
    case pm4::kWaitForMe: return "CP_WAIT_FOR_ME";
    case pm4::kWaitForIdle: return "CP_WAIT_FOR_IDLE";
    case pm4::kMemWrite: return "CP_MEM_WRITE";
    case pm4::kIndirectBuffer: return "CP_INDIRECT_BUFFER";
    case pm4::kMemcpy: return "CP_MEMCPY";
    case CP_SET_BIN_DATA5: return "CP_SET_BIN_DATA5";
    case CP_SET_BIN_DATA5_OFFSET: return "CP_SET_BIN_DATA5_OFFSET";
    case CP_COND_WRITE5: return "CP_COND_WRITE5";
    case CP_WAIT_MEM_GTE: return "CP_WAIT_MEM_GTE";
    case CP_SKIP_IB2_ENABLE_GLOBAL: return "CP_SKIP_IB2_ENABLE_GLOBAL";
    case CP_BLIT: return "CP_BLIT";
    case CP_LOAD_STATE6_GEOM: return "CP_LOAD_STATE6_GEOM";
    case CP_LOAD_STATE6_FRAG: return "CP_LOAD_STATE6_FRAG";
    case CP_LOAD_STATE6: return "CP_LOAD_STATE6";
    case CP_DRAW_INDX_OFFSET: return "CP_DRAW_INDX_OFFSET";
    case CP_DRAW_INDIRECT_MULTI: return "CP_DRAW_INDIRECT_MULTI";
    case CP_WAIT_REG_MEM: return "CP_WAIT_REG_MEM";
    case CP_REG_TO_MEM: return "CP_REG_TO_MEM";
    case CP_SET_DRAW_STATE: return "CP_SET_DRAW_STATE";
    case CP_EVENT_WRITE: return "CP_EVENT_WRITE";
    case CP_SET_MODE: return "CP_SET_MODE";
    case CP_SET_VISIBILITY_OVERRIDE: return "CP_SET_VISIBILITY_OVERRIDE";
    case CP_SET_MARKER: return "CP_SET_MARKER";
    case CP_MEM_TO_MEM: return "CP_MEM_TO_MEM";
    default: return "type-7 other";
    }
}
void histWalk(FakeGpu& gpu, uint64_t address, uint32_t count, int depth) {
    const uint8_t* bytes = gpu.peek(address, 4ull * count);
    if (bytes == nullptr) return;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(bytes);
    gHist.words += count;
    for (uint32_t i = 0; i < count;) {
        const pm4::Header h = pm4::decode(w[i]);
        if (h.kind == pm4::Header::kBad) return;
        if (h.kind == pm4::Header::kRegisters) {
            gHist.regPackets++;
            gHist.regWords += h.count;
        } else {
            gHist.n[opcodeName(h.opcode)]++;
            if (h.opcode == pm4::kIndirectBuffer && h.count == 3 && depth < 3) {
                histWalk(gpu, uint64_t(w[i + 1]) | uint64_t(w[i + 2]) << 32, w[i + 3], depth + 1);
            }
        }
        i += 1 + h.count;
    }
}
golden::Sums gSums;
void onSubmit(FakeGpu& gpu, uint64_t address, uint32_t words, void*) {
    golden::hook(gpu, address, words, &gSums);
    if (asked.hist) histWalk(gpu, address, words, 0);
}

// ====================================================================================================================
// The core's interface: the audit's scenes
// ====================================================================================================================
struct Rig {
    Rig() {
        auto t = std::make_unique<FakeGpu>();
        fake = t.get();
        gpu = std::make_unique<Gpu>(std::move(t));
        std::string why;
        queue = CommandQueue::make(*gpu, &why);
    }
    FakeGpu* fake;
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<CommandQueue> queue;
};

std::shared_ptr<Texture> texture(Rig& r, uint32_t w, uint32_t h, uint32_t usage, PixelFormat f = PixelFormat::kBGRA8Unorm) {
    TextureDesc d;
    d.format = f;
    d.width = w;
    d.height = h;
    d.usage = usage;
    std::string why;
    auto t = Texture::make(*r.gpu, d, &why);
    if (t == nullptr) std::printf("texture %ux%u: %s\n", w, h, why.c_str());
    return t;
}

// the command buffer as the CoreAnimation plug-in sets one up with no metal_* boot-arg (HakiosMetalCA.m: -initWithQueue:)
std::shared_ptr<CommandBuffer> pluginBuffer(Rig& r, bool gmem) {
    auto cb = r.queue->commandBuffer();
    cb->setSkipRefused(true);
    cb->setProfile(false);
    cb->setGmem(gmem);
    cb->setGmemMinDraws(gmem ? 56 : 0);
    cb->setGmemBinning(true);
    cb->setNullFsTextures(false);
    cb->setGmemOff(0);
    cb->setGmemTuned(true);
    cb->setProfileDraws(false);
    return cb;
}

struct Scene {
    const char* letter;
    const char* name;
    uint32_t draws;          // draws of the main pass: textured ones first, then FETCH, then UNIFORM
    uint32_t fetch;          // ...of which read their target (framebuffer fetch)
    uint32_t uniform;        // ...of which read a constant buffer bound at a multiple of 8 that is not one of 16
    uint32_t offscreen;      // extra passes of one draw each into smaller targets before the main one (a blur chain)
    bool clear;              // the main pass's load action is Clear
    bool gmem;               // as the plug-in: GMEM allowed for passes of 56 draws or more
    uint32_t commandBuffers; // the frame split over this many command buffers (the main pass in the last)
    bool second = false;     // a second colour attachment the size of the first (DontCare/DontCare): CoreAnimation's main pass
    uint32_t fetchEvery = 0; // every Nth textured draw reads its target instead (fetch and no-fetch draws interleaved)
};

void runCore(const Scene& s) {
    if (!wanted(s.letter)) return;
    Rig r;
    Result res;
    res.letter = s.letter;
    res.name = s.name;
    const uint32_t W = 750, H = 1624;
    auto fb = texture(r, W, H, kTextureUsageRenderTarget | kTextureUsageRead);
    std::vector<std::shared_ptr<Texture>> icons;
    for (uint32_t i = 0; i < 24; i++) icons.push_back(texture(r, 120, 120, kTextureUsageRead));
    auto wallpaper = texture(r, W, H, kTextureUsageRead);
    auto fb2 = texture(r, W, H, kTextureUsageRenderTarget | kTextureUsageRead);
    std::vector<std::shared_ptr<Texture>> off;
    for (uint32_t i = 0; i < s.offscreen; i++) off.push_back(texture(r, std::max(W >> (i + 1), 16u), std::max(H >> (i + 1), 16u), kTextureUsageRenderTarget | kTextureUsageRead));
    Sampler smp;
    std::string why;
    makeSampler(SamplerDesc{}, &smp, &why);
    // one buffer of uniforms, bound at multiples of 8 as CoreAnimation binds its own (render.cpp: PushedRange)
    auto uniforms = newBuffer(*r.gpu, 64 * 1024, &why);

    Program tex{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
    Program flat{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat"};
    Program fetch{kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, Linkage{}, "tri_fb"};
    for (Program* p : {&tex, &flat}) {
        p->blend.enabled = true;   // premultiplied source-over
        p->blend.srcRgb = 1, p->blend.dstRgb = 5, p->blend.srcAlpha = 1, p->blend.dstAlpha = 5;
    }

    // frame 0 warms the queue's pools as a second frame finds them (the program buffers, the slabs, the command BO)
    const uint32_t frames = 1 + (asked.golden ? 1 : asked.runs);
    for (uint32_t frame = 0; frame < frames; frame++) {
        Clock clock;
        gSums.reset();
        std::vector<std::shared_ptr<CommandBuffer>> cbs;
        auto cb = pluginBuffer(r, s.gmem);
        for (uint32_t i = 0; i < s.offscreen; i++) {
            RenderPass p;
            p.color0 = off[i].get();
            p.load = LoadAction::kDontCare;
            Draw d;
            d.vertexCount = 6;
            d.viewport = Viewport{0, 0, double(off[i]->width()), double(off[i]->height()), 0, 1};
            Bindings fsb;
            fsb.textures = {i == 0 ? wallpaper.get() : off[i - 1].get()};
            fsb.samplers = {&smp};
            clock.begin();
            const bool ok = cb->draw(tex, p, d, Bindings{}, fsb);
            clock.end();
            if (!ok) res.refused++, res.error = cb->lastSkipped() + " | " + cb->error();
            if (s.commandBuffers > 1 && i + 1 < s.commandBuffers) {
                cbs.push_back(cb);
                cb = pluginBuffer(r, s.gmem);
            }
        }
        RenderPass pass;
        pass.color0 = fb.get();
        pass.load = s.clear ? LoadAction::kClear : LoadAction::kLoad;
        if (s.second) {
            pass.more[0].texture = fb2.get();
            pass.more[0].load = LoadAction::kDontCare;
            pass.more[0].store = StoreAction::kDontCare;
        }
        for (uint32_t i = 0; i < s.draws; i++) {
            Draw d;
            d.vertexCount = 6;
            // the first draw the wallpaper over everything, the rest icon-sized quads on a grid
            const double x = i == 0 ? 0 : 30 + 180 * (i % 4), y = i == 0 ? 0 : 60 + 200 * ((i / 4) % 7);
            d.viewport = Viewport{x, y, i == 0 ? double(W) : 120.0, i == 0 ? double(H) : 120.0, 0, 1};
            RenderPass p = pass;
            if (i != 0) p.load = LoadAction::kLoad;   // (the plug-in spends the Clear on the first draw: hm_spend_clears)
            Bindings fsb;
            // (the textured draws first and the uniform ones after them: the fake GPU keeps every constant load of a
            // submission and holds it to the constlen of each later draw, so a constlen-0 stage after a constlen-4 one
            // is a fault of the model's bookkeeping, not of the stream)
            const bool uniform = i >= s.draws - s.uniform;
            const bool isFetch = !uniform && (i >= s.draws - s.uniform - s.fetch || (s.fetchEvery != 0 && i != 0 && i % s.fetchEvery == 0));
            const Program* prog = isFetch ? &fetch : uniform ? &flat : &tex;
            if (prog == &tex) {
                fsb.textures = {i == 0 ? wallpaper.get() : icons[i % icons.size()].get()};
                fsb.samplers = {&smp};
            } else if (prog == &flat) {
                fsb.buffers = {Binding{uniforms.get(), 8ull * (1 + 2 * i)}};   // 8, 24, 40...: never a multiple of 16
            }
            clock.begin();
            const bool ok = cb->draw(*prog, p, d, Bindings{}, fsb);
            clock.end();
            if (!ok) res.refused++, res.error = cb->lastSkipped() + " | " + cb->error();
        }
        cbs.push_back(cb);
        const uint64_t c0 = nowNs();
        for (auto& b : cbs) b->commit();
        const uint64_t c1 = nowNs();
        for (auto& b : cbs) b->waitUntilCompleted();
        for (auto& b : cbs) {
            if (b->status() != CommandBuffer::kCompleted) res.error = "a command buffer ended in error: " + b->error() + " | " + r.fake->stats().lastFault;
        }
        if (frame != 0) {
            res.draws = clock.draws;
            res.usPerDraw.push_back((double(clock.ns) / clock.draws - clockNs()) / 1e3);
            res.commitUs.push_back(double(c1 - c0) / 1e3);
            res.allocsPerDraw = double(clock.allocs) / clock.draws;
            res.allocBytesPerDraw = double(clock.allocBytes) / clock.draws;
            res.sums = gSums;
        }
        cbs.clear();
        cb.reset();
        settle();
    }
    gResults.push_back(res);
}

// ====================================================================================================================
// The C interface: what the plug-in calls
// ====================================================================================================================
constexpr unsigned kBGRA8 = 80, kUsageRead = 1, kUsageTarget = 4, k2D = 2;

struct CRig {
    hakios_metal_device* device = hakios_metal_open_fake();
    hakios_metal_queue* queue = hakios_metal_queue_new(device);
    ~CRig() {
        hakios_metal_queue_free(queue);
        hakios_metal_close(device);
    }
};

// -initWithQueue: of the plug-in's command buffer, with no metal_* boot-arg
hakios_metal_cb* pluginCb(CRig& r, bool gmem) {
    hakios_metal_cb* cb = hakios_metal_cb_new(r.queue);
    hakios_metal_cb_skip_refused(cb, 1);
    hakios_metal_cb_set_profile(cb, 0);
    hakios_metal_cb_set_gmem(cb, gmem ? 1 : 0);
    hakios_metal_cb_set_gmem_min_draws(cb, gmem ? 56 : 0);
    hakios_metal_cb_set_gmem_binning(cb, 1);
    hakios_metal_cb_set_null_fs_textures(cb, 0);
    hakios_metal_cb_set_gmem_off(cb, 0);
    hakios_metal_cb_set_gmem_tuned(cb, 1);
    hakios_metal_cb_set_profile_draws(cb, 0);
    return cb;
}

// one kind of draw: a program, and what an encoder has bound when it draws with it
struct Layer {
    hakios_metal_program* program = nullptr;
    std::string name;
    bool indexed = false;                                 // drawn from the index buffer, its vertex buffer at vsVertexAt
    unsigned vsCount = 0, fsCount = 0;                    // the buffer tables' sizes (highest index bound + 1)
    hakios_metal_buffer* vs[31] = {};
    hakios_metal_buffer* fs[31] = {};
    uint64_t vsOffsets[31] = {}, fsOffsets[31] = {};
    bool vsUniform[31] = {}, fsUniform[31] = {};          // a constant buffer, its offset moved a draw (as an encoder's ring)
    unsigned texCount = 0, sampCount = 0;
    hakios_metal_texture* tex[16] = {};
    bool texVaries[16] = {};                              // a slot whose texture changes from draw to draw
    hakios_metal_sampler* samp[16] = {};
};

struct Frame {
    hakios_metal_render_pass pass{};
    std::vector<Layer> layers;
    std::vector<uint32_t> order;                          // the main pass's draws: which layer each is
    std::vector<hakios_metal_texture*> icons;
    hakios_metal_buffer* index = nullptr;
    bool gmem = true;
};

// one frame through the C interface: the draws of F in order; false with the first refusal when one is refused
void drawFrame(CRig& r, Frame& f, Result& res, bool measured) {
    Clock clock;
    gSums.reset();
    hakios_metal_cb* cb = pluginCb(r, f.gmem);
    hakios_metal_render_pass pass = f.pass;
    for (size_t i = 0; i < f.order.size(); i++) {
        Layer& l = f.layers[f.order[i]];
        hakios_metal_viewport vp{0, 0, 750, 1624, 0, 1};
        hakios_metal_scissor sc{};
        const bool scissored = i % 3 != 0;   // (most of a compositor's layers are clipped to their bounds)
        if (scissored) sc = hakios_metal_scissor{unsigned(30 + 180 * (i % 4)), unsigned(60 + 200 * ((i / 4) % 7)), 120, 120};
        // what the encoder has bound by now: each uniform buffer further along its ring, the layer's own texture
        uint64_t vso[31], fso[31];
        for (unsigned k = 0; k < l.vsCount; k++) vso[k] = l.vsOffsets[k] + (l.vsUniform[k] ? 8ull * (1 + 2 * (i % 512)) : 0);
        for (unsigned k = 0; k < l.fsCount; k++) fso[k] = l.fsOffsets[k] + (l.fsUniform[k] ? 8ull * (1 + 2 * (i % 512)) : 0);
        hakios_metal_texture* tex[16];
        for (unsigned k = 0; k < l.texCount; k++) tex[k] = l.texVaries[k] ? f.icons[(i + k) % f.icons.size()] : l.tex[k];
        clock.begin();
        hakios_metal_cb_set_vertex_textures(cb, nullptr, 0, nullptr, 0);   // (-hakiosDrawOnce: sets them before every draw)
        int drawn;
        if (l.indexed) {
            drawn = hakios_metal_cb_draw_indexed(cb, l.program, &pass, &vp, scissored ? &sc : nullptr, 3, 6, 0, f.index, 12 * (i % 64), 1, 0, 0, l.vs, vso,
                                                 l.vsCount, l.fs, fso, l.fsCount, tex, l.texCount, l.samp, l.sampCount);
        } else {
            drawn = hakios_metal_cb_draw3(cb, l.program, &pass, &vp, scissored ? &sc : nullptr, 3, 0, 6, 1, 0, l.vs, vso, l.vsCount, l.fs, fso, l.fsCount, tex,
                                          l.texCount, l.samp, l.sampCount);
        }
        clock.end();
        if (drawn != 0) {
            res.refused++;
            if (res.error.empty()) res.error = l.name + ": " + hakios_metal_error();
        }
        pass.load_action = 1;   // (hm_spend_clears: a Clear is the first draw's)
    }
    const uint64_t c0 = nowNs();
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    const uint64_t c1 = nowNs();
    hakios_metal_cb_wait(cb);
    if (hakios_metal_cb_status(cb) != HAKIOS_METAL_COMPLETED) res.error = std::string("the command buffer ended in error: ") + hakios_metal_cb_error(cb);
    hakios_metal_cb_free(cb);
    settle();
    if (measured) {
        res.draws = clock.draws;
        res.usPerDraw.push_back((double(clock.ns) / clock.draws - clockNs()) / 1e3);
        res.commitUs.push_back(double(c1 - c0) / 1e3);
        res.allocsPerDraw = double(clock.allocs) / clock.draws;
        res.allocBytesPerDraw = double(clock.allocBytes) / clock.draws;
        res.sums = gSums;
    }
}

void runFrames(CRig& r, Frame& f, const char* letter, const std::string& name) {
    Result res;
    res.letter = letter;
    res.name = name;
    const uint32_t frames = 1 + (asked.golden ? 1 : asked.runs);
    for (uint32_t frame = 0; frame < frames; frame++) drawFrame(r, f, res, frame != 0);
    gResults.push_back(res);
}

// the things every C scene binds from
struct Common {
    hakios_metal_texture *fb, *fb2, *wallpaper;
    std::vector<hakios_metal_texture*> icons;
    hakios_metal_sampler* sampler;
    hakios_metal_buffer *uniforms, *vertices, *index, *device;
    explicit Common(CRig& r) {
        device = hakios_metal_buffer_new(r.device, 64 * 1024);
        fb = hakios_metal_texture_new(r.device, k2D, kBGRA8, 750, 1624, kUsageRead | kUsageTarget);
        fb2 = hakios_metal_texture_new(r.device, k2D, kBGRA8, 750, 1624, kUsageRead | kUsageTarget);
        wallpaper = hakios_metal_texture_new(r.device, k2D, kBGRA8, 750, 1624, kUsageRead);
        for (int i = 0; i < 24; i++) icons.push_back(hakios_metal_texture_new(r.device, k2D, kBGRA8, 120, 120, kUsageRead));
        hakios_metal_sampler_desc sd{};
        sd.min_filter = sd.mag_filter = 1;
        sd.normalized_coordinates = 1;
        sd.lod_max_clamp = 1000.0f;
        sd.max_anisotropy = 1;
        sampler = hakios_metal_sampler_new(r.device, &sd);
        uniforms = hakios_metal_buffer_new(r.device, 256 * 1024);
        vertices = hakios_metal_buffer_new(r.device, 64 * 1024);
        index = hakios_metal_buffer_new(r.device, 4096);
        if (fb == nullptr || sampler == nullptr || uniforms == nullptr) std::printf("drawbench: %s\n", hakios_metal_error());
    }
    hakios_metal_render_pass mainPass(bool second) const {
        hakios_metal_render_pass p{};
        p.color0 = fb;
        p.load_action = 1;
        p.store_action = 1;
        if (second) {
            p.more[0].texture = fb2;   // CoreAnimation's second screen: DontCare / DontCare
            p.more[0].load_action = 0;
            p.more[0].store_action = 0;
        }
        return p;
    }
};

hakios_metal_program* linked(const Stage& vs, const Stage& fs, bool blend) {
    hakios_metal_program* p = hakios_metal_program_from_stages(vs, fs, kBGRA8);
    if (p == nullptr) {
        std::printf("drawbench: %s + %s: %s\n", vs.k.name, fs.k.name, hakios_metal_error());
        return nullptr;
    }
    if (blend) hakios_metal_program_set_blend(p, 1, 0, 0, 1, 5, 1, 5, 0xf);   // premultiplied source-over
    return p;
}

// the fixtures' programs as a compositor's layers
void fixtureLayers(Common& c, Frame* f) {
    f->index = c.index;
    f->icons = c.icons;
    // 0: a textured quad from a vertex buffer (two attributes) and the index buffer: an icon, a label
    {
        Layer l;
        l.name = "attr+tex";
        l.program = linked(kernels::tri_attr_vs_air, kernels::tri_tex_fs_air, true);
        hakios_metal_program_set_vertex_attribute(l.program, 0, 29, 0, 0);    // Float2 at 0
        hakios_metal_program_set_vertex_attribute(l.program, 1, 9, 8, 0);     // UChar4Normalized at 8
        hakios_metal_program_set_vertex_layout(l.program, 0, 12, 1, 1);
        l.indexed = true;
        l.vs[0] = c.vertices, l.vsCount = 1;
        l.tex[0] = c.icons[0], l.texVaries[0] = true, l.texCount = 1;
        l.samp[0] = c.sampler, l.sampCount = 1;
        f->layers.push_back(l);
    }
    // 1: the same vertex stage with a fragment stage that declares ten texture slots and samples two of them, one of
    // which the encoder leaves unbound (an uber shader's variant: the null texture goes there)
    {
        Layer l;
        l.name = "attr+tex39";
        l.program = linked(kernels::tri_attr_vs_air, kernels::tri_tex39_fs_air, true);
        hakios_metal_program_set_vertex_attribute(l.program, 0, 29, 0, 0);
        hakios_metal_program_set_vertex_attribute(l.program, 1, 9, 8, 0);
        hakios_metal_program_set_vertex_layout(l.program, 0, 12, 1, 1);
        l.indexed = true;
        l.vs[0] = c.vertices, l.vsCount = 1;
        l.tex[3] = c.icons[1], l.texVaries[3] = true, l.texCount = 4;
        l.samp[0] = c.sampler, l.sampCount = 10;
        for (unsigned k = 1; k < 10; k++) l.samp[k] = c.sampler;
        f->layers.push_back(l);
    }
    // 2: a colour from a constant buffer bound at a multiple of 8: a backdrop, a separator
    {
        Layer l;
        l.name = "flat+cflat";
        l.program = linked(kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, true);
        l.fs[0] = c.uniforms, l.fsUniform[0] = true, l.fsCount = 1;
        f->layers.push_back(l);
    }
    // 3: a framebuffer fetch: programmable blending
    {
        Layer l;
        l.name = "flat+fb";
        l.program = linked(kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, false);
        f->layers.push_back(l);
    }
    // 4: two colour outputs into the pass's two attachments
    {
        Layer l;
        l.name = "flat+mrt";
        l.program = linked(kernels::tri_flat_vs_air, kernels::tri_mrt_fs_air, false);
        f->layers.push_back(l);
    }
    // 5: the wallpaper: a textured quad without a vertex buffer
    {
        Layer l;
        l.name = "color+tex";
        l.program = linked(kernels::tri_color_vs_air, kernels::tri_tex_fs_air, false);
        l.tex[0] = c.wallpaper, l.texCount = 1;
        l.samp[0] = c.sampler, l.sampCount = 1;
        f->layers.push_back(l);
    }
}

void runFixtureScenes() {
    struct { const char* letter; const char* name; uint32_t draws; bool gmem; bool fetch; } scenes[] = {
        {"J", "C interface: a light frame, 20 draws of 3 programs in runs, sysmem", 20, false, false},
        {"K", "C interface: a main pass of 110 draws, 6 programs changing as layers do, two attachments, GMEM binned", 110, true, true},
        {"L", "C interface: the main pass of K in sysmem (GMEM off)", 110, false, true},
    };
    for (const auto& s : scenes) {
        if (!wanted(s.letter)) continue;
        CRig r;
        Common c(r);
        Frame f;
        f.gmem = s.gmem;
        f.pass = c.mainPass(true);
        fixtureLayers(c, &f);
        // the wallpaper first; then icons and labels in runs of one program with other layers between them, as a
        // compositor's tree gives them (the fake holds every constant load of a submission to each later draw's
        // constlen, so the layers with uniforms come last, as in the audit's scenes)
        f.order.push_back(5);
        const uint32_t uniform = s.draws / 5;
        for (uint32_t i = 1; i + uniform < s.draws; i++) {
            const uint32_t k = i % 12;
            f.order.push_back(k < 5 ? 0 : k < 8 ? 1 : k == 8 && s.fetch ? 3 : k == 9 && s.fetch ? 4 : k == 10 ? 1 : 0);
        }
        for (uint32_t i = 0; i < uniform; i++) f.order.push_back(2);
        runFrames(r, f, s.letter, s.name);
    }
}

// --- a phone's own stages (--stages) -------------------------------------------------------------------------------
struct LoadedStage {
    Stage stage{};
    std::vector<uint32_t> words, immediates;
    std::string name, file;
    uint32_t iboBase = 0;   // a fragment stage's: the vertex stage's device buffers it was compiled past (compileStage)
    bool writesMemory = true;   // CompiledStage::writesMemory: it stores to a device buffer (a draw of it is not replayed per tile)
};

// compiler.cpp's stageCacheLoad, for a file another build of the driver wrote: the Stage as it lay in that build's
// memory (the same layout: Stage and Kernel have not changed), its words and immediates; the rest is skipped
bool loadStage(const std::string& path, LoadedStage* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::string data;
    char chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;) data.append(chunk, n);
    fclose(f);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    const uint8_t* end = p + data.size();
    auto u32 = [&](uint32_t* v) {
        if (size_t(end - p) < 4) return false;
        memcpy(v, p, 4);
        p += 4;
        return true;
    };
    auto skip = [&](size_t n) {
        if (size_t(end - p) < n) return false;
        p += n;
        return true;
    };
    uint32_t magic = 0, n = 0;
    if (!u32(&magic) || magic != 0x53434d48 || size_t(end - p) < sizeof(Stage)) return false;
    memcpy(&out->stage, p, sizeof(Stage));
    p += sizeof(Stage);
    if (!u32(&n) || size_t(end - p) < 4ull * n) return false;
    out->words.resize(n);
    memcpy(out->words.data(), p, 4ull * n);
    p += 4ull * n;
    if (!u32(&n) || size_t(end - p) < 4ull * n) return false;
    out->immediates.resize(n);
    memcpy(out->immediates.data(), p, 4ull * n);
    p += 4ull * n;
    if (!u32(&n)) return false;
    for (uint32_t i = 0; i < n; i++) {   // the resources
        uint32_t len = 0;
        if (!skip(12 + 8) || !u32(&len) || !skip(len) || !skip(8)) return false;
    }
    if (!u32(&n)) return false;
    for (uint32_t i = 0; i < n; i++) {   // the varyings' names
        uint32_t len = 0;
        if (!u32(&len) || !skip(len)) return false;
    }
    uint32_t writes = 0;
    if (!u32(&n) || !skip(size_t(n) * (4 + sizeof(Sampler))) || !u32(&out->iboBase) || !u32(&writes)) return false;   // inline samplers, iboBase, writesMemory
    out->writesMemory = writes != 0;
    uint32_t len = 0;
    if (!u32(&len) || size_t(end - p) < len) return false;
    out->name.assign(reinterpret_cast<const char*>(p), len);
    out->stage.k.words = out->words.data();
    out->stage.k.numWords = uint32_t(out->words.size());
    out->stage.k.immediates = out->immediates.empty() ? nullptr : out->immediates.data();
    out->stage.k.numImmediates = uint32_t(out->immediates.size());
    out->stage.k.name = out->name.c_str();
    out->stage.k.source = "";
    return true;
}

void runStageScenes() {
    if (asked.stages.empty() || !(wanted("S") || wanted("T"))) return;
    std::vector<std::unique_ptr<LoadedStage>> vss, fss;
    {
        std::vector<std::string> files;
        if (DIR* d = opendir(asked.stages.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.size() > 6 && n.compare(n.size() - 6, 6, ".stage") == 0) files.push_back(n);
            }
            closedir(d);
        }
        std::sort(files.begin(), files.end());
        for (const std::string& n : files) {
            auto s = std::make_unique<LoadedStage>();
            s->file = n;
            if (!loadStage(asked.stages + "/" + n, s.get())) continue;
            (s->stage.kind == kStageVertex ? vss : fss).push_back(std::move(s));
        }
    }
    if (vss.empty() || fss.empty()) {
        std::printf("drawbench: no stages in %s\n", asked.stages.c_str());
        return;
    }
    for (const char* letter : {"S", "T"}) {
        if (!wanted(letter)) continue;
        CRig r;
        Common c(r);
        Frame f;
        f.gmem = letter[0] == 'S';
        f.pass = c.mainPass(true);
        f.index = c.index;
        f.icons = c.icons;
        hakios_metal_texture* lut = hakios_metal_texture_new_3d(r.device, kBGRA8, 16, 16, 16, kUsageRead);
        // every fragment stage with the first vertex stage it links with -- the one of its own family first, by the
        // names' common beginning (a vertex function and a fragment function named alike) -- bound as its tables ask
        for (const auto& fs : fss) {
            std::vector<const LoadedStage*> order;
            for (const auto& vs : vss) order.push_back(vs.get());
            auto common = [&](const LoadedStage* v) {
                size_t n = 0;
                while (n < v->name.size() && n < fs->name.size() && v->name[n] == fs->name[n]) n++;
                return n;
            };
            std::stable_sort(order.begin(), order.end(), [&](const LoadedStage* a, const LoadedStage* b) { return common(a) > common(b); });
            for (const LoadedStage* vs : order) {
                Linkage link{};
                std::string why;
                // (a fragment stage was compiled past its own vertex stage's device buffers: another's is not its pair)
                if (fs->iboBase != vs->stage.k.numSsbos && !(fs->iboBase == 0 && fs->stage.k.numSsbos == 0)) continue;
                if (!render::link(vs->stage, fs->stage, &link, &why)) continue;
                Layer l;
                l.name = vs->name + "+" + fs->name + " (" + fs->file.substr(0, 8) + ")";
                l.program = hakios_metal_program_from_stages(vs->stage, fs->stage, kBGRA8, fs->iboBase, (vs->writesMemory ? 1u : 0u) | (fs->writesMemory ? 2u : 0u));
                if (l.program == nullptr) continue;
                hakios_metal_program_set_blend(l.program, 1, 0, 0, 1, 5, 1, 5, 0xf);
                // the vertex stage's attributes from one vertex buffer, at the index past every constant buffer's
                unsigned attributes = 0, vertexAt = 0;
                for (const Stage* s : {&vs->stage}) {
                    for (uint32_t k = 0; k < s->k.numUboRanges; k++) vertexAt = std::max(vertexAt, s->k.uboRanges[k].block + 1);
                    vertexAt = std::max(vertexAt, std::max(s->k.numUbos, s->k.numSsbos));
                }
                if (vertexAt > 30) continue;
                for (uint32_t k = 0; k < vs->stage.numInputs; k++) {
                    const StageInput& in = vs->stage.inputs[k];
                    if (in.sysval || in.regid == a6xx::kRegidNone || in.slot < 15) continue;
                    hakios_metal_program_set_vertex_attribute(l.program, in.slot - 15u, 31, 16 * attributes, vertexAt);   // Float4
                    attributes++;
                }
                if (attributes != 0) {
                    hakios_metal_program_set_vertex_layout(l.program, vertexAt, 16 * attributes, 1, 1);
                    l.indexed = true;
                    l.vs[vertexAt] = c.vertices;
                    l.vsCount = vertexAt + 1;
                }
                // each stage's constant buffers: the pushed ranges' blocks and the UBOs an ldc reads, from the uniforms'
                // buffer at a place of their own, moved a draw
                auto constants = [&](const Stage& s, hakios_metal_buffer** table, uint64_t* offsets, bool* uniform, unsigned* count, unsigned skipIndex, bool skip) {
                    auto bind = [&](uint32_t block) {
                        if (block >= 31 || (skip && block == skipIndex) || (s.k.constantDataUbo != 0 && block == s.k.constantDataUbo - 1)) return;
                        table[block] = c.uniforms;
                        offsets[block] = 16384ull * block;
                        uniform[block] = true;
                        *count = std::max(*count, block + 1);
                    };
                    for (uint32_t k = 0; k < s.k.numUboRanges; k++) bind(s.k.uboRanges[k].block);
                    for (uint32_t k = 0; k < s.k.numUbos; k++) bind(k);
                };
                constants(vs->stage, l.vs, l.vsOffsets, l.vsUniform, &l.vsCount, vertexAt, attributes != 0);
                constants(fs->stage, l.fs, l.fsOffsets, l.fsUniform, &l.fsCount, 0, false);
                // ...and its device buffers, the one 3D IBO table's slots: the vertex stage's by its own indices, the
                // fragment stage's past them (a buffer of their own, bound where it is and left there)
                for (uint32_t k = 0; k < vs->stage.k.numSsbos && k < 31; k++) {
                    if (l.vs[k] == nullptr) l.vs[k] = c.device, l.vsCount = std::max(l.vsCount, k + 1);
                }
                for (uint32_t k = fs->iboBase; k < fs->stage.k.numSsbos && k - fs->iboBase < 31; k++) {
                    const uint32_t at = k - fs->iboBase;
                    if (l.fs[at] == nullptr) l.fs[at] = c.device, l.fsCount = std::max(l.fsCount, at + 1);
                }
                // the fragment stage's textures: what it samples, in the shape it samples it; every other draw of the
                // layer only the first of them (an uber shader's variant declares more than a layer binds)
                const Kernel& k = fs->stage.k;
                for (uint32_t t = 0; t < kMaxTextureSlots && t < k.numTextureSlots; t++) {
                    const KernelTexture& kt = k.textures[t];
                    if (kt.kind != kTexSampled || kt.isNull || kt.fbTarget != 0) continue;
                    if (kt.dim == kTexDim3D) l.tex[t] = lut;
                    else if (kt.dim == kTexDim2D && !kt.isArray && kt.element == kElemFloat) l.tex[t] = c.icons[t % c.icons.size()], l.texVaries[t] = l.texCount == 0;
                    else continue;   // (left unbound: the null texture)
                    l.texCount = t + 1;
                }
                for (uint32_t m = 0; m < kMaxTextureSlots; m++) {
                    if ((k.samplerMask >> m) & 1) l.samp[m] = c.sampler, l.sampCount = m + 1;
                }
                f.layers.push_back(l);
                break;
            }
        }
        // one frame of every layer to see which the driver takes; the main pass is then made of those, in runs
        {
            Result probe;
            f.order.clear();
            std::vector<uint32_t> taken;
            for (uint32_t i = 0; i < f.layers.size(); i++) {
                f.order.assign(1, i);
                Result one;
                drawFrame(r, f, one, false);
                if (one.refused == 0 && one.error.empty()) taken.push_back(i);
                else if (!asked.golden && !asked.csv) std::printf("   (left out: %s: %s)\n", f.layers[i].name.c_str(), one.error.c_str());
            }
            if (taken.empty()) {
                std::printf("drawbench: none of %zu programs of %s was drawn\n", f.layers.size(), asked.stages.c_str());
                return;
            }
            f.order.clear();
            // 110 draws: a program stays for 1 to 4 draws, then the next one of the cache
            for (uint32_t i = 0, at = 0; f.order.size() < 110; i++, at++) {
                for (uint32_t run = 0; run < 1 + i % 4 && f.order.size() < 110; run++) f.order.push_back(taken[at % taken.size()]);
            }
            char name[200];
            snprintf(name, sizeof name, "C interface: %zu programs of the phone's stage cache (%zu drawn), a main pass of 110 draws, two attachments, %s",
                     f.layers.size(), taken.size(), f.gmem ? "GMEM binned" : "sysmem");
            runFrames(r, f, letter, name);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--golden") asked.golden = true;
        else if (a == "--hist") asked.hist = true;
        else if (a == "--csv") asked.csv = true;
        else if (a == "--runs") asked.runs = std::max(1, atoi(value()));
        else if (a == "--cold") asked.coldBytes = size_t(atoi(value())) * 1024;
        else if (a == "--scene") asked.scenes = value();
        else if (a == "--stages") asked.stages = value();
        else {
            std::printf("drawbench [--runs N] [--cold KB] [--scene LETTERS] [--stages DIR] [--hist] [--csv] | --golden\n");
            return 2;
        }
    }
    if (asked.golden) asked.coldBytes = 0;
    FakeGpu::submitHook = onSubmit;
    const Scene scenes[] = {
        {"A", "light frame: 1 command buffer, 1 pass (Load), 20 blended draws (14 textured, 6 with uniforms), sysmem", 20, 0, 6, 0, false, true, 1},
        {"B", "the same frame with 5 of the textured draws reading their target (framebuffer fetch), sysmem", 20, 5, 6, 0, false, true, 1},
        {"C", "heavy frame: 3 offscreen passes of 1 draw + a main pass of 140 textured draws (>= 56: GMEM, binned)", 140, 0, 0, 3, false, true, 1},
        {"D", "the heavy frame with GMEM off (every pass sysmem), 46 of its draws with uniforms", 140, 0, 46, 3, false, false, 1},
        {"E", "the same with 23 framebuffer-fetch draws, GMEM off", 140, 23, 46, 3, false, false, 1},
        {"F", "the heavy frame of C split over 4 command buffers (3 offscreen + main)", 140, 0, 0, 3, false, true, 4},
        {"P", "the phone's main pass: 108 textured draws into two 750x1624 attachments (the second DontCare), GMEM binned", 108, 0, 0, 0, false, true, 1, true, 0},
        {"Q", "the same with every 4th draw a framebuffer fetch (27 runs of fetch after no-fetch)", 108, 0, 0, 0, false, true, 1, true, 4},
        {"R", "the main pass of P recorded once, in sysmem (GMEM off)", 108, 0, 0, 0, false, false, 1, true, 0},
        {"G", "one draw alone in a command buffer (the fixed cost of a pass and a submit)", 1, 0, 0, 0, false, true, 1},
        {"H", "one draw into a pass whose load action is Clear", 1, 0, 0, 0, true, true, 1},
    };
    for (const Scene& s : scenes) runCore(s);
    runFixtureScenes();
    runStageScenes();

    if (asked.golden) {
        for (const Result& r : gResults) {
            // (a frame of several command buffers in flight at once: which of their counter-stamp buffers comes back to the
            // queue's pool first is the threads' to decide, and the next frame's addresses with it -- its state alone is summed)
            const bool timed = r.sums.submits > 1;
            std::printf("%s draws %u refused %u submits %u words %llu loads %u stream %016llx state %016llx%s%s%s\n", r.letter.c_str(), r.draws, r.refused,
                        r.sums.submits, (unsigned long long)r.sums.words, r.sums.loads, (unsigned long long)(timed ? 0 : r.sums.stream),
                        (unsigned long long)r.sums.state, r.sums.bad ? " (unread)" : "", r.error.empty() ? "" : " (error)", timed ? " (stream by timing: not summed)" : "");
        }
        return 0;
    }
    if (asked.csv) {
        std::printf("scene,draws,us_min,us_median,us_max,allocs_per_draw,alloc_bytes_per_draw,words_per_draw,commit_us_median,runs,cold_kb\n");
        for (const Result& r : gResults) {
            std::printf("%s,%u,%.3f,%.3f,%.3f,%.2f,%.0f,%.1f,%.1f,%zu,%zu\n", r.letter.c_str(), r.draws, *std::min_element(r.usPerDraw.begin(), r.usPerDraw.end()),
                        median(r.usPerDraw), *std::max_element(r.usPerDraw.begin(), r.usPerDraw.end()), r.allocsPerDraw, r.allocBytesPerDraw,
                        double(r.sums.words) / r.draws, median(r.commitUs), r.usPerDraw.size(), asked.coldBytes / 1024);
        }
        return 0;
    }
    std::printf("drawbench: %u measured frame(s) a scene after one to warm up%s; us a draw: the time inside the draw calls\n", asked.runs,
                asked.coldBytes ? (", " + std::to_string(asked.coldBytes / 1024) + " KB touched before each draw").c_str() : "");
    std::printf("%-2s %6s %27s %10s %10s %10s %10s  %s\n", "", "draws", "us a draw (min median max)", "allocs", "bytes", "words", "commit us", "");
    for (const Result& r : gResults) {
        std::printf("%-2s %6u %9.2f %8.2f %8.2f %10.2f %10.0f %10.1f %10.0f  %s\n", r.letter.c_str(), r.draws,
                    *std::min_element(r.usPerDraw.begin(), r.usPerDraw.end()), median(r.usPerDraw), *std::max_element(r.usPerDraw.begin(), r.usPerDraw.end()),
                    r.allocsPerDraw, r.allocBytesPerDraw, double(r.sums.words) / r.draws, median(r.commitUs), r.name.c_str());
        if (r.refused != 0 || !r.error.empty()) std::printf("   !! %u refused; %s\n", r.refused, r.error.c_str());
    }
    if (asked.hist) {
        std::printf("\nall scenes' packets: %llu words, %llu register packets writing %llu registers\n", (unsigned long long)gHist.words,
                    (unsigned long long)gHist.regPackets, (unsigned long long)gHist.regWords);
        for (const auto& [k, v] : gHist.n) std::printf("   %-32s %llu\n", k.c_str(), (unsigned long long)v);
    }
    return 0;
}
