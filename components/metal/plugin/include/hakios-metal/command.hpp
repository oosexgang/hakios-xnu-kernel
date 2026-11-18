// Metal's command model on the GPU: a queue, the command buffers committed to it in order, and the blit work the
// command processor can do on its own. It is what MTLCommandQueue, MTLCommandBuffer and MTLBlitCommandEncoder are
// in the plug-in (objc/), and it is testable without them.
//
//   CommandQueue        a submission queue of the driver's own; its command buffers complete in commit order, and
//                       a thread of its own waits for their fences and runs their completed handlers in that order
//   CommandBuffer       Metal's states (not enqueued, enqueued, committed, scheduled, completed, error); its
//                       packets go into a buffer of the queue's (returned to the queue once the GPU is done with it)
//   BlitEncoder         copy and fill of buffers, by the command processor: CP_MEMCPY and CP_MEM_WRITE (pm4.hpp).
//                       Offsets and lengths are in bytes and must be multiples of 4: the command processor moves
//                       words. Metal on macOS asks the same of copies; a blit that is not aligned is refused with
//                       the reason, and the command buffer ends in the error state, as Metal's does on a bad blit.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "hakios-metal/compute.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/gpu.hpp"
#include "hakios-metal/pm4.hpp"

namespace hakios::metal {

class CommandQueue;
class Texture;

// a buffer a program sees: a GEM object with its length (MTLBuffer's -length may be less than the object's size)
class Buffer {
public:
    Buffer(std::unique_ptr<Bo> bo, uint64_t length) : bo_(std::move(bo)), length_(length) {}
    uint64_t length() const { return length_; }
    void* contents() const { return bo_->cpu(); }
    uint64_t gpuAddress() const { return bo_->gpuAddress(); }
    const Bo& bo() const { return *bo_; }
    Bo& bo() { return *bo_; }

private:
    std::unique_ptr<Bo> bo_;
    uint64_t length_;
};

std::shared_ptr<Buffer> newBuffer(Gpu& gpu, uint64_t length, std::string* why);
// the live Buffer whose memory holds GPU address ADDRESS, and *OFFSET its offset in it; nullptr when none does. An
// argument buffer names a buffer member by the address it was encoded with (setBuffer:offset:atIndex:, or the
// gpuAddress an application writes itself), and this is how the draw finds the Buffer to bind and keep.
std::shared_ptr<Buffer> bufferAt(uint64_t address, uint64_t* offset);
// IOSurface SURFACE_ID's memory as a Buffer, *INFO what the kernel says of the surface. One Buffer per surface while it
// lives: a second ask for the same surface gets the same Buffer (the kernel's handle for it is one), so that the
// handle is closed once, by the last owner. The last four wrapped are kept past their last user, a surface wrapped
// again then answered without the kernel (the window server wraps its display's page anew every frame). nullptr with *why.
std::shared_ptr<Buffer> surfaceBuffer(Gpu& gpu, uint32_t surfaceId, SurfaceInfo* info, std::string* why);
// B, which surfaceBuffer gave for a texture that was then refused, not kept for the next wrap
void forgetSurfaceBuffer(const Gpu* gpu, const std::shared_ptr<Buffer>& b);

class CommandBuffer : public std::enable_shared_from_this<CommandBuffer> {
public:
    enum Status { kNotEnqueued, kEnqueued, kCommitted, kScheduled, kCompleted, kError };

    Status status() const { return status_.load(); }
    const std::string& error() const { return error_; }

    // run after completion (or error), on the queue's thread, in the order of commit; only before commit
    void addCompletedHandler(std::function<void(CommandBuffer&)> handler);
    void enqueue();
    void commit();
    void waitUntilCompleted();
    // how long the GPU had it: from commit to the fence, in nanoseconds (Metal's GPUStartTime..GPUEndTime, roughly)
    int64_t gpuNanoseconds() const { return completedAt_ - committedAt_; }

