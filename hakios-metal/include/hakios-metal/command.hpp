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
// handle is closed once, by the last owner. nullptr with *why.
std::shared_ptr<Buffer> surfaceBuffer(Gpu& gpu, uint32_t surfaceId, SurfaceInfo* info, std::string* why);

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
    struct Counts { uint32_t passes = 0, draws = 0, dispatches = 0, copies = 0, clears = 0, gmemPasses = 0, tiles = 0; };   // (gmemPasses of the passes drawn in tiles)
    const Counts& counts() const { return counts_; }
    // With profiling on, the command processor writes its always-on counter (CP_ALWAYS_ON_COUNTER, 19.2 MHz) at the start
    // of each pass and after its tail -- whose wait for idle makes the end the end -- for the first kMaxProfiled passes:
    // what each costs, beside what it was (a draw pass with its target's size and its draws, a copy, a clear, a
    // dispatch). Read once the buffer has completed. metal_fps names a frame's slowest passes with it.
    enum PassKind : uint32_t { kPassDraw = 0, kPassCopy = 1, kPassClear = 2, kPassDispatch = 3 };
    struct PassTime { PassKind kind; uint32_t width, height, draws; double us; uint32_t tiles = 0; uint64_t tuneKey = 0; };   // tiles 0: in sysmem; tuneKey: CommandQueue::preferGmem's
    static constexpr uint32_t kMaxProfiled = 64;
    void setProfile(bool on) { profile_ = on; }
    // ...and each draw of a sysmem pass on its own, the first kMaxProfiledDraws: a wait for idle before it and after it,
    // the counter at both (so the draws no longer overlap: what each costs alone), with its program's name. Which
    // draws of a heavy frame are heavy is read off these (metal_fps=draws).
    struct DrawTime { std::string name; double us; uint32_t pass = 0, area[4] = {}; };   // PASS: its pass among passTimes'; AREA render::drawArea's
    static constexpr uint32_t kMaxProfiledDraws = 512;
    void setProfileDraws(bool on) { profileDraws_ = on; }
    std::vector<DrawTime> drawTimes() const;
    // render::Options::gmem for this buffer's draws besides each draw's own (both must allow it): the C API's switch
    void setGmem(bool on) { gmem_ = on; }
    // GMEM's refinements off, for a bisect on the phone: kGmemWholeArea tiles the whole window whatever the draws touch,
    // kGmemEveryAttachment loads and stores every attachment, kGmemFetchBarrier orders a fetch by the barrier
    enum : uint32_t { kGmemWholeArea = 1, kGmemEveryAttachment = 2, kGmemFetchBarrier = 4 };
    void setGmemOff(uint32_t bits) { gmemOff_ = bits; }
    // GMEM or sysmem for each pass by CommandQueue::preferGmem (each pass stamped for it) rather than GMEM whenever a
    // pass can have it: what the plug-in asks; off by default, so that a test gets the mode it asks for
    void setGmemTuned(bool on) { tuning_ = on; }
    // GMEM for a pass of at least DRAWS draws, sysmem for one of fewer; 0: not by the draws. The draws are counted as
    // they come, so the pass is recorded both ways until it closes. SpringBoard's passes of 63 to 161 draws took 13 to
    // 22 ms in GMEM against 17 to 31 in sysmem, its passes of 45 to 49 draws 8 to 10 against 6 to 8, its light ones 5
    // against 0.13.
    void setGmemMinDraws(uint32_t draws) { gmemMinDraws_ = draws; }
    // render::Options::gmemBinning for this buffer's draws besides each draw's own (either asks for it)
    void setGmemBinning(bool on) { gmemBinning_ = on; }
    // why the last pass that could not be drawn in GMEM was not (gmemPlan's or gmemDrawOk's reason), empty when none was refused
    const std::string& gmemRefusal() const { return gmemWhy_; }
    // the words recorded so far (what the command processor will read)
    size_t streamWords() const { return stream_.words().size(); }
    std::vector<PassTime> passTimes() const;

