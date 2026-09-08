#include "test.h"
#include <cstring>

namespace xfade_test {
std::vector<Case>& cases() { static std::vector<Case> c; return c; }
int g_failures = 0;
bool g_real = false;

int runAll(int argc, char** argv) {
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--real") == 0) g_real = true;
        else filter = argv[i];
    }
    int ran = 0;
    for (const Case& c : cases()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = g_failures;
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", c.name);
        ++ran;
    }
    std::printf("%d tests, %d failures\n", ran, g_failures);
    return g_failures ? 1 : 0;
}
}  // namespace xfade_test

int main(int argc, char** argv) { return xfade_test::runAll(argc, argv); }
