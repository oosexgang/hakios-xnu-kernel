#include <cstring>

#include "golden.hpp"
#include "test.hpp"

// tests [--golden] [NAME]: every test whose name holds NAME. With --golden, instead of the verdicts, what each test
// submitted to the fake GPU, summed (golden.hpp): a line a test, compared with tests/golden/tests-<chip>.txt by
// `make -f tools/dev.mk check`.
int main(int argc, char** argv) {
    int ran = 0;
    bool sums = false;
    const char* only = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--golden") == 0) sums = true;
        else only = argv[i];
    }
    golden::Sums s;
    if (sums) golden::sumInto(&s);
    // tests whose submissions depend on which thread got there first (a command buffer let go while the GPU has it:
    // whether its memory is back in the queue's pools when the next one asks is the race the test is about), so that
    // the addresses in their streams differ from run to run: counted, not summed
    static const char* const kTimed[] = {"a_command_buffer_released_in_flight_gives_its_buffers_back"};
    for (const test::Case& c : test::cases()) {
        if (only != nullptr && strstr(c.name, only) == nullptr) {
            continue;
        }
        int before = test::failures();
        s.reset();
        c.run();
        if (sums) {
            bool timed = false;
            for (const char* t : kTimed) timed |= strcmp(t, c.name) == 0;
            // (a test that submits nothing says so with zeros: its line still tells a test added or taken away)
            std::printf("%s submits %u words %llu draws %u loads %u stream %016llx state %016llx names %u %016llx%s%s\n", c.name, s.submits,
                        (unsigned long long)s.words, s.draws, s.loads, (unsigned long long)(s.submits && !timed ? s.stream : 0),
                        (unsigned long long)(s.submits && !timed ? s.state : 0), timed ? 0 : s.named, (unsigned long long)(timed ? 0 : s.names),
                        s.bad ? " (unread)" : "", timed ? " (by timing: not summed)" : "");
        } else {
            std::printf("%s %s\n", test::failures() == before ? "ok  " : "FAIL", c.name);
        }
        ran++;
    }
    if (!sums) std::printf("%d tests, %d failed checks\n", ran, test::failures());
    return test::failures() == 0 ? 0 : 1;
}
