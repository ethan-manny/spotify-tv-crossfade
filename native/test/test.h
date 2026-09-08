#pragma once
#include <cstdio>
#include <vector>

namespace xfade_test {

struct Case { const char* name; void (*fn)(); };
std::vector<Case>& cases();
struct Registrar { Registrar(const char* name, void (*fn)()) { cases().push_back({name, fn}); } };
extern int g_failures;
extern bool g_real;  // true when run with --real (tests that touch the speaker)
int runAll(int argc, char** argv);

}  // namespace xfade_test

#define TEST(name) \
    static void name(); \
    static xfade_test::Registrar registrar_##name(#name, name); \
    static void name()

#define EXPECT_TRUE(cond) \
    do { if (!(cond)) { std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++xfade_test::g_failures; } } while (0)

#define EXPECT_EQ(a, b) \
    do { auto va_ = (a); auto vb_ = (b); \
         if (!(va_ == vb_)) { std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                                          (long long)va_, (long long)vb_); ++xfade_test::g_failures; } } while (0)
