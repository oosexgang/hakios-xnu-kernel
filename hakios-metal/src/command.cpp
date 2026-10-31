#include "hakios-metal/command.hpp"
#include "hakios-metal/texture.hpp"

#include <algorithm>
#include <cerrno>
#include <map>
#include <mutex>
#include <cstring>

namespace hakios::metal {

namespace {
constexpr size_t kCommandBo = 64 * 1024;           // one command buffer's packets, at most (grown when needed)
constexpr int64_t kPatienceNs = 5000000000;        // a fence not passed in 5 s: the GPU is hung, the buffer is an error
constexpr uint32_t kFillHead = 64;                 // words written by CP_MEM_WRITE before the copies double them
// The most one CP_MEMCPY moves. On the phone (27 October) 125 000 words in one packet took 4.2 ms, and 4 Mi words (16 MB)
// in one packet hung the GPU ("no answer" from the kernel's driver, and the system after it). Cut into packets of this
// size, each waited for before the next, the same copy is a row of small ones.
constexpr uint32_t kMemcpyMax = 64 * 1024;
}  // namespace

void appendStream(pm4::Stream& to, const pm4::Stream& from);   // render.cpp's: a pass of its own onto the buffer's stream

namespace {
std::mutex gAddressLock;
std::map<uint64_t, std::weak_ptr<Buffer>> gByAddress;   // by gpuAddress, every Buffer newBuffer made and still alive

std::shared_ptr<Buffer> addressed(Buffer* raw) {
    const uint64_t at = raw->gpuAddress();
    std::shared_ptr<Buffer> b(raw, [at](Buffer* x) {
        {
            std::lock_guard<std::mutex> g(gAddressLock);
            auto it = gByAddress.find(at);
            if (it != gByAddress.end() && it->second.expired()) gByAddress.erase(it);
        }
        delete x;
    });
    std::lock_guard<std::mutex> g(gAddressLock);
    gByAddress[at] = b;
    return b;
}
}  // namespace

std::shared_ptr<Buffer> bufferAt(uint64_t address, uint64_t* offset) {
    std::lock_guard<std::mutex> g(gAddressLock);
    auto it = gByAddress.upper_bound(address);
    if (it == gByAddress.begin()) return nullptr;
    --it;
    std::shared_ptr<Buffer> b = it->second.lock();
    if (b == nullptr || address - it->first >= b->length()) return nullptr;
    *offset = address - it->first;
    return b;
}

std::shared_ptr<Buffer> newBuffer(Gpu& gpu, uint64_t length, std::string* why) {
    int error = 0;
    std::unique_ptr<Bo> bo = gpu.newBo(length, kBoWriteCombined, &error);
    if (bo == nullptr) {
        *why = "the driver gave no buffer of " + std::to_string(length) + " bytes: " + strerror(error);
        return nullptr;
    }
    return addressed(new Buffer(std::move(bo), length));
}

namespace {
// One slab is 32 draw tables. Pieces are cut at 256 bytes, which is more than anything in them wants (128 for a
// program's preload and the border colour, 64 for a texture descriptor), and a piece of a page or more starts on a
// page, so that a table lies in memory as it did in a buffer of its own.
constexpr uint64_t kSlabBytes = 256 * 1024;
constexpr uint64_t kPieceAlign = 256;
constexpr uint64_t kPageAlign = 4096;
}  // namespace

bool CommandBuffer::piece(uint64_t bytes, Piece* out, std::string* why) {
    bytes = (std::max<uint64_t>(bytes, 1) + kPieceAlign - 1) & ~(kPieceAlign - 1);
    const uint64_t align = bytes >= kPageAlign ? kPageAlign : kPieceAlign;
    if (slab_ != nullptr) {
        slabUsed_ = std::min(kSlabBytes, (slabUsed_ + align - 1) & ~(align - 1));
    }
    if (bytes > kSlabBytes / 4) {
        // (a piece this large in a slab would leave most of the slab unused when the next one did not fit)
        out->buffer = newBuffer(queue_.gpu(), bytes, why);
        out->offset = 0;
    } else {
        if (slab_ == nullptr || kSlabBytes - slabUsed_ < bytes) {
            slab_ = newBuffer(queue_.gpu(), kSlabBytes, why);
            slabUsed_ = 0;
        }
        out->buffer = slab_;
        out->offset = slabUsed_;
        if (slab_ != nullptr) {
            slabUsed_ += bytes;
        }
    }
    if (out->buffer == nullptr) {
        return false;
    }
    if (out->offset == 0) {
        use(*out->buffer, true);
        keep(out->buffer);
    }
    return true;
}

uint64_t CommandBuffer::programAddress(const Kernel& k, std::string* why) {
    const uint32_t n = std::max<uint32_t>(k.numWords, 1);
    for (auto& [ptr, u] : uploaded_) {
        if (ptr == k.words && u.words.size() == k.numWords && std::memcmp(u.words.data(), k.words, 4ull * k.numWords) == 0) {
            return u.gpuAddress;
        }
    }
    Piece p;
    if (!piece(4ull * n, &p, why)) {
        return 0;
    }
    memcpy(p.contents(), k.words, 4ull * k.numWords);
    uploaded_.emplace_back(k.words, Uploaded{std::vector<uint32_t>(k.words, k.words + k.numWords), p.gpuAddress()});
    return p.gpuAddress();
}

namespace {
// the live surface buffers, by (GPU, kernel handle): the kernel gives this client one handle per surface, so two
// Buffers over it would close it twice. The deleter takes the lock too, so that a handle the kernel hands out again
// cannot be met by a Buffer still being destroyed.
std::mutex gSurfaceLock;
std::map<std::pair<const Gpu*, uint32_t>, std::weak_ptr<Buffer>> gSurfaceBuffers;
}  // namespace

std::shared_ptr<Buffer> surfaceBuffer(Gpu& gpu, uint32_t surfaceId, SurfaceInfo* info, std::string* why) {
    std::lock_guard<std::mutex> g(gSurfaceLock);
    int error = 0;
    std::unique_ptr<Bo> bo = gpu.surfaceBo(surfaceId, info, &error);
    if (bo == nullptr) {
        *why = "IOSurface " + std::to_string(surfaceId) + ": the driver gave no buffer for it: " +
               (error == ENOENT ? std::string("no such surface") : error == ENOTSUP ? std::string("a surface around a program's own memory is not one the GPU takes") : strerror(error));
        return nullptr;
    }
    const auto key = std::make_pair(static_cast<const Gpu*>(&gpu), bo->handle());
    auto live = gSurfaceBuffers.find(key);
    if (live != gSurfaceBuffers.end()) {
        if (std::shared_ptr<Buffer> b = live->second.lock()) {
            gpu.disown(std::move(bo));   // the same handle as the live Buffer's Bo: closed once, by that Bo
            return b;
        }
    }
    const uint64_t length = bo->size();
    std::shared_ptr<Buffer> b(new Buffer(std::move(bo), length), [key](Buffer* x) {
        std::lock_guard<std::mutex> g(gSurfaceLock);
        auto it = gSurfaceBuffers.find(key);
        if (it != gSurfaceBuffers.end() && it->second.expired()) gSurfaceBuffers.erase(it);
        delete x;
    });
    gSurfaceBuffers[key] = b;
    return b;
}

// --- CommandBuffer ----------------------------------------------------------------------------------------------

bool CommandBuffer::fail(const std::string& why) {
    if (error_.empty()) {
        error_ = why;
    }
    return false;
}

void CommandBuffer::stamp(uint32_t slot) {
    if (profileStamps_ == nullptr) {
        std::string why;
        profileStamps_ = newBuffer(queue_.gpu(), 16 * kMaxProfiled, &why);
        if (profileStamps_ == nullptr) {
            profile_ = tuning_ = false;
            return;
        }
        memset(profileStamps_->contents(), 0, 16 * kMaxProfiled);
        keep(profileStamps_);
    }
    const uint64_t at = profileStamps_->gpuAddress() + 8ull * slot;
    const uint32_t w[3] = {a6xx::CP_REG_TO_MEM_0_REG(0x0980) | a6xx::CP_REG_TO_MEM_0_CNT(2) | a6xx::CP_REG_TO_MEM_0_64B, uint32_t(at),
                           uint32_t(at >> 32)};
    stream_.packet(a6xx::CP_REG_TO_MEM, w, 3);
    use(*profileStamps_, true);
}

std::shared_ptr<Buffer> CommandQueue::vscBuffer(std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    if (vsc_ == nullptr) vsc_ = newBuffer(gpu_, render::vscBytes(), why);
    return vsc_;
}

bool CommandQueue::preferGmem(uint64_t key) {
    std::lock_guard<std::mutex> g(lock_);
    Tune& t = tune_[key];
    t.passes++;
    // (sysmem first: a kind of pass seen once may not come again, and an untried GMEM pass is the dearer gamble -- 5 ms
    // against 0.13 for a light one)
    if (t.runs[0] == 0) return false;
    if (t.runs[1] == 0) return true;
    const bool better = t.us[1] < t.us[0];
    // (the other mode again now and then, what a pass costs drifting: every 16th pass, or every 64th when the one is
    // dearer by half -- a light pass of CoreAnimation took 5 ms in GMEM against 0.13 in sysmem)
    const double ratio = better ? t.us[0] / t.us[1] : t.us[1] / t.us[0];
    return t.passes % (ratio > 1.5 ? 64 : 16) == 0 ? !better : better;
}

namespace {
uint64_t kindKey(uint64_t kind, uint32_t ordinal) { return kind * 0x100000001b3ull ^ ordinal; }
}  // namespace

uint32_t CommandQueue::lastDraws(uint64_t kind, uint32_t ordinal) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = lastDraws_.find(kindKey(kind, ordinal));
    return it == lastDraws_.end() ? 0 : it->second;
}