    // --- the blit encoder's work, recorded into this command buffer ---
    bool copy(const Buffer& from, uint64_t fromOffset, const Buffer& to, uint64_t toOffset, uint64_t length);
    bool fill(const Buffer& buffer, uint64_t offset, uint64_t length, uint8_t value);
    // --- the compute encoder's work (compute.hpp): KERNEL over GROUPS work groups of LOCAL threads each (nullptr:
    // the kernel's own size), BUFFERS bound by index, IMMEDIATES in place of the kernel's constants (nullptr: its own).
    // The program and the descriptors go into buffers of their own that live until this completes.
    bool dispatch(const Kernel& kernel, const uint32_t groups[3], const uint32_t* local, const std::vector<Binding>& buffers,
                  const uint32_t* immediates = nullptr, const compute::Options& options = compute::Options{});
    // the same with textures and samplers (compute.hpp: Bindings); the textures' memory is used and kept as buffers are
    bool dispatch(const Kernel& kernel, const uint32_t groups[3], const uint32_t* local, const Bindings& bindings,
                  const uint32_t* immediates = nullptr, const compute::Options& options = compute::Options{});
    // the group counts three 32-bit words in ARGS at OFFSET (a multiple of 4), read when the GPU runs it
    bool dispatchIndirect(const Kernel& kernel, const Buffer& args, uint64_t offset, const uint32_t* local, const Bindings& bindings,
                          const uint32_t* immediates = nullptr, const compute::Options& options = compute::Options{});
    // --- the render encoder's work (render.hpp): one draw of PROGRAM into PASS's colour target, one complete pass per
    // call (begin, state, draw, end). The two programs' words and the pass's table go into buffers of their own that live
    // until this completes; the target's memory is used for write and kept. False with the reason recorded (render::encode's).
    bool draw(const Program& program, const RenderPass& pass, const Draw& draw, const Bindings& vsBindings, const Bindings& fsBindings,
              const render::Options& options = render::Options{});
    // a pass with no draw whose load action is Clear (render::encodeClear): the target filled with the clear colour, a
    // complete pass of its own. What an encoder that recorded no draw needs at its end; anything else is refused.
    bool clear(const RenderPass& pass, const render::Options& options = render::Options{});
    // -copyFromTexture:...toTexture:... and the scaled copies under it: rectangle S of SRC's mip level SL into rectangle D
    // of DST's level DL by the 2D engine, a pass of its own (render::encodeBlit says what it takes)
    bool blit(const Texture& src, uint32_t srcLevel, render::BlitRect s, const Texture& dst, uint32_t dstLevel, render::BlitRect d, bool linear,
              const render::Options& options = render::Options{});
    // the compressed textures among B's sampled ones made current (Texture::staging); a draw and a dispatch do it first
    bool freshen(const Bindings& b, const render::Options& options = {});
private:
    bool blitInto(const Texture& src, uint32_t srcLevel, render::BlitRect s, const Texture& dst, uint32_t dstLevel, render::BlitRect d, bool linear,
                  const render::Options& options);
public:
    // -generateMipmapsForTexture:: every level past 0 made from the one above it, halved with linear filtering, in order
    bool generateMipmaps(const Texture& texture, const render::Options& options = render::Options{});
    // a blit the caller could not record (a buffer not in the GPU's memory, say): the buffer ends in the error state
    void refuse(const std::string& why) { fail(why); }
    // a buffer it uses, kept alive until it completes (the owner may let go of it before that)
    void keep(std::shared_ptr<const Buffer> b) { kept_.push_back(std::move(b)); }
    // anything else it uses (a compiled kernel's words), the same way
    void keepAlive(std::shared_ptr<const void> p) { keptAny_.push_back(std::move(p)); }
    // OTHER's recorded work after this one's, as if it had been recorded here: its packets, what it names and what it
    // keeps alive -- a parallel render encoder's sub-encoders each record into a command buffer of their own, from any
    // thread in any order, and are appended in the order they were made. OTHER is left empty and is not committed; an
    // error it ended in is this one's. False after commit (of either).
    bool append(CommandBuffer& other);
    // What becomes of a draw, dispatch, copy or clear the encoder refuses (a binding it cannot serve, a shape it does not
    // take): by default the command buffer ends in error with the reason, as every test here expects; with skipRefused
    // that one piece of work is left out, counted and its reason kept, and the rest of the buffer still runs. Apple's
    // driver validates nothing and runs a frame whatever is wrong in it; a game whose every frame has one dispatch this
    // does not serve otherwise shows nothing at all (Resident Evil 4). Running out of memory, or work after commit,
    // still fails the buffer either way.
    void setSkipRefused(bool on) { skipRefused_ = on; }
    uint32_t skipped() const { return skipped_; }
    const std::string& lastSkipped() const { return lastSkipped_; }
    // the encoder's refusal of one piece of work, by the rule above: false either way
    bool refuseWork(const std::string& why);
    // What the buffer holds, counted as it is recorded: passes opened (each a pass start, its flushes and the wait for
    // idle at its tail, which is a fixed cost whatever is drawn), the draws in them, and dispatches, copies and clears
    // (each a pass of its own). What a frame's GPU time goes to is read off these beside the time (metal_fps).
    struct Counts { uint32_t passes = 0, draws = 0, dispatches = 0, copies = 0, clears = 0, gmemPasses = 0, tiles = 0, binnedPasses = 0; };   // (gmemPasses of the passes drawn in tiles, binnedPasses of those binned)
    const Counts& counts() const { return counts_; }
    // With profiling on, the command processor writes its always-on counter (CP_ALWAYS_ON_COUNTER, 19.2 MHz) at the start
    // of each pass and after its tail -- whose wait for idle makes the end the end -- for the first kMaxProfiled passes:
    // what each costs, beside what it was (a draw pass with its target's size and its draws, a copy, a clear, a
    // dispatch). Read once the buffer has completed. metal_fps names a frame's slowest passes with it.
    enum PassKind : uint32_t { kPassDraw = 0, kPassCopy = 1, kPassClear = 2, kPassDispatch = 3 };
    struct PassTime { PassKind kind; uint32_t width, height, draws; double us; uint32_t tiles = 0; uint64_t tuneKey = 0; uint8_t attached = 0, touched = 0; std::string sysmemWhy; };   // ATTACHED / TOUCHED: the colour attachments set, and those its draws wrote or read (a GMEM pass's)   // tiles 0: in sysmem; tuneKey: CommandQueue::preferGmem's
    static constexpr uint32_t kMaxProfiled = 64;
    void setProfile(bool on) { profile_ = on; }
    // ...and each draw of a sysmem pass on its own, the first kMaxProfiledDraws: a wait for idle before it and after it,
    // the counter at both (so the draws no longer overlap: what each costs alone), with its program's name. Which
    // draws of a heavy frame are heavy is read off these (metal_fps=draws).
    // PASS: its pass among passTimes'; AREA render::drawArea's; SLOT its stamps' (-1: a draw in tiles, listed and not timed)
    struct DrawTime { std::string name; double us; uint32_t pass = 0, area[4] = {}; int32_t slot = -1; };
    static constexpr uint32_t kMaxProfiledDraws = 512;
    void setProfileDraws(bool on) { profileDraws_ = on; }
    std::vector<DrawTime> drawTimes() const;
    // render::Options::gmem for this buffer's draws besides each draw's own (both must allow it): the C API's switch
    void setGmem(bool on) { gmem_ = on; }
    // GMEM's refinements off, for a bisect on the phone: kGmemWholeArea tiles the whole window whatever the draws touch,
    // kGmemEveryAttachment loads and stores every attachment, kGmemFetchBarrier orders a fetch by the barrier;
    // kGmemFetchNoBarrier leaves a tile's fetch to the rasterizer's ordering alone, kGmemOrderAll puts every draw of a
    // GMEM pass in that ordering and leaves the barrier out (both measurements: their pictures may be wrong)
    // kGmemFetchWeight: a pass's fetching draws count once, as any other draw, against setGmemMinDraws (gmemWeight)
    // kGmemHeavyBarrier: a tile's fetch barrier with a wait for idle before its CACHE_INVALIDATE as well
    // (render::Options::fbReadBarrierLight off)
    // kGmemSysmemUnordered: a fetching draw in sysmem with the barrier alone, its own primitives unordered
    // (render::Options::fbReadOrderedSysmem off: a wrong picture wherever one draw's primitives overlap)
    // kGmemAlwaysDual: a pass whose like cleared the bar last time recorded both ways all the same, as before
    // kGmemNoLikely: GMEM shared among all of a pass's attachments as it opens, not those its like touched, as before
    // (render::gmemPlan's LIKELY)
    enum : uint32_t { kGmemWholeArea = 1, kGmemEveryAttachment = 2, kGmemFetchBarrier = 4, kGmemFetchNoBarrier = 64, kGmemOrderAll = 128,
                      kGmemFetchWeight = 512, kGmemHeavyBarrier = 1024, kGmemNoShare = 8192, kGmemSysmemUnordered = 16384,
                      kGmemAlwaysDual = 32768, kGmemNoLikely = 65536 };
    void setGmemOff(uint32_t bits) { gmemOff_ = bits; }
    // GMEM or sysmem for each pass by CommandQueue::preferGmem (each pass stamped for it) rather than GMEM whenever a
    // pass can have it: what the plug-in asks; off by default, so that a test gets the mode it asks for
    void setGmemTuned(bool on) { tuning_ = on; }
    // GMEM for a pass of at least DRAWS draws, sysmem for one of fewer; 0: not by the draws. The draws are counted as
    // they come, so the pass is recorded both ways until it closes. SpringBoard's passes of 63 to 161 draws took 13 to
    // 22 ms in GMEM against 17 to 31 in sysmem, its passes of 45 to 49 draws 8 to 10 against 6 to 8, its light ones 5
    // against 0.13.
    void setGmemMinDraws(uint32_t draws) { gmemMinDraws_ = draws; }
    // what a pass weighs against that bar: its draws, and kFetchWeight more for each that reads its attachments -- in
    // sysmem each such draw waits for the colour cache's flush and reads the target from memory (render::encodeInPass's
    // barrier), in GMEM it reads the tile. CoreAnimation's paths, path_blit cut to its mask (ca_paths.h), left 47 draws
    // and 12 fetches in the home screen's main pass: 19 ms in sysmem while the page moved (logs/080158-run.log). And a
    // pass with any fetch weighs the bar at least: the rasterized wallpaper drawn again into its 896x1856 texture as an
    // app opened, its stripes by the path renderer -- 9 draws, 2 of them fetches, weighing 25 -- took 293 ms in sysmem,
    // a frame of a third of a second (logs/113822-run.log).
    static constexpr uint32_t kFetchWeight = 8;
    uint32_t gmemWeight(uint32_t draws, uint32_t fetches) const {
        if ((gmemOff_ & kGmemFetchWeight) || fetches == 0) return draws;
        return std::max(draws + kFetchWeight * fetches, gmemMinDraws_);
    }
    // render::Options::gmemBinning for this buffer's draws besides each draw's own (either asks for it)
    void setGmemBinning(bool on) { gmemBinning_ = on; }
    // every fragment texture the null texture (1x1, always in the texture cache): wrong pictures, but a draw's time then
    // says what its shader costs without its texture reads -- a measurement switch, as setGmemOff is a bisect's
    void setNullFsTextures(bool on) { nullFsTextures_ = on; }
    // why the last pass that could not be drawn in GMEM was not (gmemPlan's or gmemDrawOk's reason), empty when none was refused
    const std::string& gmemRefusal() const { return gmemWhy_; }
    // the words recorded so far (what the command processor will read)
    size_t streamWords() const { return stream_.words().size(); }
    size_t streamCapacity() const { return stream_.capacity(); }   // (words it holds room for: the queue's memory, takePass)
    std::vector<PassTime> passTimes() const;
    // why profiled pass I of a draw was drawn in sysmem ("" for a GMEM pass, or one not profiled); valid while the buffer lives
    const char* passSysmemWhy(size_t i) const { return i < profiled_.size() ? profiled_[i].sysmemWhy.c_str() : ""; }

private:
    friend class CommandQueue;
    explicit CommandBuffer(std::shared_ptr<CommandQueue> queue) : owner_(std::move(queue)), queue_(*owner_) {}
public:
    ~CommandBuffer();   // (its stamp buffers back to the queue: read until the last reader lets go, which is now)
private:
    bool fail(const std::string& why);
    bool skipRefused_ = false;
    Counts counts_;
    bool profile_ = false;
    bool tuning_ = false;     // setGmemTuned's (cleared by a failed stamp buffer too)
    bool profileDraws_ = false;
    std::vector<DrawTime> drawsProfiled_;
    uint32_t timedDraws_ = 0;   // (drawsProfiled_ with stamps: the next one's slot)
    std::shared_ptr<Buffer> drawStamps_;
    void drawStamp(uint32_t slot);
    bool gmem_ = true;
    uint32_t gmemOff_ = 0;
    uint32_t gmemMinDraws_ = 0;
    bool gmemBinning_ = false;
    bool nullFsTextures_ = false;
    uint32_t gmemClears_ = 0, gmemClearWords_[kMaxColorOutputs][4] = {};   // (draw's, for the pass it opens)
    std::string gmemWhy_;
    std::unordered_map<uint64_t, uint32_t> ordinals_;   // passes opened so far into each kind of target
    std::vector<PassTime> profiled_;   // as recorded; us filled in by passTimes from the stamps
    std::shared_ptr<Buffer> profileStamps_;
    // the counter into stamp slot SLOT (two a pass: start, end), when profiling and there is room
    void stamp(uint32_t slot);
    void stampStart(PassKind kind, uint32_t width, uint32_t height);
    void stampEnd();
    uint32_t skipped_ = 0;
    std::string lastSkipped_;
    // dispatch and dispatchIndirect: GROUPS, or (GROUPS null) the counts at OFFSET of ARGS
    bool dispatchAny(const Kernel& kernel, const uint32_t* groups, const Buffer* args, uint64_t offset, const uint32_t* local, const Bindings& bindings,
                     const uint32_t* immediates, const compute::Options& options);
    // the end of it, however it ends: the status, the handlers (which see that status), and only then the waiters
    void finish(Status s, bool recycle = true);
    void use(const Buffer& b, bool write) { uses_.push_back(BoUse{&b.bo(), write}); }
public:
    // What each register written in a pass holds, for appendDeduped: a draw writes some 500 of them and almost all
    // hold their value already, so each is looked up once, in an open-addressed table of its own (std::unordered_map's
    // find and insert, two lookups and a node a register, were most of a draw's 20 us of deduplication on the phone)
    class RegShadow {
    public:
        // whether REG held VALUE; it holds VALUE from now on
        bool hold(uint32_t reg, uint32_t value) {
            if (keys_.empty() || 2 * (used_ + 1) > keys_.size()) grow();
            const size_t mask = keys_.size() - 1;
            for (size_t i = (reg * 2654435761u) & mask;; i = (i + 1) & mask) {
                if (keys_[i] == reg) {
                    const bool same = values_[i] == value;
                    values_[i] = value;
                    return same;
                }
                if (keys_[i] == kEmpty) {
                    keys_[i] = reg;
                    values_[i] = value;
                    used_++;
                    return false;
                }
            }
        }
        void clear() {
            std::fill(keys_.begin(), keys_.end(), kEmpty);
            used_ = 0;
        }
    private:
        static constexpr uint32_t kEmpty = ~0u;
        void grow() {
            std::vector<uint32_t> keys = std::move(keys_), values = std::move(values_);
            keys_.assign(std::max<size_t>(1024, keys.size() * 2), kEmpty);
            values_.assign(keys_.size(), 0);
            used_ = 0;
            for (size_t i = 0; i < keys.size(); i++) {
                if (keys[i] != kEmpty) hold(keys[i], values[i]);
            }
        }
        std::vector<uint32_t> keys_, values_;
        size_t used_ = 0;
    };

private:
    // the pass draws are joining (render.cpp: draw), closed by its tail before anything else is recorded or it is submitted
    struct OpenPass {
        RenderPass pass;
        render::Options options;
        uint64_t table;   // the GPU address of the pass's table: a piece (below), kept by the command buffer
        RegShadow regs;   // what each register written in the pass holds (render.cpp: appendDeduped)
        std::vector<uint8_t> staticKey;   // what decided the last draw's program-and-pass state (render.cpp: draw)
        // A pass drawn in GMEM (gmemPlan and every draw gmemDrawOk) is recorded rather than streamed: its draws gather
        // in BODIES, deduplicated among themselves from nothing (BODY_REGS) since each tile replays them after the
        // tile's own register writes, and closePass puts them in the tiles.
        bool gmem = false;
        render::GmemPlan plan;
        pm4::Stream bodies;
        RegShadow bodyRegs;
        // what the draws touch: the render area (X0 Y0 X1 Y1, the union of render::drawArea's) and the attachments they
        // write and read (a framebuffer fetch), as masks -- what closePass tiles, loads and stores
        uint32_t area[4] = {~0u, ~0u, 0, 0};
        uint32_t written = 0, read = 0;
        // closed for a draw the tiles cannot replay, and the same pass going on in sysmem after it: what the tiles wrote
        // is stored whatever the store action, as the rest of the pass draws on over it (a DontCare or memoryless
        // attachment is undefined after the pass, not in the middle of it: CoreAnimation's path mask)
        bool goesOn = false;
        // Both ways at once (setGmemMinDraws): the draws gathered for tiles in BODIES and, beside them, as a sysmem pass in
        // SYS_STREAM (its head included, deduplicated through REGS), the GMEM head held in HEAD_GMEM; closePass takes the
        // one the pass's draws call for, now that they are known
        bool dual = false;
        pm4::Stream sysStream, headGmem;
        uint32_t clears = 0;        // the attachments a GMEM pass clears in its tiles (render::gmemClearWords), and their words
        uint32_t clearWords[kMaxColorOutputs][4] = {};
        uint64_t kind = 0;          // render::tuneKeyOf the pass and its ordinal in the buffer among passes of that kind
        uint32_t ordinal = 0, draws = 0;
        uint32_t fetches = 0;       // its draws that read their attachments (a framebuffer fetch): gmemWeight
        uint32_t missed = 0;        // attachments a draw needed that the plan had no bin for (GmemPlan::unplanned): it ended here
        bool lastFetched = false;   // the last draw read its attachments (render::encodeInPass's AFTER_FETCH for the next)
    };
    void closePass();
    // A pass's record, the one closed last kept with its streams' and shadows' memory for the next pass of this buffer:
    // made anew for every pass, its streams grew from nothing to a pass's size each time, and the frees of the large ones
    // went back to the system -- madvise was 8-13% of the window server's time inside draw calls (metal_fps=sample,
    // logs/204443-run.log)
    std::unique_ptr<OpenPass> takePass();
    void retirePass();
    std::unique_ptr<OpenPass> sparePass_;
    void useAttachments(const RenderPass& pass);
    std::unique_ptr<OpenPass> open_;
    // A piece of a larger buffer, for a draw's or a dispatch's table and a program's words. A buffer of its own for each
    // was three requests to the kernel for every draw, which on the phone cost about 200 us a draw (probe 9r) while the
    // GPU spent 3 us on it. A piece is the command buffer's: written once, used for writing, kept until completion.
    struct Piece {
        std::shared_ptr<Buffer> buffer;
        uint64_t offset = 0;
        uint64_t gpuAddress() const { return buffer->gpuAddress() + offset; }
        uint8_t* contents() const { return static_cast<uint8_t*>(buffer->contents()) + offset; }
    };
    bool piece(uint64_t bytes, Piece* out, std::string* why);
    std::shared_ptr<Buffer> slab_;
    uint64_t slabUsed_ = 0;
    std::vector<std::shared_ptr<Buffer>> slabs_;   // every slab this buffer took, given back to the queue when it is done
    // A program's words where they are already in this command buffer, so that every draw with the program does not
    // upload them again. Found by the caller's pointer, but taken only when the words still look the same: the pointer
    // is the caller's to free and reuse. They are compared at every 64th word and the last, not all: the whole compare,
    // once a draw, was 7-10 us of a draw's 50 on the phone, and CoreAnimation's uber shader is 31 000 words. (The copy
    // compared is a CPU one: reading the piece back is uncached memory.)
    struct Uploaded {
        std::vector<uint32_t> words;
        uint64_t gpuAddress;
    };
    // the GPU address of K's words in this buffer, uploading them when they are not there; 0 with *WHY when out of memory
    uint64_t programAddress(const Kernel& k, std::string* why);
    std::unordered_multimap<const uint32_t*, Uploaded> uploaded_;

