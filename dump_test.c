/* The disassembler against the runtime it describes. Its reader knows the
   format only from docs/bytecode.md, so every unit the compiler wrote for
   the corpus and the oracle (make device leaves them in build/units) must
   read, list and agree with the loader; and a constant must be written as
   the runtime writes it, for any double.

   usage: dump_test DIR */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fbc_decompile.h"
#include "fbc_dump.h"
#include "filo.h"
#include "filo_fmt.h"
#include "filo_libc.h"
#include "filo_math.h"
#include "filo_strings.h"

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static uint8_t data[1U << 20U];
static uint8_t persistent_mem[1U << 20U];
static uint8_t run_mem[1U << 20U];
static fbc_unit unit;
static filo_ctx ctx;
static size_t lines = 0;

static size_t broken = 0;

/* A listing of a unit the compiler wrote reads every instruction whole.
   It is also kept beside the unit (NNNNN.dump), for the Go engine's
   listing to be held to it (make govm). */
static void count(void *user, const char *line) {
    FILE *keep = user;
    if (keep != NULL) {
        (void)fputs(line, keep);
        (void)fputc('\n', keep);
    }
    lines++;
    if (strstr(line, "(an instruction cut short)") != NULL || strstr(line, "(unknown") != NULL ||
        strstr(line, "runs past the function") != NULL) {
        broken++;
    }
}

static uint64_t rng = 0x9E3779B97F4A7C15ULL;

static uint64_t next_rand(void) {
    rng += 0x9E3779B97F4A7C15ULL;
    uint64_t z = rng;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

static void test_numbers_read_as_the_runtime_writes_them(void) {
    int diff = 0;
    for (int i = 0; i < 200000; i++) {
        double x = 0;
        if (i % 2 == 0) {
            uint64_t bits = next_rand();
            memcpy(&x, &bits, sizeof(x));
        } else {
            x = (double)(int64_t)(next_rand() % 2000001U) - 1000000.0;
            x /= (double)(1U << (next_rand() % 12U));
        }
        char want[64];
        char got[64];
        size_t n = filo_libc_num_to_str(NULL, x, want, sizeof(want) - 1);
        want[n] = '\0';
        fbc_number(x, got, sizeof(got));
        if (strcmp(want, got) != 0) {
            if (diff < 5) {
                printf("  %a: runtime %s, listing %s\n", x, want, got);
            }
            diff++;
        }
    }
    /* every power of two too, where the shortest digits may lie on the far
       side of x: a random double almost never is one */
    for (int e = -1074; e <= 1023; e++) {
        double x = ldexp(1.0, e);
        char want[64];
        char got[64];
        size_t n = filo_libc_num_to_str(NULL, x, want, sizeof(want) - 1);
        want[n] = '\0';
        fbc_number(x, got, sizeof(got));
        if (strcmp(want, got) != 0) {
            if (diff < 5) {
                printf("  2^%d: runtime %s, listing %s\n", e, want, got);
            }
            diff++;
        }
    }
    printf("numbers: 200000 doubles and 2098 powers of two, %d written differently\n", diff);
    CHECK(diff == 0);
}

static void test_units_list_and_agree_with_the_loader(const char *dir) {
    unsigned read = 0;
    for (unsigned no = 0;; no++) {
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%05u.fbc", dir, no);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            break;
        }
        size_t len = fread(data, 1, sizeof(data), f);
        fclose(f);
        char why[128];
        bool ok = fbc_read(&unit, data, len, why, sizeof(why));
        CHECK(ok);
        if (!ok) {
            printf("  %s: %s\n", path, why);
            continue;
        }
        CHECK(unit.checksum_ok);
        lines = 0;
        (void)snprintf(path, sizeof(path), "%s/%05u.dump", dir, no);
        FILE *keep = fopen(path, "w");
        fbc_dump(&unit, count, keep);
        if (keep != NULL) {
            (void)fclose(keep);
        }
        CHECK(lines > unit.nfns);
        CHECK(broken == 0);
        filo_init(&ctx, &filo_libc_host, persistent_mem, sizeof(persistent_mem), run_mem,
                  sizeof(run_mem));
        (void)filo_math_register(&ctx, &filo_libc_math);
        (void)filo_strings_register(&ctx, &filo_libc_strings);
        const filo_unit *u = NULL;
        CHECK(filo_bc_load_lazy(&ctx, data, len, &u) ==
              FILO_OK); /* corpus units: as the interpreter */
        for (uint32_t i = 0; i < unit.nexports; i++) {
            char name[256];
            fbc_span s = unit.export_names[i];
            (void)snprintf(name, sizeof(name), "%.*s", (int)s.len, (const char *)data + s.off);
            CHECK(u != NULL && filo_bc_has(u, name));
        }
        read++;
    }
    printf("units: %u read, listed and loaded\n", read);
    CHECK(read > 1000);
}

