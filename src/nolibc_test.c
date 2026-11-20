/* The libc-free number host, checked against the libc one on a machine that
   has both. The promise it has to keep is stated in filo_nolibc.h: the same
   text and the same values as a libc host — as Go — for every double, both
   ways. Everyday values go through a one-step path and the rest through big
   integers, so both are checked: values in the one-step range, arbitrary bit
   patterns, long digit strings, and %f at any precision. Any difference is a
   failure. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filo_libc.h"
#include "filo_nolibc.h"

static int failures = 0;
static int shown = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ULL;

static uint64_t next_rand(void) {
    rng += 0x9E3779B97F4A7C15ULL;
    uint64_t z = rng;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

static size_t fmt(double x, char *dst, size_t cap) {
    size_t n = filo_nolibc_num_to_str(NULL, x, dst, cap);
    dst[n] = '\0';
    return n;
}

static bool same_as_libc(double x) {
    char a[64];
    char b[64];
    size_t na = filo_libc_num_to_str(NULL, x, a, sizeof(a));
    a[na] = '\0';
    size_t nb = fmt(x, b, sizeof(b));
    if (na == nb && memcmp(a, b, na) == 0) {
        return true;
    }
    if (shown < 10) {
        printf("     libc=%-26s nolibc=%-26s (%a)\n", a, b, x);
        shown++;
    }
    return false;
}

static bool round_trips(double x) {
    char b[64];
    size_t nb = fmt(x, b, sizeof(b));
    double back = 0;
    if (!filo_nolibc_str_to_num(NULL, (const uint8_t *)b, nb, &back)) {
        return false;
    }
    if (back != x) {
        if (shown < 10) {
            printf("     %s did not read back (%a)\n", b, x);
            shown++;
        }
        return false;
    }
    return true;
}

static void test_everyday_values_match_libc(void) {
    const double v[] = {
        0,
        1,
        2,
        10,
        100,
        999999,
        1000000,
        1e6,
        1e-4,
        1e-5,
        0.1,
        0.2,
        0.3,
        0.5,
        1.5,
        3.14159,
        42,
        42.5,
        1e15,
        1e21,
        1e22,
        1e-22,
        123,
        0.001,
        0.0001,
        0.00001,
        1234.5,
        99.99,
        2.5,
        0.125,
        1e9,
        65536,
        9007199254740992.0,
        0.3333333333333333,
        1e-8,
        1.0 / 3,
        2.0 / 3,
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        CHECK(same_as_libc(v[i]));
        CHECK(same_as_libc(-v[i]));
        CHECK(round_trips(v[i]));
    }
    char b[64];
    fmt(0.0, b, sizeof(b));
    CHECK(strcmp(b, "0") == 0);
    fmt(-0.0, b, sizeof(b));
    CHECK(strcmp(b, "-0") == 0);
    fmt(1e308 * 10, b, sizeof(b));
    CHECK(strcmp(b, "+Inf") == 0);
    fmt(-1e308 * 10, b, sizeof(b));
    CHECK(strcmp(b, "-Inf") == 0);
    fmt((1e308 * 10) - (1e308 * 10), b, sizeof(b));
    CHECK(strcmp(b, "NaN") == 0);
    /* Go's placement rule: plain up to a million, scientific from it */
    fmt(999999, b, sizeof(b));
    CHECK(strcmp(b, "999999") == 0);
    fmt(1000000, b, sizeof(b));
    CHECK(strcmp(b, "1e+06") == 0);
    fmt(0.0001, b, sizeof(b));
    CHECK(strcmp(b, "0.0001") == 0);
    fmt(0.00001, b, sizeof(b));
    CHECK(strcmp(b, "1e-05") == 0);
}