void CommandQueue::noteDraws(uint64_t kind, uint32_t ordinal, uint32_t draws) {
    std::lock_guard<std::mutex> g(lock_);
    lastDraws_[kindKey(kind, ordinal)] = draws;
}

void CommandQueue::tuned(uint64_t key, bool gmem, double us) {
    if (!(us > 0)) return;
    std::lock_guard<std::mutex> g(lock_);
    Tune& t = tune_[key];
    double& avg = t.us[gmem ? 1 : 0];
    avg = t.runs[gmem ? 1 : 0]++ == 0 ? us : 0.75 * avg + 0.25 * us;
}

void CommandBuffer::stampStart(PassKind kind, uint32_t width, uint32_t height) {
    if (!(profile_ || tuning_) || profiled_.size() >= kMaxProfiled) return;
    profiled_.push_back(PassTime{kind, width, height, 0, 0, 0});
    stamp(uint32_t(2 * (profiled_.size() - 1)));
}

void CommandBuffer::stampEnd() {
    if (!(profile_ || tuning_) || profiled_.empty()) return;
    stamp(uint32_t(2 * (profiled_.size() - 1) + 1));
}

void CommandBuffer::drawStamp(uint32_t slot) {
    if (drawStamps_ == nullptr) {
        std::string why;
        drawStamps_ = newBuffer(queue_.gpu(), 16 * kMaxProfiledDraws, &why);
        if (drawStamps_ == nullptr) {
            profileDraws_ = false;
            return;
        }
        memset(drawStamps_->contents(), 0, 16 * kMaxProfiledDraws);
        keep(drawStamps_);
    }
    const uint64_t at = drawStamps_->gpuAddress() + 8ull * slot;
    stream_.waitForIdle();
    const uint32_t w[3] = {a6xx::CP_REG_TO_MEM_0_REG(0x0980) | a6xx::CP_REG_TO_MEM_0_CNT(2) | a6xx::CP_REG_TO_MEM_0_64B, uint32_t(at),
                           uint32_t(at >> 32)};
    stream_.packet(a6xx::CP_REG_TO_MEM, w, 3);
    use(*drawStamps_, true);
}

