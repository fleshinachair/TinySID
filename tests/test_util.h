#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_fails;

#define SECTION(name) printf("[%s]\n", name)

#define CHECK(cond) do { \
        g_checks++; \
        if (!(cond)) { g_fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ(a, b) do { \
        long long va_ = (long long)(a), vb_ = (long long)(b); \
        g_checks++; \
        if (va_ != vb_) { \
            g_fails++; \
            printf("  FAIL %s:%d: %s == %s (got %lld, want %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        } \
    } while (0)

#define CHECK_NEAR(a, b, tol) do { \
        double va_ = (double)(a), vb_ = (double)(b); \
        g_checks++; \
        if (!(fabs(va_ - vb_) <= (tol))) { \
            g_fails++; \
            printf("  FAIL %s:%d: %s ~ %s (got %g, want %g +-%g)\n", __FILE__, __LINE__, #a, #b, va_, vb_, (double)(tol)); \
        } \
    } while (0)

#define CHECK_STR(buf, want) do { \
        g_checks++; \
        if (strcmp((buf), (want)) != 0) { \
            g_fails++; \
            printf("  FAIL %s:%d: %s is \"%s\", want \"%s\"\n", __FILE__, __LINE__, #buf, (buf), (want)); \
        } \
    } while (0)

static int test_finish(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif
