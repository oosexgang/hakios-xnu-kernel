#include "hakios-metal/fake_gpu.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/pm4.hpp"

namespace hakios::metal {

namespace {
// The fake GPU reads a header with arithmetic of its own and not with pm4::decode: while the two were one function an
// encoder whose parity bit was even passed every test here, and only the comparison with Mesa's formula (pm4_test.cpp)
// said so. The command processor does not share the encoder's code either. Odd parity: the bit is 1 when the field has
// an even number of ones, so that field and bit together have an odd count.
uint32_t oddBit(uint32_t field) { return (__builtin_popcount(field) & 1) ^ 1u; }

pm4::Header readHeader(uint32_t w) {
    pm4::Header h;
    switch (w >> 28) {
    case 4:
        h.count = w & 0x7f;
        h.reg = (w >> 8) & 0x3ffff;
        if (((w >> 7) & 1) == oddBit(h.count) && ((w >> 27) & 1) == oddBit(h.reg)) {
            h.kind = pm4::Header::kRegisters;
        }
        break;
    case 7:
        h.count = w & 0x3fff;
        h.opcode = (w >> 16) & 0x7f;
        if (((w >> 15) & 1) == oddBit(h.count) && ((w >> 23) & 1) == oddBit(h.opcode)) {
            h.kind = pm4::Header::kCommand;
        }
        break;
    default:
        break;
    }
    return h;
}

std::string text(const char* format, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
    char buffer[160];
    snprintf(buffer, sizeof buffer, format, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    return buffer;
}

// a field read back the way the registers hold it, from a6xx.hpp's masks (the fake decodes; the encoder encodes)
uint32_t fieldOf(uint32_t word, uint32_t mask, uint32_t shift) { return (word & mask) >> shift; }
}  // namespace

FakeGpu::FakeGpu(bool async, uint32_t delayUs, bool recovers) : async_(async), recovers_(recovers), delayUs_(delayUs) {
    if (async_) {
        thread_ = std::thread([this] { worker(); });
    }
}

FakeGpu::~FakeGpu() {
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
        paused_ = false;
    }
    changed_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

int64_t FakeGpu::nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

FakeGpu::Stats FakeGpu::stats() {
    std::lock_guard<std::mutex> g(lock_);
    return stats_;
}

uint32_t FakeGpu::liveBuffers() {
    std::lock_guard<std::mutex> g(lock_);
    return uint32_t(buffers_.size());
}

void FakeGpu::pause() {
    std::lock_guard<std::mutex> g(lock_);
    paused_ = true;
}

void FakeGpu::resume() {
    {
        std::lock_guard<std::mutex> g(lock_);
        paused_ = false;
    }
    changed_.notify_all();
}

void* FakeGpu::map(uint32_t handle, uint64_t size) {
    std::lock_guard<std::mutex> g(lock_);
    auto b = buffers_.find(handle);
    if (b == buffers_.end() || size > b->second.bytes.size()) {
        return nullptr;
    }
    return b->second.bytes.data();
}

void FakeGpu::unmap(uint32_t, void*) {}

uint8_t* FakeGpu::at(uint64_t iova, uint64_t length) {
    for (auto& [handle, b] : buffers_) {
        if (iova >= b.iova && length <= b.bytes.size() && iova - b.iova <= b.bytes.size() - length) {
            return b.bytes.data() + (iova - b.iova);
        }
    }
    return nullptr;
}

void FakeGpu::fault(const std::string& why) {
    stats_.faults++;
    stats_.lastFault = why;
}

uint32_t FakeGpu::reg(uint32_t r) const {
    auto it = regs_.find(r);
    return it == regs_.end() ? 0 : it->second;
}

uint64_t FakeGpu::reg64(uint32_t r) const { return reg(r) | uint64_t(reg(r + 1)) << 32; }

// CP_LOAD_STATE6_FRAG: the program, constants or descriptors of the compute stage, from the packet or from memory
bool FakeGpu::loadState(const uint32_t* p, uint32_t count) {
    using namespace a6xx;
    if (count < 3) {
        fault("CP_LOAD_STATE6_FRAG shorter than its three words");
        return false;
    }
    uint32_t w0 = p[0];
    uint32_t dstOff = fieldOf(w0, CP_LOAD_STATE6_0_DST_OFF__MASK, CP_LOAD_STATE6_0_DST_OFF__SHIFT);
    uint32_t type = fieldOf(w0, CP_LOAD_STATE6_0_STATE_TYPE__MASK, CP_LOAD_STATE6_0_STATE_TYPE__SHIFT);
    uint32_t src = fieldOf(w0, CP_LOAD_STATE6_0_STATE_SRC__MASK, CP_LOAD_STATE6_0_STATE_SRC__SHIFT);
    uint32_t block = fieldOf(w0, CP_LOAD_STATE6_0_STATE_BLOCK__MASK, CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT);
    uint32_t units = fieldOf(w0, CP_LOAD_STATE6_0_NUM_UNIT__MASK, CP_LOAD_STATE6_0_NUM_UNIT__SHIFT);
    if (block != SB6_CS_SHADER && block != SB6_CS_TEX) {
        fault(text("CP_LOAD_STATE6_FRAG for state block %llu: not the compute stage's", block));
        return false;
    }
    uint32_t unitBytes = type == ST6_SHADER ? 128 : type == ST6_CONSTANTS ? 16 : type == ST6_UBO ? 8 : 64;
    if (type == ST6_SHADER && units > kInstrCacheSize) {
        fault(text("a program preload of %llu units: 128 hangs this GPU, the instruction cache holds %llu", units, kInstrCacheSize));
        return false;
    }
    uint64_t from = 0;
    if (src == SS6_DIRECT) {
        if (type == ST6_SHADER) {
            fault("a program loaded from the packet itself: not what this GPU model runs");
            return false;
        }
        if (count != 3 + units * unitBytes / 4) {
            fault(text("a direct load of %llu units carries %llu words, not %llu", units, count - 3, units * unitBytes / 4));
            return false;
        }
    } else if (src == SS6_INDIRECT) {
        if (count != 3) {
            fault(text("an indirect load with %llu words after its header, not 3", count));
            return false;
        }
        from = (p[1] & CP_LOAD_STATE6_1_EXT_SRC_ADDR__MASK) | uint64_t(p[2]) << 32;
        if (at(from, uint64_t(units) * unitBytes) == nullptr) {
            fault(text("a load of %llu units from %#llx: outside every buffer", units, from));
            return false;
        }
    } else {
        fault("a bindless or UBO-sourced load: not what this GPU model runs");
        return false;
    }
    switch (type) {
    case ST6_SHADER:
        cs_.programLoaded = true;
        cs_.programFrom = from;
        cs_.programUnits = units;
        break;
    case ST6_CONSTANTS:
        cs_.consts.push_back(ConstLoad{dstOff, units, src == SS6_DIRECT});
        break;
    case ST6_IBO:
        cs_.ibosLoaded = true;
        cs_.ibosFrom = from;
        cs_.iboUnits = units;
        break;
    default:
        break;   // ST6_UBO: taken, not checked further
    }
    return true;
}

// CP_EXEC_CS: the dispatch itself. Everything the registers and the loads say about it has to hold together; what
// the real GPU does with state that does not is hang, and that is what a fault stands for here.
bool FakeGpu::execCs(const uint32_t* p, uint32_t count) {
    using namespace a6xx;
    if (count != 4) {
        fault(text("CP_EXEC_CS of %llu words, not 4", count));
        return false;
    }
    uint32_t config = reg(REG_A6XX_SP_CS_CONFIG), hlsq = reg(REG_A6XX_HLSQ_CS_CNTL);
    if ((config & A6XX_SP_CS_CONFIG_ENABLED) == 0 || (hlsq & A6XX_HLSQ_CS_CNTL_ENABLED) == 0) {
        fault("CP_EXEC_CS with the compute stage not enabled in SP_CS_CONFIG and HLSQ_CS_CNTL");
        return false;
    }
    uint32_t instrlen = reg(REG_A6XX_SP_CS_INSTRLEN);
    uint64_t program = reg64(REG_A6XX_SP_CS_OBJ_START);
    if (instrlen == 0) {
        fault("CP_EXEC_CS with SP_CS_INSTRLEN 0");
        return false;
    }
    if (program % kSpCsObjStartAlign != 0 || at(program, uint64_t(instrlen) * 128) == nullptr) {
        fault(text("the program at SP_CS_OBJ_START %#llx, %llu units: outside every buffer or not 32-byte aligned", program, instrlen));
        return false;
    }
    if (!cs_.programLoaded || cs_.programFrom != program) {
        fault("CP_EXEC_CS without the program preloaded from where SP_CS_OBJ_START points");
        return false;
    }
    if (cs_.programUnits > instrlen) {
        fault(text("a preload of %llu units for a program of %llu", cs_.programUnits, instrlen));
        return false;
    }
    uint32_t constlen = fieldOf(hlsq, A6XX_HLSQ_CS_CNTL_CONSTLEN__MASK, A6XX_HLSQ_CS_CNTL_CONSTLEN__SHIFT) << A6XX_HLSQ_CS_CNTL_CONSTLEN__SCALE;
    for (const ConstLoad& c : cs_.consts) {
        if (c.dstOffVec4 + c.units > constlen) {
            fault(text("constants at vec4 %llu, %llu of them: past the program's constlen of %llu", c.dstOffVec4, c.units, constlen));
            return false;
        }
    }
    uint32_t ctrl0 = reg(REG_A6XX_SP_CS_CTRL_REG0);
    uint32_t spThreads = fieldOf(ctrl0, A6XX_SP_CS_CTRL_REG0_THREADSIZE__MASK, A6XX_SP_CS_CTRL_REG0_THREADSIZE__SHIFT);
    uint32_t hlsqThreads = fieldOf(reg(REG_A6XX_HLSQ_CS_CNTL_1), A6XX_HLSQ_CS_CNTL_1_THREADSIZE__MASK, A6XX_HLSQ_CS_CNTL_1_THREADSIZE__SHIFT);
    if (spThreads != hlsqThreads) {
        fault("SP_CS_CTRL_REG0 and HLSQ_CS_CNTL_1 disagree about the thread size");
        return false;
    }
    if (reg(REG_A6XX_HLSQ_CS_CNTL_0) != reg(REG_A6XX_SP_CS_CNTL_0) || reg(REG_A6XX_HLSQ_CS_CNTL_1) != reg(REG_A6XX_SP_CS_CNTL_1)) {
        fault("the HLSQ's and the SP's copies of the id registers (CS_CNTL_0, CS_CNTL_1) differ");
        return false;
    }
    if (reg(REG_A6XX_SP_CS_CTRL_REG1) != reg(REG_A6XX_HLSQ_CS_CTRL_REG1)) {
        fault("SP_CS_CTRL_REG1 and HLSQ_CS_CTRL_REG1 differ about shared memory and the constant split");
        return false;
    }
    uint32_t nd0 = reg(REG_A6XX_HLSQ_CS_NDRANGE_0);
    uint32_t dim = fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM__MASK, A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM__SHIFT);
    uint32_t local[3] = {fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__MASK, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__SHIFT) + 1,
                         fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__MASK, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__SHIFT) + 1,
                         fieldOf(nd0, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__MASK, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__SHIFT) + 1};
    if (dim == 0) {
        fault("HLSQ_CS_NDRANGE_0 with a kernel dimension of 0");
        return false;
    }
    if (uint64_t(local[0]) * local[1] * local[2] > 1024) {
        fault(text("a threadgroup of %llu x %llu x %llu threads: more than the 1024 the GPU runs", local[0], local[1], local[2]));
        return false;
    }
    const uint32_t* groups = p + 1;
    uint32_t global[3] = {reg(REG_A6XX_HLSQ_CS_NDRANGE_1), reg(REG_A6XX_HLSQ_CS_NDRANGE_3), reg(REG_A6XX_HLSQ_CS_NDRANGE_5)};
    for (int i = 0; i < 3; i++) {
        if (groups[i] == 0) {
            fault("CP_EXEC_CS with 0 work groups along an axis");
            return false;
        }
        if (global[i] != local[i] * groups[i]) {
            fault(text("HLSQ_CS_NDRANGE global size %llu is not the local size %llu times the %llu groups CP_EXEC_CS asks for", global[i],
                       local[i], groups[i]));
            return false;
        }
    }
    uint32_t nibo = fieldOf(config, A6XX_SP_CS_CONFIG_NIBO__MASK, A6XX_SP_CS_CONFIG_NIBO__SHIFT);
    if (nibo != reg(REG_A6XX_SP_CS_IBO_COUNT)) {
        fault(text("SP_CS_CONFIG names %llu IBOs and SP_CS_IBO_COUNT %llu", nibo, reg(REG_A6XX_SP_CS_IBO_COUNT)));
        return false;
    }
    if (nibo != 0) {
        uint64_t table = reg64(REG_A6XX_SP_CS_IBO);
        if (!cs_.ibosLoaded || cs_.ibosFrom != table || cs_.iboUnits != nibo) {
            fault("the IBO descriptors were not loaded from where SP_CS_IBO points, or not as many as SP_CS_IBO_COUNT says");
            return false;
        }
        const uint8_t* bytes = at(table, uint64_t(nibo) * 64);
        if (table % kSpCsTexConstAlign != 0 || bytes == nullptr) {
            fault(text("the IBO descriptors at %#llx: outside every buffer or not 64-byte aligned", table));
            return false;
        }
        for (uint32_t i = 0; i < nibo; i++) {
            uint32_t d[kTexConstDwords];
            memcpy(d, bytes + 64 * i, sizeof d);
            if (fieldOf(d[2], A6XX_TEX_CONST_2_TYPE__MASK, A6XX_TEX_CONST_2_TYPE__SHIFT) != A6XX_TEX_BUFFER) {
                fault(text("IBO %llu is not a buffer descriptor", i));
                return false;
            }
            uint64_t elements = fieldOf(d[1], A6XX_TEX_CONST_1_WIDTH__MASK, A6XX_TEX_CONST_1_WIDTH__SHIFT) |
                                uint64_t(fieldOf(d[1], A6XX_TEX_CONST_1_HEIGHT__MASK, A6XX_TEX_CONST_1_HEIGHT__SHIFT)) << 15;
            uint64_t base = (d[4] & A6XX_TEX_CONST_4_BASE_LO__MASK) |
                            uint64_t(fieldOf(d[5], A6XX_TEX_CONST_5_BASE_HI__MASK, A6XX_TEX_CONST_5_BASE_HI__SHIFT)) << 32;
            uint64_t start = 4ull * fieldOf(d[2], A6XX_TEX_CONST_2_STARTOFFSETTEXELS__MASK, A6XX_TEX_CONST_2_STARTOFFSETTEXELS__SHIFT);
            if (elements == 0 || at(base + start, 4 * elements) == nullptr) {
                fault(text("IBO %llu: %llu elements at %#llx: outside every buffer", i, elements, base + start));
                return false;
            }
        }
    }
    stats_.dispatches++;
    Dispatch& d = stats_.lastDispatch;
    for (int i = 0; i < 3; i++) {
        d.groups[i] = groups[i];
        d.local[i] = local[i];
    }
    d.program = program;
    d.instrlen = instrlen;
    d.constlen = constlen;
    d.ibos = nibo;
    d.consts = cs_.consts;
    cs_ = CsState{};   // the next dispatch loads its own
    return true;
}

// the packets from ADDRESS on, WORDS of them; false on a fault (the submission stops there)
bool FakeGpu::execute(uint64_t address, uint32_t words, int depth) {
    using namespace a6xx;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(at(address, 4ull * words));
    if (w == nullptr) {
        fault("the command stream is not inside a buffer");
        return false;
    }
    for (uint32_t i = 0; i < words;) {
        pm4::Header h = readHeader(w[i]);
        if (h.kind == pm4::Header::kBad) {
            fault(text("word %llu (0x%08llx): not a packet, or its parity is wrong", i, w[i]));
            return false;
        }
        if (h.count > words - i - 1) {
            fault("a packet runs past the end of the stream");
            return false;
        }
        const uint32_t* p = w + i + 1;
        stats_.packets++;
        if (h.kind == pm4::Header::kRegisters) {
            for (uint32_t j = 0; j < h.count; j++) {
                regs_[h.reg + j] = p[j];   // kept as written; a dispatch is checked against them
            }
        } else {
            switch (h.opcode) {
            case pm4::kNop:
            case pm4::kWaitForIdle:
            case pm4::kWaitMemWrites:
            case pm4::kWaitForMe:
                break;
            case pm4::kMemWrite: {
                if (h.count < 2) {
                    fault("CP_MEM_WRITE without its address");
                    return false;
                }
                uint64_t to = p[0] | uint64_t(p[1]) << 32;
                uint8_t* d = at(to, 4ull * (h.count - 2));
                if (d == nullptr || to % 4 != 0) {
                    fault("CP_MEM_WRITE outside every buffer");
                    return false;
                }
                memcpy(d, p + 2, 4ull * (h.count - 2));
                break;
            }
            case pm4::kMemcpy: {
                if (h.count != 5) {
                    fault("CP_MEMCPY of other than 5 words");
                    return false;
                }
                uint64_t n = p[0], from = p[1] | uint64_t(p[2]) << 32, to = p[3] | uint64_t(p[4]) << 32;
                // The phone's GPU took 125 000 words in one packet (4.2 ms) and hung on 4 Mi (27 October); where between
                // the two it stops taking them is not measured. The driver cuts at 64 Ki, and the fake holds it to that.
                if (n > 64 * 1024) {
                    fault("CP_MEMCPY of more words in one packet than the driver's own limit of 64 Ki");
                    return false;
                }
                uint8_t* s = at(from, 4 * n);
                uint8_t* d = at(to, 4 * n);
                if (s == nullptr || d == nullptr || from % 4 != 0 || to % 4 != 0) {
                    fault("CP_MEMCPY outside every buffer");
                    return false;
                }
                memmove(d, s, 4 * n);
                stats_.wordsCopied += n;
                break;
            }
            case pm4::kIndirectBuffer: {
                if (h.count != 3 || depth >= 2) {
                    fault(depth >= 2 ? "indirect buffers nested deeper than the GPU's two levels" : "CP_INDIRECT_BUFFER of other than 3 words");
                    return false;
                }
                if (!execute(p[0] | uint64_t(p[1]) << 32, p[2], depth + 1)) {
                    return false;
                }
                break;
            }
            // --- the compute dispatch's packets ---
            case CP_LOAD_STATE6_FRAG:
                if (!loadState(p, h.count)) {
                    return false;
                }
                break;
            case CP_SET_MARKER:
                if (h.count != 1) {
                    fault("CP_SET_MARKER of other than 1 word");
                    return false;
                }
                break;
            case CP_EVENT_WRITE: {
                // 1 word: the event; 4: and the address and value written when it completes (at once here: the model
                // is in order, so what a later wait reads is what an earlier event wrote)
                if (h.count != 1 && h.count != 4) {
                    fault(text("CP_EVENT_WRITE of %llu words, not 1 or 4", h.count));
                    return false;
                }
                if (h.count == 4) {
                    uint64_t to = p[1] | uint64_t(p[2]) << 32;
                    uint8_t* d = at(to, 4);
                    if (d == nullptr || to % 4 != 0) {
                        fault(text("CP_EVENT_WRITE's timestamp at %#llx: outside every buffer", to));
                        return false;
                    }
                    memcpy(d, p + 3, 4);
                }
                break;
            }
            case CP_WAIT_REG_MEM: {
                if (h.count != 6) {
                    fault(text("CP_WAIT_REG_MEM of %llu words, not 6", h.count));
                    return false;
                }
                if (fieldOf(p[0], CP_WAIT_REG_MEM_0_POLL__MASK, CP_WAIT_REG_MEM_0_POLL__SHIFT) != POLL_MEMORY) {
                    fault("CP_WAIT_REG_MEM on a register: not what this GPU model runs");
                    return false;
                }
                uint64_t a = p[1] | uint64_t(p[2]) << 32;
                const uint8_t* m = at(a, 4);
                if (m == nullptr || a % 4 != 0) {
                    fault(text("CP_WAIT_REG_MEM on %#llx: outside every buffer", a));
                    return false;
                }
                uint32_t value;
                memcpy(&value, m, 4);
                value &= p[4];
                uint32_t ref = p[3];
                bool holds = false;
                switch (fieldOf(p[0], CP_WAIT_REG_MEM_0_FUNCTION__MASK, CP_WAIT_REG_MEM_0_FUNCTION__SHIFT)) {
                case WRITE_ALWAYS: holds = true; break;
                case WRITE_LT: holds = value < ref; break;
                case WRITE_LE: holds = value <= ref; break;
                case WRITE_EQ: holds = value == ref; break;
                case WRITE_NE: holds = value != ref; break;
                case WRITE_GE: holds = value >= ref; break;
                case WRITE_GT: holds = value > ref; break;
                default: break;
                }
                if (!holds) {
                    // nothing after this packet can change the word, so the real command processor would spin forever
                    fault(text("CP_WAIT_REG_MEM would wait forever: the word at %#llx is %#llx, the reference %#llx", a, value, ref));
                    return false;
                }
                break;
            }
            case CP_WAIT_MEM_GTE: {
                if (h.count != 4) {
                    fault(text("CP_WAIT_MEM_GTE of %llu words, not 4", h.count));
                    return false;
                }
                uint64_t a = p[1] | uint64_t(p[2]) << 32;
                const uint8_t* m = at(a, 4);
                if (m == nullptr || a % 4 != 0) {
                    fault(text("CP_WAIT_MEM_GTE on %#llx: outside every buffer", a));
                    return false;
                }
                uint32_t value;
                memcpy(&value, m, 4);
                if (int32_t(value - p[3]) < 0) {
                    fault(text("CP_WAIT_MEM_GTE would wait forever: the word at %#llx is %#llx, below %#llx", a, value, p[3]));
                    return false;
                }
                break;
            }
            case CP_EXEC_CS:
                if (!execCs(p, h.count)) {
                    return false;
                }
                break;
            default: {
                fault(text("opcode 0x%02llx is not one this GPU model runs", h.opcode));
                return false;
            }
            }
        }
        i += 1 + h.count;
    }
    return true;
}

// A submission the GPU never finishes, as the kernel treats it. With recovery, the GPU is reset and everything it
// had been given is given up as done: the fence word is set to the last submission's number (hakios_gpu_recover),
// so every wait ends, the hung submission's own with the timeout it was found by, and FAULTS says it happened.
// Without recovery nothing is waited for again in this run.
void FakeGpu::hung() {
    jobs_.clear();
    if (recovers_) {
        stats_.recoveries++;
        completed_ = submitted_;
    } else {
        wedged_ = true;
    }
}

void FakeGpu::run(const Job& job) {
    stats_.submits++;
    cs_ = CsState{};   // (a submission starts with nothing loaded: the hardware keeps no state across them that this models)
    if (execute(job.address, job.words, 0)) {
        completed_ = job.fence;
        return;
    }
    hungFences_.insert(job.fence);
    hung();
}

void FakeGpu::worker() {
    std::unique_lock<std::mutex> g(lock_);
    for (;;) {
        changed_.wait(g, [this] { return quit_ || (!paused_ && !jobs_.empty()); });
        if (quit_ && jobs_.empty()) {
            return;
        }
        if (paused_) {
            continue;
        }
        Job job = jobs_.front();
        jobs_.pop_front();
        if (delayUs_ != 0) {
            g.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(delayUs_));
            g.lock();
        }
        run(job);
        changed_.notify_all();
    }
}

