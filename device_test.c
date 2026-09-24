/* The corpus on the device build: filo.c compiled with FILO_VM_ONLY, so no
   parser, no IR and no compiler, with the libc-free number host. The units
   come from corpus_runner --nolibc --vm --write-units DIR, with what each
   run gave there; every one must give the same here. A build that cannot
   compile running what another build compiled is the whole point of the
   bytecode, so this is the check that it holds.

   usage: device_test DIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filo.h"
#include "filo_math.h"
#include "filo_nolibc.h"
#include "filo_strings.h"

enum {
    MEM_PERSISTENT = 4U << 20U,
    MEM_RUN = 8U << 20U,
    UNIT_MAX = 1U << 20U,
    TEXT_MAX = 65536,
    GIVEN_MAX = 16,
};

static uint8_t persistent_mem[MEM_PERSISTENT];
static uint8_t run_mem[MEM_RUN];
static uint8_t given_persistent[1U << 20U];
static uint8_t given_run[1U << 20U];
static uint8_t unit_mem[UNIT_MAX];
static uint8_t given_unit[UNIT_MAX];
static char expect[TEXT_MAX];
static char repr[TEXT_MAX];

static const char *dir = NULL;

static size_t read_file(unsigned no, const char *suffix, void *dst, size_t cap) {
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%05u%s", dir, no, suffix);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t n = fread(dst, 1, cap - 1, f);
    (void)fclose(f);
    return n;
}

static void init_ctx(filo_ctx *ctx, void *p, size_t pcap, void *r, size_t rcap) {
    filo_init(ctx, &filo_nolibc_host, p, pcap, r, rcap);
    (void)filo_math_register(ctx, NULL);
    (void)filo_strings_register(ctx, &filo_nolibc_strings);
}

static bool load_run(filo_ctx *ctx, const uint8_t *unit, size_t len, const filo_limits *limits,
                     filo_value *out) {
    const filo_unit *u = NULL;
    if (filo_bc_load_lazy(ctx, unit, len, &u) != FILO_OK) {
        return false;
    }
    return filo_bc_run(ctx, u, "main", limits, out) == FILO_OK;
}

/* A given value is run on its own context and copied in, as the corpus
   runner does with the source. */
static bool set_given(filo_ctx *ctx, unsigned no, int k, const char *name, char *why, size_t cap) {
    static filo_ctx tmp;
    char suffix[16];
    (void)snprintf(suffix, sizeof(suffix), ".g%d.fbc", k);
    size_t len = read_file(no, suffix, given_unit, sizeof(given_unit));
    init_ctx(&tmp, given_persistent, sizeof(given_persistent), given_run, sizeof(given_run));
    filo_value v;
    if (len == 0 || !load_run(&tmp, given_unit, len, NULL, &v)) {
        snprintf(why, cap, "given %s: %s", name, len == 0 ? "no unit" : filo_error(&tmp));
        return false;
    }
    if (filo_set_global(ctx, name, v) != FILO_OK) {
        snprintf(why, cap, "given %s: %s", name, filo_error(ctx));
        return false;
    }
    return true;
}

static bool same_repr(filo_ctx *ctx, const filo_value *v, const char *want) {
    size_t len = 0;
    if (filo_value_repr(ctx, v, repr, sizeof(repr), &len) != FILO_OK) {
        return false;
    }
    if (strlen(want) != len) {
        return false;
    }
    return memcmp(want, repr, len) == 0;
}

static bool check(filo_ctx *ctx, char *line, bool ok, const filo_value *got, char *why,
                  size_t cap) {
    if (strncmp(line, "error", 5) == 0) {
        if (ok) {
            snprintf(why, cap, "expected an error");
            return false;
        }
        uint32_t at_line = 0;
        uint32_t at_col = 0;
        char at[32] = "error";
        if (filo_error_at(ctx, &at_line, &at_col)) {
            snprintf(at, sizeof(at), "error at %u:%u", at_line, at_col);
        }
        if (strcmp(line, at) != 0) {
            snprintf(why, cap, "%s, want %s (%s)", at, line, filo_error(ctx));
            return false;
        }
        return true;
    }
    if (!ok) {
        snprintf(why, cap, "unexpected error: %s", filo_error(ctx));
        return false;
    }
    if (strncmp(line, "result ", 7) == 0) {
        if (!same_repr(ctx, got, line + 7)) {
            snprintf(why, cap, "got %s, want %s", repr, line + 7);
            return false;
        }
        return true;
    }
    char *name = line + 7;
    char *want = strchr(name, ' ');
    if (strncmp(line, "global ", 7) != 0 || want == NULL) {
        snprintf(why, cap, "bad expectation: %s", line);
        return false;
    }
    *want = '\0';
    filo_value v;
    if (!filo_get_global(ctx, name, &v) || !same_repr(ctx, &v, want + 1)) {
        snprintf(why, cap, "global %s is %s, want %s", name, repr, want + 1);
        return false;
    }
    return true;
}

static bool run_case(unsigned no, char *why, size_t cap) {
    static filo_ctx ctx;
    char *lines[GIVEN_MAX * 3];
    int nlines = 0;
    for (char *p = expect; *p != '\0' && nlines < GIVEN_MAX * 3;) {
        lines[nlines] = p;
        nlines++;
        char *end = strchr(p, '\n');
        if (end == NULL) {
            break;
        }
        *end = '\0';
        p = end + 1;
    }
    init_ctx(&ctx, persistent_mem, sizeof(persistent_mem), run_mem, sizeof(run_mem));
    filo_limits limits = {0};
    bool has_limits = false;
    int ngiven = 0;
    int i = 0;
    for (; i < nlines; i++) {
        if (strncmp(lines[i], "limits ", 7) == 0) {
            char *rest = NULL;
            limits.step_limit = (uint32_t)strtoul(lines[i] + 7, &rest, 10);
            limits.recursion_limit = (uint32_t)strtoul(rest, NULL, 10);
            has_limits = true;
        } else if (strncmp(lines[i], "given ", 6) == 0) {
            if (!set_given(&ctx, no, ngiven, lines[i] + 6, why, cap)) {
                return false;
            }
            ngiven++;
        } else {
            break;
        }
    }
    size_t len = read_file(no, ".fbc", unit_mem, sizeof(unit_mem));
    filo_value got;
    bool ok = false;
    if (len > 0) {
        ok = load_run(&ctx, unit_mem, len, has_limits ? &limits : NULL, &got);
    }
    if (i == nlines) {
        snprintf(why, cap, "no expectation");
        return false;
    }
    for (; i < nlines; i++) {
        if (!check(&ctx, lines[i], ok, &got, why, cap)) {
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        printf("usage: device_test DIR\n"
               "Runs the units corpus_runner --nolibc --vm --write-units DIR wrote, on a\n"
               "build without the compiler, and checks each gives what it gave there.\n");
        return argc == 2 ? 0 : 2;
    }
    dir = argv[1];
    unsigned passed = 0;
    unsigned failed = 0;
    for (unsigned no = 0;; no++) {
        size_t n = read_file(no, ".expect", expect, sizeof(expect));
        if (n == 0) {
            break;
        }
        expect[n] = '\0';
        char why[1024] = {0};
        if (run_case(no, why, sizeof(why))) {
            passed++;
        } else {
            failed++;
            printf("FAIL %s/%05u: %s\n", dir, no, why);
        }
    }
    printf("device build: %u passed, %u failed\n", passed, failed);
    if (passed == 0) {
        return 1;
    }
    return failed > 255 ? 255 : (int)failed;
}