    // The queue, kept alive by every command buffer of it: the last reference to a queue may go inside a completed
    // handler (the plug-in's command buffer releasing its queue there), and the buffer still gives its memory back to
    // the queue after that. Let go (by the waiter, under the queue's lock) when the buffer is quarantined: the queue
    // holds those for its life, and the two holding each other would outlive everyone.
    std::shared_ptr<CommandQueue> owner_;
    CommandQueue& queue_;
    std::atomic<Status> status_{kNotEnqueued};
    std::string error_;
    pm4::Stream stream_;
    // a draw's own words before they join the pass (render.cpp: draw), kept from one draw to the next: made anew for
    // every draw, each grew from nothing a word at a time -- nine allocations for a draw's 350 words, and the phone's
    // allocator hands large frees back to the system (madvise), which the samples of a draw call showed beside them
    pm4::Stream drawBody_, drawBodySys_;
    std::vector<BoUse> uses_;
    std::vector<std::shared_ptr<const Buffer>> kept_;   // the buffers it uses stay alive until it completes
    std::vector<std::shared_ptr<const void>> keptAny_;
    std::vector<std::function<void(CommandBuffer&)>> handlers_;
    std::mutex lock_;
    std::condition_variable done_;
    bool finished_ = false;                             // (under lock_) the handlers have run: waitUntilCompleted returns
    uint32_t fence_ = 0;
    uint64_t faultsAtCommit_ = 0;                       // the GPU's reset count then: one more by completion = given up, not run
    std::unique_ptr<Bo> commands_;
    int64_t committedAt_ = 0, completedAt_ = 0;
};

class CommandQueue : public std::enable_shared_from_this<CommandQueue> {
public:
    // a queue of its own with the driver; nullptr with *why when the driver has none to give. Shared: each of its
    // command buffers holds it too, so it lives until the caller and its last command buffer have let go of it.
    static std::shared_ptr<CommandQueue> make(Gpu& gpu, std::string* why);
    ~CommandQueue();