/* ---- decompiled, and compiled back ---- */

static uint8_t dmem[64U << 20U];
static uint8_t again[1U << 20U];
static uint8_t stripped[2][1U << 20U];
static char texts[64][1U << 16U];
static size_t text_len[64];
static char names[64][256];
static uint32_t nsources;

static void first_line(void *user, const char *line) {
    char *keep = user;
    if (keep[0] == '\0') {
        (void)snprintf(keep, 128, "%s", line);
    }
}

/* The folder's answer, as filo decompile's: the folded call is a literal. */
static bool folds(void *user, const char *call, size_t len) {
    filo_ctx *c = user;
    char root[128] = "";
    if (filo_show(c, (const uint8_t *)call, len, "folded", first_line, root) != FILO_OK) {
        return false;
    }
    const char *what = strchr(root, ' ');
    while (what != NULL && *what == ' ') {
        what++;
    }
    if (what == NULL) {
        return false;
    }
    if (strncmp(what, "number ", 7) == 0 || strncmp(what, "bool ", 5) == 0) {
        return true;
    }
    return strncmp(what, "string ", 7) == 0;
}

static void keep_source(void *user, const char *name, size_t nlen, const char *text, size_t len) {
    (void)user;
    if (nsources < 64 && len < sizeof(texts[0])) {
        (void)snprintf(names[nsources], sizeof(names[0]), "%.*s", (int)nlen, name);
        memcpy(texts[nsources], text, len);
        text_len[nsources] = len;
    }
    nsources++;
}

/* Each unit decompiles (fbc_decompile.c) to Filo that this runtime, with
   the case's packs, compiles back to the same unit but for its debug
   section; the text is kept beside it (NNNNN.dec: "; NAME" and the text,
   an entry after another) for the Go engine's decompiler to be held to it
   (make govm), character for character. */