int FakeGpu::call(uint32_t request, void* data, size_t size) {
    std::unique_lock<std::mutex> g(lock_);
    switch (request) {
    case kGetParam: {
        if (size != sizeof(MsmParam)) return EINVAL;
        auto* p = static_cast<MsmParam*>(data);
        // the kernel's table (bsd/dev/hakios.adreno.c, hakios_gpu_parameter), number for number
        switch (p->param) {
        case kParamGpuId: p->value = 0; return 0;                   // "none, the chip ID says it"
        case kParamGmemSize: p->value = kGmemBytes; return 0;
        case kParamChipId: p->value = kChipId | kSpeedBin << 32; return 0;
        case kParamMaxFreq: p->value = kMaxFrequencyHz; return 0;
        case kParamTimestamp: p->value = uint64_t(nowNs()); return 0;
        case kParamGmemBase: p->value = 0; return 0;
        case kParamPriorities: p->value = 1; return 0;
        case kParamPpPgtable: p->value = 0; return 0;
        case kParamFaults: p->value = stats_.recoveries; return 0;
        case kParamSuspends: p->value = 0; return 0;
        case kParamHighestBankBit: p->value = 14; return 0;
        case kParamRaytracing: p->value = 0; return 0;
        case kParamUbwcSwizzle: p->value = 6; return 0;
        case kParamMacrotileMode: p->value = 1; return 0;
        default: return EINVAL;                                      // (kIOReturnBadArgument there: VA_START, VA_SIZE among them)
        }
    }
    case kGemNew: {
        if (size != sizeof(MsmGemNew)) return EINVAL;
        auto* n = static_cast<MsmGemNew*>(data);
        if (n->size == 0 || n->size > (256ull << 20)) return EINVAL;
        Buffer b{nextIova_, std::vector<uint8_t>(n->size)};
        nextIova_ += (n->size + 0xffff) & ~0xffffull;
        n->handle = nextHandle_++;
        buffers_.emplace(n->handle, std::move(b));
        return 0;
    }
    case kGemInfo: {
        if (size != sizeof(MsmGemInfo)) return EINVAL;
        auto* i = static_cast<MsmGemInfo*>(data);
        auto b = buffers_.find(i->handle);
        if (b == buffers_.end()) return ENOENT;
        if (i->info == kInfoGetIova) { i->value = b->second.iova; return 0; }
        if (i->info == kInfoGetOffset) { i->value = 0; return 0; }
        return EINVAL;
    }
    case kGemClose: {
        if (size != sizeof(DrmGemClose)) return EINVAL;
        return buffers_.erase(static_cast<DrmGemClose*>(data)->handle) ? 0 : ENOENT;
    }
    case kSubmitQueueNew: {
        if (size != sizeof(MsmSubmitQueue)) return EINVAL;
        auto* q = static_cast<MsmSubmitQueue*>(data);
        q->id = nextQueue_++;
        queues_.insert(q->id);
        return 0;
    }
    case kSubmitQueueClose:
        return 0;
    case kGemSubmit: {
        if (size != sizeof(MsmGemSubmit)) return EINVAL;
        auto* s = static_cast<MsmGemSubmit*>(data);
        if (s->nr_cmds != 1 || s->nr_bos == 0 || (s->queueid != 0 && queues_.count(s->queueid) == 0)) return EINVAL;
        auto* bos = reinterpret_cast<const MsmSubmitBo*>(uintptr_t(s->bos));
        auto* cmd = reinterpret_cast<const MsmSubmitCmd*>(uintptr_t(s->cmds));
        if (cmd->submit_idx >= s->nr_bos || cmd->type != kSubmitCmdBuf || cmd->size % 4 != 0) return EINVAL;
        for (uint32_t i = 0; i < s->nr_bos; i++) {
            if (buffers_.count(bos[i].handle) == 0) return ENOENT;   // a buffer the GPU would not know: refused
        }
        Buffer& c = buffers_.at(bos[cmd->submit_idx].handle);
        if (uint64_t(cmd->submit_offset) + cmd->size > c.bytes.size()) return EINVAL;
        Job job{++submitted_, c.iova + cmd->submit_offset, cmd->size / 4};
        s->fence = job.fence;
        // (a wedged GPU still takes submissions, as the kernel's ring does until it is full: they are never run)
        if (wedged_) {
            return 0;
        }
        if (async_) {
            jobs_.push_back(job);
            changed_.notify_all();
        } else {
            run(job);
        }
        return 0;
    }
    case kWaitFence: {
        if (size != sizeof(MsmWaitFence)) return EINVAL;
        auto* w = static_cast<MsmWaitFence*>(data);
        if (w->queueid != 0 && queues_.count(w->queueid) == 0) return EINVAL;
        if (w->fence > submitted_) {
            // The kernel does not know this fence was never handed out: it waits the caller's whole patience for a word
            // that never comes, and that is a hang like any other. Here it is one at once.
            fault("a fence that was never handed out was waited for");
            hung();
            return ETIMEDOUT;
        }
        if (wedged_ || hungFences_.count(w->fence) != 0) {
            // (the kernel takes the caller's whole patience to say so; the fake knows already)
            return ETIMEDOUT;
        }
        int64_t deadline = w->timeout.seconds * 1000000000 + w->timeout.nanoseconds;
        while (completed_ < w->fence) {
            int64_t left = deadline - nowNs();
            if (left <= 0) return ETIMEDOUT;
            changed_.wait_for(g, std::chrono::nanoseconds(left));
            if (wedged_ || hungFences_.count(w->fence) != 0) return ETIMEDOUT;
        }
        return 0;
    }
    case kGemCpuPrep:
    case kGemCpuFini:
        return 0;
    default:
        return ENOSYS;
    }
}

}  // namespace hakios::metal
