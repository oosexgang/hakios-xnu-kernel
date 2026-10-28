// A GPU for the tests: the kernel driver's requests answered in this process, and the command streams run by an
// interpreter of the packets this driver writes. It is strict where the command processor is: a header whose parity
// is wrong, a count that runs past the end, an opcode it does not know, or an address outside every buffer stops the
// submission there and is counted as a fault, the way the real GPU would hang on it. A test that passes here has
// written only packets the real one reads the same way.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hakios-metal/gpu.hpp"

namespace hakios::metal {

class FakeGpu : public Transport {
public:
    struct Stats {
        uint32_t submits = 0;
        uint32_t packets = 0;
        uint32_t faults = 0;
        std::string lastFault;
        uint64_t wordsCopied = 0;
    };

    // ASYNC: submissions run on a thread of their own, after DELAY_US each, as the real GPU runs them after the call
    // has returned; otherwise at once, inside the submit call
    explicit FakeGpu(bool async = false, uint32_t delayUs = 0);
    ~FakeGpu() override;

    int call(uint32_t request, void* data, size_t size) override;
    void* map(uint32_t handle, uint64_t size) override;
    void unmap(uint32_t handle, void* address) override;
    int64_t nowNs() override;

    Stats stats();
    uint32_t liveBuffers();
    // stop running submissions until resume(): to see what a program does while the GPU is behind
    void pause();
    void resume();

private:
    struct Buffer {
        uint64_t iova;
        std::vector<uint8_t> bytes;
    };
    struct Job {
        uint32_t queue, fence;
        uint64_t address;
        uint32_t words;
    };

    uint8_t* at(uint64_t iova, uint64_t length);
    void run(const Job& job);
    bool execute(uint64_t address, uint32_t words, int depth);
    void fault(const std::string& why);
    void worker();

    std::mutex lock_;
    std::condition_variable changed_;
    std::map<uint32_t, Buffer> buffers_;
    std::map<uint32_t, uint32_t> submitted_, completed_;  // per queue: the last fence handed out, the last done
    std::deque<Job> jobs_;
    uint32_t nextHandle_ = 1, nextQueue_ = 1;
    uint64_t nextIova_ = 0x100000000ull;
    Stats stats_;
    bool async_, paused_ = false, quit_ = false;
    uint32_t delayUs_;
    std::thread thread_;
};

}  // namespace hakios::metal
