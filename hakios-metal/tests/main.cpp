#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
    int ran = 0;
    for (const test::Case& c : test::cases()) {
        if (argc > 1 && strstr(c.name, argv[1]) == nullptr) {
            continue;
        }
        int before = test::failures();
        c.run();
        std::printf("%s %s\n", test::failures() == before ? "ok  " : "FAIL", c.name);
        ran++;
    }
    std::printf("%d tests, %d failed checks\n", ran, test::failures());
    return test::failures() == 0 ? 0 : 1;
}