static void test_parse_matches_libc(void) {
    const char *s[] = {
        "0",     "1",   "-1",   "1.5",  "0.1",    "1e10",  "1E10",  "1e-10", "-2.5e3",
        "  7  ", "+5",  ".5",   "5.",   "1_000",  "1_0.5", "0x10",  "inf",   "-inf",
        "Inf",   "nan", "NaN",  "",     "abc",    "1e",    "1.2.3", "1e999", "-1e999",
        "12abc", "1 2", "1e+3", "1e-3", "000123", "0.0",   "-0",    "1__0",  "_1",
    };
    for (size_t i = 0; i < sizeof(s) / sizeof(s[0]); i++) {
        double a = 0;
        double b = 0;
        bool oka = filo_libc_str_to_num(NULL, (const uint8_t *)s[i], strlen(s[i]), &a);
        bool okb = filo_nolibc_str_to_num(NULL, (const uint8_t *)s[i], strlen(s[i]), &b);
        if (oka != okb) {
            printf("FAIL parse %-10s libc=%d nolibc=%d\n", s[i], oka ? 1 : 0, okb ? 1 : 0);
            failures++;
            continue;
        }
        if (oka && a != b && !(a != a && b != b)) {
            printf("FAIL parse %-10s libc=%.17g nolibc=%.17g\n", s[i], a, b);
            failures++;
        }
    }
}

/* At most 15 significant digits, magnitude from 1e-8 up to 1e22: the range
   the header promises, where the scaling the formatter needs is a single
   exact step. */
static double in_range_value(void) {
    int k = 1 + (int)(next_rand() % 15);
    char txt[48];
    int p = 0;
    txt[p] = (char)('1' + (next_rand() % 9));
    p++;
    for (int d = 1; d < k; d++) {
        txt[p] = (char)('0' + (next_rand() % 10));
        p++;
    }
    int e10 = (int)(next_rand() % 31) - 8; /* magnitude exponent in [-8, 22] */
    p += snprintf(txt + p, sizeof(txt) - (size_t)p, "e%d", e10 - (k - 1));
    txt[p] = '\0';
    double x = strtod(txt, NULL);
    if (next_rand() % 2 == 0) {
        x = -x;
    }
    return x;
}

static void test_in_range_is_exact(int rounds) {
    int checked = 0;
    int trip = 0;
    int diff = 0;
    for (int i = 0; i < rounds; i++) {
        double x = in_range_value();
        if (x == 0 || x != x) {
            continue;
        }
        checked++;
        if (!round_trips(x)) {
            trip++;
        }
        if (!same_as_libc(x)) {
            diff++;
        }
    }
    printf("in range: %d values, %d round-trip failures, %d differ from libc\n", checked, trip,
           diff);
    CHECK(trip == 0);
    CHECK(diff == 0);
}

/* Every double, not only the everyday ones: arbitrary bit patterns cover
   subnormals, the largest values and 17-digit mantissas. */
static void test_any_double_is_exact(int rounds) {
    int checked = 0;
    int trip = 0;
    int diff = 0;
    for (int i = 0; i < rounds; i++) {
        uint64_t bits = next_rand();
        double x = 0;
        memcpy(&x, &bits, sizeof(x));
        if (x - x != x - x || x == 0) {
            continue; /* NaN, the infinities and zero have their own checks */
        }
        checked++;
        if (!round_trips(x)) {
            trip++;
        }
        if (!same_as_libc(x)) {
            diff++;
        }
    }
    printf("any double: %d values, %d round-trip failures, %d differ from libc\n", checked, trip,
           diff);
    CHECK(trip == 0);
    CHECK(diff == 0);
}

/* Every power of two, subnormal ones too, and its negative: there the gap
   above is twice the one below, so the shortest digits may lie on the far
   side of x from the nearest ones, which a random double almost never
   finds. The libc host once wrote 2^-1007 with 17 digits instead of 16. */
static void test_powers_of_two_are_exact(void) {
    int checked = 0;
    int trip = 0;
    int diff = 0;
    for (int e = -1074; e <= 1023; e++) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            double x = sign * ldexp(1.0, e);
            checked++;
            if (!round_trips(x)) {
                trip++;
            }
            if (!same_as_libc(x)) {
                diff++;
            }
        }
    }
    printf("powers of two: %d values, %d round-trip failures, %d differ from libc\n", checked, trip,
           diff);
    CHECK(trip == 0);
    CHECK(diff == 0);
}