std::vector<CommandBuffer::DrawTime> CommandBuffer::drawTimes() const {
    std::vector<DrawTime> out = drawsProfiled_;
    if (drawStamps_ == nullptr) return {};
    const auto* t = static_cast<const uint64_t*>(drawStamps_->contents());
    for (size_t i = 0; i < out.size(); i++) {
        out[i].us = t[2 * i + 1] > t[2 * i] ? double(t[2 * i + 1] - t[2 * i]) / 19.2 : 0.0;
    }
    return out;
}

std::vector<CommandBuffer::PassTime> CommandBuffer::passTimes() const {
    std::vector<PassTime> out = profiled_;
    if (profileStamps_ == nullptr) return {};
    const auto* t = static_cast<const uint64_t*>(profileStamps_->contents());
    for (size_t i = 0; i < out.size(); i++) {
        out[i].us = t[2 * i + 1] > t[2 * i] ? double(t[2 * i + 1] - t[2 * i]) / 19.2 : 0.0;
    }
    return out;
}

bool CommandBuffer::refuseWork(const std::string& why) {
    if (!skipRefused_) {
        return fail(why);
    }
    skipped_++;
    lastSkipped_ = why;
    return false;
}

void CommandBuffer::addCompletedHandler(std::function<void(CommandBuffer&)> handler) {
    if (status_ <= kEnqueued) {
        handlers_.push_back(std::move(handler));
    }
}

void CommandBuffer::enqueue() {
    Status expected = kNotEnqueued;
    status_.compare_exchange_strong(expected, kEnqueued);
}

void CommandBuffer::commit() {
    if (status_ > kEnqueued) {
        return;  // Metal asserts here; committing twice does nothing at all in this driver
    }
    queue_.submit(shared_from_this());
}

void CommandBuffer::waitUntilCompleted() {
    std::unique_lock<std::mutex> g(lock_);
    // (a buffer never committed is not waited for: Metal would wait forever, which helps nobody)
    done_.wait(g, [this] { return finished_ || status_ <= kEnqueued; });
}

void CommandBuffer::finish(Status s) {
    status_ = s;
    if (s == kCompleted && tuning_) {
        for (const PassTime& t : passTimes()) {
            if (t.tuneKey != 0) queue_.tuned(t.tuneKey, t.tiles != 0, t.us);
        }
    }
    for (auto& h : handlers_) {
        h(*this);
    }
    handlers_.clear();
    kept_.clear();
    keptAny_.clear();
    {
        std::lock_guard<std::mutex> g(lock_);
        finished_ = true;
    }
    done_.notify_all();
}

bool CommandBuffer::copy(const Buffer& from, uint64_t fromOffset, const Buffer& to, uint64_t toOffset, uint64_t length) {
    closePass();
    if (status_ > kEnqueued) {
        return fail("copy after commit");
    }
    if (fromOffset > from.length() || length > from.length() - fromOffset || toOffset > to.length() ||
        length > to.length() - toOffset) {
        return fail("copy outside a buffer");
    }
    if (length == 0) {
        return true;
    }
    if (fromOffset % 4 || toOffset % 4 || length % 4) {
        // the command processor moves words; a copy of bytes goes through the 2D engine instead (a pass of its own).
        // MapKit's snapshot service copies rows of 2 bytes into its textures.
        std::string why;
        Piece table;
        if (!piece(render::kTableBytes, &table, &why)) {
            return fail(std::string("no memory for the copy's table: ") + why);
        }
        pm4::Stream added;
        render::encodeBytes(added, from.gpuAddress() + fromOffset, to.gpuAddress() + toOffset, length, table.gpuAddress(), table.contents());
        appendStream(stream_, added);
        use(from, false);
        use(to, true);
        return true;
    }
    uint64_t words = length / 4, done = 0;
    while (done < words) {
        uint32_t n = uint32_t(std::min<uint64_t>(words - done, kMemcpyMax));
        stream_.memcpy(to.gpuAddress() + toOffset + 4 * done, from.gpuAddress() + fromOffset + 4 * done, n);
        // what comes after may read what this wrote: the copy lands before the next packet is taken
        stream_.waitMemWrites();
        stream_.waitForMe();
        done += n;
    }
    use(from, false);
    use(to, true);
    return true;
}