private:
    friend class CommandQueue;
    explicit CommandBuffer(CommandQueue& queue) : queue_(queue) {}
    bool fail(const std::string& why);
    bool skipRefused_ = false;
    Counts counts_;
    bool profile_ = false;
    bool tuning_ = false;     // setGmemTuned's (cleared by a failed stamp buffer too)
    bool profileDraws_ = false;
    std::vector<DrawTime> drawsProfiled_;
    std::shared_ptr<Buffer> drawStamps_;
    void drawStamp(uint32_t slot);
    bool gmem_ = true;
    uint32_t gmemOff_ = 0;
    uint32_t gmemMinDraws_ = 0;
    bool gmemBinning_ = false;
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
    void finish(Status s);
    void use(const Buffer& b, bool write) { uses_.push_back(BoUse{&b.bo(), write}); }
    // the pass draws are joining (render.cpp: draw), closed by its tail before anything else is recorded or it is submitted
    struct OpenPass {
        RenderPass pass;
        render::Options options;
        uint64_t table;   // the GPU address of the pass's table: a piece (below), kept by the command buffer
        std::unordered_map<uint32_t, uint32_t> regs;   // what each register written in the pass holds (render.cpp: appendDeduped)
        // A pass drawn in GMEM (gmemPlan and every draw gmemDrawOk) is recorded rather than streamed: its draws gather
        // in BODIES, deduplicated among themselves from nothing (BODY_REGS) since each tile replays them after the
        // tile's own register writes, and closePass puts them in the tiles.
        bool gmem = false;
        render::GmemPlan plan;
        pm4::Stream bodies;
        std::unordered_map<uint32_t, uint32_t> bodyRegs;
        // what the draws touch: the render area (X0 Y0 X1 Y1, the union of render::drawArea's) and the attachments they
        // write and read (a framebuffer fetch), as masks -- what closePass tiles, loads and stores
        uint32_t area[4] = {~0u, ~0u, 0, 0};
        uint32_t written = 0, read = 0;
        // Both ways at once (setGmemMinDraws): the draws gathered for tiles in BODIES and, beside them, as a sysmem pass in
        // SYS_STREAM (its head included, deduplicated through REGS), the GMEM head held in HEAD_GMEM; closePass takes the
        // one the pass's draws call for, now that they are known
        bool dual = false;
        pm4::Stream sysStream, headGmem;
        uint32_t clears = 0;        // the attachments a GMEM pass clears in its tiles (render::gmemClearWords), and their words
        uint32_t clearWords[kMaxColorOutputs][4] = {};
        uint64_t kind = 0;          // render::tuneKeyOf the pass and its ordinal in the buffer among passes of that kind
        uint32_t ordinal = 0, draws = 0;
        bool lastFetched = false;   // the last draw read its attachments (render::encodeInPass's AFTER_FETCH for the next)
    };
    void closePass();
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
    // A program's words where they are already in this command buffer, so that every draw with the program does not
    // upload them again. Found by the caller's pointer, but taken only when the words are still the same: the pointer is
    // the caller's to free and reuse, and the copy compared is a CPU one (reading the piece back is uncached memory).
    struct Uploaded {
        std::vector<uint32_t> words;
        uint64_t gpuAddress;
    };
    // the GPU address of K's words in this buffer, uploading them when they are not there; 0 with *WHY when out of memory
    uint64_t programAddress(const Kernel& k, std::string* why);
    std::vector<std::pair<const uint32_t*, Uploaded>> uploaded_;

    CommandQueue& queue_;
    std::atomic<Status> status_{kNotEnqueued};
    std::string error_;
    pm4::Stream stream_;
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

class CommandQueue {
public:
    // a queue of its own with the driver; nullptr with *why when the driver has none to give
    static std::unique_ptr<CommandQueue> make(Gpu& gpu, std::string* why);
    ~CommandQueue();

    std::shared_ptr<CommandBuffer> commandBuffer();
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
    bool preferGmem(uint64_t key);
    void tuned(uint64_t key, bool gmem, double us);
    // What tells passes into one kind of target apart: the draws the pass at that place in a command buffer (the Nth into
    // this kind of target) had the last time, which preferGmem's key takes in as a class (a frame of CoreAnimation is
    // the same passes in the same order as the one before it, light or heavy). 0 when not seen yet.
    uint32_t lastDraws(uint64_t kind, uint32_t ordinal);
    void noteDraws(uint64_t kind, uint32_t ordinal, uint32_t draws);


private:
    friend class CommandBuffer;
    CommandQueue(Gpu& gpu, uint32_t id) : gpu_(gpu), id_(id) {}
    void submit(std::shared_ptr<CommandBuffer> cb);
    void waiter();
    std::unique_ptr<Bo> takeCommandBo(size_t bytes, int* error);

    Gpu& gpu_;
    std::shared_ptr<Texture> null_;   // (under lock_) nullTexture's
    std::shared_ptr<Buffer> vsc_;     // (under lock_) vscBuffer's
    std::vector<std::shared_ptr<Buffer>> private_[2];   // (under lock_) privateMemory's, by layout, the last the current one
    uint32_t privatePerFiber_[2] = {0, 0};
    uint32_t id_;
    std::mutex lock_;
    struct Tune { double us[2] = {0, 0}; uint32_t runs[2] = {0, 0}; uint32_t passes = 0; };
    std::unordered_map<uint64_t, Tune> tune_;   // (under lock_) preferGmem's, by target
    std::unordered_map<uint64_t, uint32_t> lastDraws_;   // (under lock_) noteDraws', by kind and ordinal
    std::condition_variable changed_;
    std::deque<std::shared_ptr<CommandBuffer>> inFlight_;
    std::vector<std::unique_ptr<Bo>> idle_;
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace hakios::metal