/* Long digit strings read as strtod reads them: correctly rounded however
   many digits there are and wherever the exponent lands. */
static void test_long_decimals_parse_exactly(int rounds) {
    int diff = 0;
    for (int i = 0; i < rounds; i++) {
        char txt[128];
        int n = 16 + (int)(next_rand() % 60);
        int p = 0;
        txt[p] = (char)('1' + (next_rand() % 9));
        p++;
        for (int d = 1; d < n; d++) {
            txt[p] = (char)('0' + (next_rand() % 10));
            p++;
        }
        p += snprintf(txt + p, sizeof(txt) - (size_t)p, "e%d", (int)(next_rand() % 740) - 370);
        double want = strtod(txt, NULL);
        double got = 0;
        bool ok = filo_nolibc_str_to_num(NULL, (const uint8_t *)txt, (size_t)p, &got);
        uint64_t want_bits = 0;
        uint64_t got_bits = 0;
        memcpy(&want_bits, &want, sizeof(want_bits));
        memcpy(&got_bits, &got, sizeof(got_bits));
        if (!ok || want_bits != got_bits) {
            if (shown < 10) {
                printf("     %s: libc=%.17g nolibc=%.17g\n", txt, want, got);
                shown++;
            }
            diff++;
        }
    }
    printf("long decimals: %d strings, %d read differently from libc\n", rounds, diff);
    CHECK(diff == 0);
}

/* %f is exact for every double and precision: the formatter works on the
   value's exact decimal expansion, so it agrees with printf digit for digit —
   subnormals, 1e308 and ties included. */
static void fixed_same(double x, uint32_t prec) {
    static char want[4096];
    static char got[4096];
    int n = snprintf(want, sizeof(want), "%.*f", (int)prec, x);
    size_t g = filo_nolibc_strings.fmt_fixed(x, prec, got, sizeof(got) - 1);
    got[g] = '\0';
    if (n < 0 || (size_t)n != g || memcmp(want, got, g) != 0) {
        if (shown < 10) {
            printf("FAIL %%.%uf of %.17g: libc=%.60s nolibc=%.60s\n", prec, x, want, got);
            shown++;
        }
        failures++;
    }
}

static void test_fixed_point_matches_libc(int rounds) {
    const double v[] = {
        0,     -0.0,   1,         1.5,    2.5, 3.14159, 2,
        -2.5,  0.125,  1234.5678, 99.995, 1e6, -0.0001, 9007199254740992.0,
        1e300, 5e-324,
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        for (uint32_t prec = 0; prec <= 40; prec++) {
            fixed_same(v[i], prec);
        }
        fixed_same(v[i], 1100);
    }
    int checked = 0;
    for (int i = 0; i < rounds; i++) {
        uint64_t bits = next_rand();
        double x = 0;
        memcpy(&x, &bits, sizeof(x));
        if (x - x != x - x) {
            continue; /* NaN and the infinities: str-fmt spells those itself */
        }
        fixed_same(x, (uint32_t)(next_rand() % 41U));
        checked++;
    }
    printf("%%f: %d arbitrary doubles against printf\n", checked);
}

int main(int argc, char **argv) {
    long asked = argc > 1 ? strtol(argv[1], NULL, 10) : 0;
    int rounds = asked > 0 && asked < 100000000L ? (int)asked : 200000;
    test_everyday_values_match_libc();
    test_parse_matches_libc();
    test_fixed_point_matches_libc(rounds / 4);
    test_in_range_is_exact(rounds);
    test_any_double_is_exact(rounds / 4);
    test_powers_of_two_are_exact();
    test_long_decimals_parse_exactly(rounds / 8);
    if (failures > 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("nolibc tests passed\n");
    return 0;
}