    std::shared_ptr<CommandBuffer> commandBuffer();
    // the number of command buffers held in quarantine: ones whose wait timed out with the GPU neither finishing nor
    // reset, their memory never reused (for tests and diagnostics)
    size_t quarantinedCount();
    Gpu& gpu() { return gpu_; }
    // the buffers kept for command streams, idle (to see that they are given back and used again)
    size_t idleCommandBuffers();
    // What a draw binds at the slot the front end reserves for Metal's null texture (Kernel: KernelTexture::isNull): a
    // 1x1 RGBA8Unorm texture of zeros, made at the first ask, the same for every draw of the queue. Zero is an
    // assumption, not a reading -- nothing reachable says what Apple's null texture samples as (nir_air.h) -- and this
    // is the one place it lives; a probe may write the texel to see which instructions read it. nullptr with *why.
    std::shared_ptr<Texture> nullTexture(std::string* why);
    // Private memory for a stage that wants BYTES a fiber in the PERWAVE layout: one buffer for every program of the
    // queue, as turnip keeps one a device (tu_setup_pvtmem, tu_shader.cc:2132-2190) -- a buffer a pipeline is 4 MiB for
    // 512 bytes a fiber on this GPU -- grown to the next power of two when a program wants more. A grown-out buffer is
    // kept as long as the queue: a draw recorded against it may still be in flight. The buffer and *OUT its layout;
    // nullptr with *why.
    std::shared_ptr<Buffer> privateMemory(uint32_t bytes, bool perWave, PrivateMemory* out, std::string* why);
    // GMEM or sysmem for a pass into the target at KEY (its memory's address), by what the GPU spent on such passes:
    // each mode's time kept as a running average from the passes' own counter stamps (tuned), the cheaper one taken,
    // the other tried again now and then, sysmem tried first and GMEM next -- turnip's autotune, by time instead of samples.
    // GMEM is no gain for a pass that touches a corner of the screen (its tiles load and store the whole target: 23
    // draws of CoreAnimation took 5 ms so, 0.1 ms in sysmem), nor for one of framebuffer fetches (the texture unit
    // reads GMEM at a quarter of the rate it reads memory: probe "full-screen cost").
    // the visibility streams' memory binned GMEM passes share (render::vscBytes): one is enough, the GPU running one
    // submission at a time and each pass's binning before its tiles; nullptr with *why
    std::shared_ptr<Buffer> vscBuffer(std::string* why);
    // K's words in a buffer of their own, made the first time a command buffer of this queue draws with them and kept
    // for the next: copying them into each command buffer was most of a draw's first cost (CoreAnimation's uber shader is
    // 124 KB, into write-combined memory). Found by the words' pointer and taken only when every word is the same -- a
    // freed program's pointer may come back with other words, and then its entry goes. nullptr with *why.
    std::shared_ptr<Buffer> programBuffer(const Kernel& k, std::string* why);
    // A slab for draw tables (CommandBuffer::piece): one a completed command buffer gave back, zeroed as a new one comes,
    // or a new one. The kernel's making of a 256 KB buffer, once every 32 draws, was most of a draw's CPU time on the
    // phone (40 us a draw on a fast core, where the same encoding on the host takes 5).
    std::shared_ptr<Buffer> takeSlab(uint64_t bytes, std::string* why);
    void giveSlabs(std::vector<std::shared_ptr<Buffer>>* slabs);
    // ...and any other buffer a command buffer makes for itself (a large piece, its counter stamps): BYTES rounded up to
    // a power of two from 4 KB, zeroed; given back the same way, 32 MB of them kept at most
    std::shared_ptr<Buffer> takeBuffer(uint64_t bytes, std::string* why);
    void giveBuffer(std::shared_ptr<Buffer> b);
    // whether GMEM passes may still be binned: no pass's visibility streams have overrun their pitch (render::kVscOverflowAt,
    // read by checkVsc once a buffer with a binned pass has completed)
    bool binningOk() const { return !vscOverflowed_.load(); }
    void checkVsc();
    bool preferGmem(uint64_t key);
    void tuned(uint64_t key, bool gmem, double us);
    // What tells passes into one kind of target apart: the draws the pass at that place in a command buffer (the Nth into
    // this kind of target) had the last time, which preferGmem's key takes in as a class (a frame of CoreAnimation is
    // the same passes in the same order as the one before it, light or heavy). 0 when not seen yet.
    uint32_t lastDraws(uint64_t kind, uint32_t ordinal);
    // ...and the most it had in the times since its peak, the peak let go after 16 times below it: a pass that is heavy
    // only now and then (CoreAnimation's: light frames between the heavy ones of an animation) counts as heavy
    uint32_t peakDraws(uint64_t kind, uint32_t ordinal);
    void noteDraws(uint64_t kind, uint32_t ordinal, uint32_t draws);
    // ...and the attachments it touched (a mask) in any of its last kLikelyTimes times drawn for tiles: those its GMEM is
    // shared among as it opens (render::gmemPlan's LIKELY). ~0u when not seen yet: all of them. An attachment touched
    // now and then keeps its bin, and one touched for the first time ends its pass there once (GmemPlan::unplanned).
    static constexpr uint32_t kLikelyTimes = 16;
    uint32_t likelyTouched(uint64_t kind, uint32_t ordinal);
    void noteTouched(uint64_t kind, uint32_t ordinal, uint32_t touched);


private:
    friend class CommandBuffer;
    CommandQueue(Gpu& gpu, uint32_t id) : hold_(gpu), gpu_(gpu), id_(id) {}
    void submit(std::shared_ptr<CommandBuffer> cb);
    void waiter(const std::shared_ptr<bool>& orphaned);
    std::unique_ptr<Bo> takeCommandBo(size_t bytes, int* error);

    Gpu::QueueHold hold_;   // first: let go after every other member, the last of them to touch the Gpu
    Gpu& gpu_;
    std::shared_ptr<Texture> null_;   // (under lock_) nullTexture's
    std::shared_ptr<Buffer> vsc_;     // (under lock_) vscBuffer's
    // (under poolLock_, a lock of its own: a command buffer gives its stamps back as it is destroyed, which can be under
    // lock_ -- the queue letting go of the last one in flight -- and lock_ there was the window server hung at boot)
    std::mutex poolLock_;
    std::unordered_map<uint64_t, std::vector<std::shared_ptr<Buffer>>> freeBuffers_;   // by size: giveSlabs', giveBuffer's
    uint64_t freeBytes_ = 0;
    struct Resident { std::vector<uint32_t> words; std::shared_ptr<Buffer> buffer; };
    std::unordered_multimap<const uint32_t*, Resident> resident_;   // (under lock_) programBuffer's
    uint64_t residentBytes_ = 0;
    std::atomic<bool> vscOverflowed_{false};
    std::vector<std::shared_ptr<Buffer>> private_[2];   // (under lock_) privateMemory's, by layout, the last the current one
    uint32_t privatePerFiber_[2] = {0, 0};
    uint32_t id_;
    std::mutex lock_;
    struct Tune { double us[2] = {0, 0}; uint32_t runs[2] = {0, 0}; uint32_t passes = 0; };
    std::unordered_map<uint64_t, Tune> tune_;   // (under lock_) preferGmem's, by target
    std::unordered_map<uint64_t, uint32_t> lastDraws_;   // (under lock_) noteDraws', by kind and ordinal
    struct Peak { uint32_t draws = 0, below = 0; };
    std::unordered_map<uint64_t, Peak> peakDraws_;        // (under lock_) noteDraws' too: peakDraws'
    // (under lock_) noteTouched's, by kind and ordinal: how many times noted, and for each attachment the count at the
    // last time it was touched (0: never)
    struct Touched { uint32_t times = 0; uint32_t at[kMaxColorOutputs] = {}; };
    std::unordered_map<uint64_t, Touched> touched_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<CommandBuffer>> inFlight_;
    // (under lock_) command buffers whose wait timed out with the GPU neither finishing nor reset: it may still be
    // reading their memory, so they are held here for the queue's life -- never freed, never returned to a pool
    std::vector<std::shared_ptr<CommandBuffer>> quarantined_;
    std::vector<std::unique_ptr<Bo>> idle_;
    // (under lock_) command buffers' streams' memory, given back at submit for the next command buffer (pm4::Stream::release)
    std::vector<std::vector<uint32_t>> spareStreams_;
    bool quit_ = false;
    // set by the destructor when it runs on the waiter's own thread (the last reference let go there), which it cannot
    // join: the thread is detached and the waiter, which holds this flag and not the queue, returns at once
    std::shared_ptr<bool> orphaned_ = std::make_shared<bool>(false);
    std::thread thread_;
};

}  // namespace hakios::metal