bool CommandBuffer::fill(const Buffer& b, uint64_t offset, uint64_t length, uint8_t value) {
    closePass();
    if (status_ > kEnqueued) {
        return fail("fill after commit");
    }
    if (offset % 4 || length % 4) {
        return fail("fill of " + std::to_string(length) + " bytes at " + std::to_string(offset) +
                    ": the command processor writes words, so both must be multiples of 4");
    }
    if (offset > b.length() || length > b.length() - offset) {
        return fail("fill outside the buffer");
    }
    if (length == 0) {
        return true;
    }
    uint32_t word = uint32_t(value) * 0x01010101u;
    uint64_t words = length / 4, base = b.gpuAddress() + offset;
    // the first words written as they are, ...
    uint32_t head = uint32_t(std::min<uint64_t>(words, kFillHead));
    std::vector<uint32_t> pattern(head, word);
    stream_.memWrite(base, pattern.data(), head);
    stream_.waitMemWrites();
    stream_.waitForMe();
    // ... and the rest copied from what is already filled, twice as much each time
    uint64_t have = head;
    while (have < words) {
        uint32_t n = uint32_t(std::min<uint64_t>(std::min<uint64_t>(have, words - have), kMemcpyMax));
        stream_.memcpy(base + 4 * have, base, n);
        stream_.waitMemWrites();
        stream_.waitForMe();
        have += n;
    }
    use(b, true);
    return true;
}

// --- CommandQueue -----------------------------------------------------------------------------------------------

std::unique_ptr<CommandQueue> CommandQueue::make(Gpu& gpu, std::string* why) {
    uint32_t id = 0;
    int e = gpu.newQueue(0, &id);
    if (e != 0) {
        *why = std::string("the driver gave no submission queue: ") + strerror(e);
        return nullptr;
    }
    std::unique_ptr<CommandQueue> q(new CommandQueue(gpu, id));
    q->thread_ = std::thread([p = q.get()] { p->waiter(); });
    return q;
}

CommandQueue::~CommandQueue() {
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    idle_.clear();
    gpu_.closeQueue(id_);
}

std::shared_ptr<CommandBuffer> CommandQueue::commandBuffer() {
    return std::shared_ptr<CommandBuffer>(new CommandBuffer(*this));
}

std::shared_ptr<Texture> CommandQueue::nullTexture(std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    if (null_ == nullptr) {
        TextureDesc d;
        d.format = PixelFormat::kRGBA8Unorm;
        std::shared_ptr<Texture> t = Texture::make(gpu_, d, why);
        // (written, not taken as the kernel's fresh zeros: the zero is the claim, so it is put there in so many words)
        const uint32_t zero = 0;
        if (t == nullptr || !t->replace(0, 0, 1, 1, &zero, 4, why)) return nullptr;
        null_ = std::move(t);
    }
    return null_;
}

namespace {
// the A635's SP cores and fibers an SP (freedreno_devices.py, the 7c+ Gen 3 entry: num_ccu 2, fibers_per_sp 128 * 2 * 16)
constexpr uint32_t kSpCores = 2;
constexpr uint32_t kFibersPerSp = 128 * 2 * 16;
}  // namespace

std::shared_ptr<Buffer> CommandQueue::privateMemory(uint32_t bytes, bool perWave, PrivateMemory* out, std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    const int l = perWave ? 1 : 0;
    if (privatePerFiber_[l] < bytes) {
        uint32_t perFiber = 512;
        while (perFiber < bytes) perFiber *= 2;
        const uint64_t perSp = (uint64_t(perFiber) * kFibersPerSp + 4095) & ~uint64_t(4095);
        std::shared_ptr<Buffer> b = newBuffer(gpu_, perSp * kSpCores, why);
        if (b == nullptr) return nullptr;
        private_[l].push_back(std::move(b));
        privatePerFiber_[l] = perFiber;
    }
    const uint32_t perFiber = privatePerFiber_[l];
    out->address = private_[l].back()->gpuAddress();
    out->perFiber = perFiber;
    out->perSp = uint32_t((uint64_t(perFiber) * kFibersPerSp + 4095) & ~uint64_t(4095));
    out->perWave = perWave;
    return private_[l].back();
}

size_t CommandQueue::idleCommandBuffers() {
    std::lock_guard<std::mutex> g(lock_);
    return idle_.size();
}

