// Host unit test for money_format_whole — the Enterprise Spending card's amount
// formatting. No Arduino/LVGL deps:
//
//   g++ -std=c++17 -I ../../src test_main.cpp -o t && ./t

#include "money_format.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK_WITH(fn, minor, exp, cur, expected)                              \
    do {                                                                       \
        char out[32];                                                          \
        fn(minor, exp, cur, out, sizeof(out));                                 \
        if (strcmp(out, expected) != 0) {                                      \
            printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__,  \
                   out, expected);                                             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)
#define CHECK_FMT(minor, exp, cur, expected)   CHECK_WITH(money_format_whole, minor, exp, cur, expected)
#define CHECK_SHORT(minor, exp, cur, expected) CHECK_WITH(money_format_short, minor, exp, cur, expected)

int main() {
    CHECK_FMT(1250, 2, "USD", "$12");            // $12.50 floors to $12
    CHECK_FMT(150000, 2, "USD", "$1,500");       // thousands separator
    CHECK_FMT(99, 2, "USD", "$0");               // under a dollar floors to $0
    CHECK_FMT(0, 2, "USD", "$0");
    CHECK_FMT(-5, 2, "USD", "$0");               // never render a negative spend
    CHECK_FMT(123456789, 2, "USD", "$1,234,567");
    CHECK_FMT(100000, 2, "USD", "$1,000");       // exact group boundary
    CHECK_FMT(12500, 2, "EUR", "EUR 125");       // non-USD keeps its ISO code
    CHECK_FMT(5000, 0, "JPY", "JPY 5,000");      // zero-exponent currency
    CHECK_FMT(1250, 2, "", "$12");               // missing currency defaults to $
    CHECK_FMT(1250, 2, nullptr, "$12");
    CHECK_SHORT(1743912, 2, "USD", "$17.4k");    // floors, never rounds up past the real spend
    CHECK_SHORT(123456789, 2, "EUR", "EUR 1.2M");
    CHECK_SHORT(99999900, 2, "USD", "$999.9k");
    CHECK_SHORT(100000000, 2, "USD", "$1.0M");
    CHECK_SHORT(1250, 2, "USD", "$12");          // small amounts stay whole
    CHECK_SHORT(-1, 2, "USD", "$0");
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all money format checks passed\n");
    return 0;
}