static void test_units_decompile_and_build_back(const char *dir) {
    unsigned done = 0;
    unsigned differ = 0;
    for (unsigned no = 0;; no++) {
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%05u.fbc", dir, no);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            break;
        }
        size_t len = fread(data, 1, sizeof(data), f);
        fclose(f);
        char packs[64] = "";
        (void)snprintf(path, sizeof(path), "%s/%05u.packs", dir, no);
        f = fopen(path, "rb");
        if (f != NULL) {
            packs[fread(packs, 1, sizeof(packs) - 1, f)] = '\0';
            fclose(f);
        }
        filo_init(&ctx, &filo_libc_host, persistent_mem, sizeof(persistent_mem), run_mem,
                  sizeof(run_mem));
        if (strstr(packs, "math") != NULL) {
            (void)filo_math_register(&ctx, &filo_libc_math);
        }
        if (strstr(packs, "strings") != NULL) {
            (void)filo_strings_register(&ctx, &filo_libc_strings);
        }
        char why[128];
        bool ok = fbc_read(&unit, data, len, why, sizeof(why));
        nsources = 0;
        if (ok) {
            ok = fbc_decompile(&unit, dmem, sizeof(dmem), folds, &ctx, keep_source, NULL, why,
                               sizeof(why));
        }
        if (!ok || nsources > 64) {
            if (differ < 10) {
                printf("  %05u: %s\n", no, ok ? "more than 64 entries" : why);
            }
            differ++;
            continue;
        }
        (void)snprintf(path, sizeof(path), "%s/%05u.dec", dir, no);
        f = fopen(path, "wb");
        for (uint32_t i = 0; f != NULL && i < nsources; i++) {
            (void)fprintf(f, "; %s\n", names[i]);
            (void)fwrite(texts[i], 1, text_len[i], f);
        }
        if (f != NULL) {
            (void)fclose(f);
        }
        static filo_prog progs[64];
        static filo_bc_entry entries[64];
        bool built = true;
        for (uint32_t i = 0; i < nsources && built; i++) {
            built =
                filo_compile(&ctx, (const uint8_t *)texts[i], text_len[i], &progs[i]) == FILO_OK;
            entries[i].name = names[i];
            entries[i].prog = &progs[i];
        }
        size_t n = 0;
        size_t a = 0;
        size_t b = 0;
        if (built) {
            built = filo_bc_build(&ctx, entries, nsources, again, sizeof(again), &n) == FILO_OK;
        }
        if (built) {
            built = filo_bc_strip(&ctx, data, len, stripped[0], sizeof(stripped[0]), &a) == FILO_OK;
        }
        if (built) {
            built = filo_bc_strip(&ctx, again, n, stripped[1], sizeof(stripped[1]), &b) == FILO_OK;
        }
        if (!built || a != b || memcmp(stripped[0], stripped[1], a) != 0) {
            if (differ < 10) {
                printf("  %05u: %s\n  %.*s\n", no,
                       built ? "built back, not the same unit" : filo_error(&ctx), (int)text_len[0],
                       texts[0]);
            }
            differ++;
            continue;
        }
        done++;
    }
    printf("decompiled: %u built back the same, %u not\n", done, differ);
    CHECK(differ == 0);
    CHECK(done > 1000);
}

/* Each source the corpus kept (NNNNN.filo) formatted by filo_fmt.c, kept
   beside it (NNNNN.fmt) for the Go repository's filofmt to be held to it
   (make govm), byte for byte; formatted again, the same text. */
static char fmt_src[1U << 20U];
static uint8_t fmt_mem[32U << 20U];
static char fmt_once[1U << 20U];

static void test_sources_format_as_filofmt_does(const char *dir) {
    unsigned done = 0;
    unsigned unstable = 0;
    for (unsigned no = 0; no < 100000; no++) {
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%05u.filo", dir, no);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            continue; /* not every unit keeps its source */
        }
        size_t len = fread(fmt_src, 1, sizeof(fmt_src), f);
        fclose(f);
        size_t n = 0;
        const char *text = filo_fmt(fmt_src, len, 2, 80, fmt_mem, sizeof(fmt_mem), &n);
        CHECK(text != NULL);
        if (text == NULL) {
            continue;
        }
        (void)snprintf(path, sizeof(path), "%s/%05u.fmt", dir, no);
        FILE *keep = fopen(path, "wb");
        if (keep != NULL) {
            (void)fwrite(text, 1, n, keep);
            (void)fclose(keep);
        }
        memcpy(fmt_once, text, n);
        size_t m = 0;
        const char *twice = filo_fmt(fmt_once, n, 2, 80, fmt_mem, sizeof(fmt_mem), &m);
        if (twice == NULL || m != n || memcmp(twice, fmt_once, n) != 0) {
            if (unstable < 5) {
                printf("  %05u: formatted twice, not the same\n", no);
            }
            unstable++;
        }
        done++;
    }
    printf("formatted: %u sources, %u not the same formatted twice\n", done, unstable);
    CHECK(done > 1000);
    CHECK(unstable == 0);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: dump_test DIR\n");
        return 2;
    }
    test_numbers_read_as_the_runtime_writes_them();
    test_units_list_and_agree_with_the_loader(argv[1]);
    test_units_decompile_and_build_back(argv[1]);
    test_sources_format_as_filofmt_does(argv[1]);
    if (failures > 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dump tests passed\n");
    return 0;
}