std::unique_ptr<Bo> CommandQueue::takeCommandBo(size_t bytes, int* error) {
    {
        std::lock_guard<std::mutex> g(lock_);
        for (auto it = idle_.begin(); it != idle_.end(); ++it) {
            if ((*it)->size() >= bytes) {
                std::unique_ptr<Bo> bo = std::move(*it);
                idle_.erase(it);
                return bo;
            }
        }
    }
    return gpu_.newBo(std::max(bytes, kCommandBo), kBoGpuReadOnly | kBoWriteCombined, error);
}

void CommandQueue::submit(std::shared_ptr<CommandBuffer> cb) {
    cb->committedAt_ = gpu_.transport().nowNs();
    if (!cb->error_.empty()) {
        cb->finish(CommandBuffer::kError);   // a blit that was refused: nothing of it reaches the GPU
        return;
    }
    cb->closePass();   // the last pass's tail
    // every command buffer ends with the GPU idle: what Metal's completion promises, that the work is done
    cb->stream_.waitMemWrites();
    cb->stream_.waitForIdle();
    const std::vector<uint32_t>& w = cb->stream_.words();
    int error = 0;
    cb->commands_ = takeCommandBo(4 * w.size(), &error);
    if (cb->commands_ == nullptr) {
        cb->error_ = std::string("no memory for the command stream: ") + strerror(error);
        cb->finish(CommandBuffer::kError);
        return;
    }
    // the stream is written in order, as write-combined memory wants it, and read back by nobody
    memcpy(cb->commands_->cpu(), w.data(), 4 * w.size());
    std::unique_lock<std::mutex> g(lock_);  // the fences of one queue are handed out in commit order
    cb->status_ = CommandBuffer::kCommitted;
    cb->faultsAtCommit_ = gpu_.faults();
    int e = gpu_.submit(id_, *cb->commands_, 0, uint32_t(w.size()), cb->uses_, &cb->fence_);
    if (e != 0) {
        cb->error_ = std::string("the driver refused the submission: ") + strerror(e);
        idle_.push_back(std::move(cb->commands_));
        g.unlock();                         // (the handlers may commit more: not under the queue's lock)
        cb->finish(CommandBuffer::kError);
        return;
    }
    cb->status_ = CommandBuffer::kScheduled;
    inFlight_.push_back(std::move(cb));
    changed_.notify_all();
}

void CommandQueue::waiter() {
    std::unique_lock<std::mutex> g(lock_);
    for (;;) {
        changed_.wait(g, [this] { return quit_ || !inFlight_.empty(); });
        if (inFlight_.empty()) {
            return;  // quitting, and nothing left in flight
        }
        std::shared_ptr<CommandBuffer> cb = inFlight_.front();
        g.unlock();
        int e = gpu_.wait(id_, cb->fence_, kPatienceNs);
        cb->completedAt_ = gpu_.transport().nowNs();
        // A fence that passed is not yet a submission that ran. When the kernel resets a hung GPU (gpu_recover=1) it
        // gives up everything the GPU had been given, the hung submission and those behind it in the ring, by setting
        // the fence word to the last submission's number -- so their waits end as if they had run (hakios_gpu_recover:
        // "submissions up to N are given up as done"). FAULTS counts the resets; one more since the commit says so.
        uint64_t faults = gpu_.faults();
        bool givenUp = faults > cb->faultsAtCommit_;
        if (e == 0 && givenUp) {
            cb->error_ = "given up by a reset of the GPU (it hung on this submission or on one before it): not run";
            e = EIO;
        } else if (e == ETIMEDOUT) {
            cb->error_ = givenUp ? "the GPU did not finish it in 5 s: it hung, and the kernel has reset it (reset " +
                                       std::to_string(faults) + " of this run)"
                                 : "the GPU did not finish it in 5 s and is not answering; without gpu_recover=1 the "
                                   "kernel waits for nothing again in this run";
        } else if (e != 0) {
            cb->error_ = std::string("waiting for it failed: ") + strerror(e);
        }
        cb->finish(e == 0 ? CommandBuffer::kCompleted : CommandBuffer::kError);
        g.lock();
        inFlight_.pop_front();
        if (cb->commands_ != nullptr) {
            idle_.push_back(std::move(cb->commands_));
        }
    }
}

}  // namespace hakios::metal
