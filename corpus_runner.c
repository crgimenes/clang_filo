/* Runs the language corpus (the .txt files under testdata/corpus) against the C runtime.
   Same format the Go runner reads (see testdata/corpus/README.md); a file
   that names a pack this runtime does not have is skipped (math and strings
   are registered on demand). Exit status is
   the number of failing cases, capped at 255. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filo.h"
#include "filo_libc.h"
#include "filo_nolibc.h"

enum {
    MEM_PERSISTENT = 4U << 20U,
    MEM_RUN = 8U << 20U,
    LINE_MAX_LEN = 4096,
    TEXT_MAX = 65536,
    BINDINGS_MAX = 16,
};

typedef struct {
    char name[256];
    char expr[1024];
} binding;

typedef struct {
    char name[256];
    int line;
    binding given[BINDINGS_MAX];
    int ngiven;
    binding globals[BINDINGS_MAX];
    int nglobals;
    filo_limits limits;
    bool has_limits;
    bool needs_pow;
    bool needs_math;
    char script[TEXT_MAX];
    char want[TEXT_MAX];
    bool want_err;
    char at[32]; /* "line:col" the error must say, from '--- at'; "" when not checked */
} corpus_case;

static uint8_t persistent_mem[MEM_PERSISTENT];
static uint8_t run_mem[MEM_RUN];

/* the packs the current file asked for */
static bool want_math = false;
static bool want_strings = false;

/* The same corpus against the libc-free host: a runtime that answers
   differently depending on who formats its numbers is two runtimes. */
static bool use_nolibc = false;
/* --vm: every case goes through the bytecode too — compiled, written as a
   unit, loaded back from the bytes and run by the VM. A case that fails
   there fails on the IR too, and both must say it happened at the same
   line and column; steps count differently on the two, so a step limit is
   the one error whose place may differ. */
static bool use_vm = false;
static uint8_t unit_mem[1U << 20U];
/* --write-units DIR: each case's unit is also written there (NNNNN.fbc),
   with the units of its given values (NNNNN.gK.fbc) and what its run gave
   (NNNNN.expect): the bytecode fuzzer's seeds, and the corpus device_test
   runs on a build that cannot compile. */
static const char *units_dir = NULL;
static unsigned case_no = 0;
static bool unit_written = false;

static void write_file(const char *suffix, const void *data, size_t len) {
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%05u%s", units_dir, case_no, suffix);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    (void)fwrite(data, 1, len, f);
    (void)fclose(f);
}

/* Builds prog into unit_mem as a one-entry unit named "main"; 0 when it
   does not build. */
static size_t build_unit(filo_ctx *ctx, const filo_prog *prog) {
    filo_bc_entry entry = {"main", prog};
    size_t len = 0;
    if (filo_bc_build(ctx, &entry, 1, unit_mem, sizeof(unit_mem), &len) != FILO_OK) {
        return 0;
    }
    return len;
}

static int misplaced = 0; /* errors the VM and the IR place differently */

/* --pause N: the VM runs each case N instructions at a time, paused and
   resumed until it ends, and must end as a run in one go does */
static uint32_t pause_budget = 0;
static unsigned long pauses = 0;

static int vm_run(filo_ctx *ctx, const filo_unit *unit, const filo_limits *limits,
                  filo_value *out) {
    if (pause_budget == 0) {
        return filo_bc_run(ctx, unit, "main", limits, out);
    }
    int rc = filo_bc_start(ctx, unit, "main", limits, pause_budget, out);
    while (rc == FILO_PAUSED) {
        pauses++;
        rc = filo_bc_resume(ctx, pause_budget, out);
    }
    return rc;
}

/* --steps FILE: the steps each case takes on the Go engine (tools/gosteps),
   "file<TAB>case<TAB>steps" a line. The IR counts a step per node as the
   Go engine does, and folds constants as it does, so the counts are the
   same — a step limit is behavior a script can see. */
enum { STEPS_MAX = 4096 };
static struct {
    char key[320];
    uint32_t steps;
} go_steps[STEPS_MAX];
static int ngo_steps = 0;
static int miscounted = 0;
static const char *current_file = "";

static void load_steps(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        printf("cannot open %s\n", path);
        return;
    }
    char line[512];
    while (ngo_steps < STEPS_MAX && fgets(line, sizeof(line), f) != NULL) {
        char *tab = strrchr(line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab = '\0';
        snprintf(go_steps[ngo_steps].key, sizeof(go_steps[ngo_steps].key), "%s", line);
        go_steps[ngo_steps].steps = (uint32_t)strtoul(tab + 1, NULL, 10);
        ngo_steps++;
    }
    fclose(f);
}

static void check_steps(const char *name, uint32_t steps) {
    char key[320];
    snprintf(key, sizeof(key), "%s\t%s", current_file, name);
    for (int i = 0; i < ngo_steps; i++) {
        if (strcmp(go_steps[i].key, key) == 0 && go_steps[i].steps != steps) {
            printf("steps: %s/%s takes %u here and %u on the Go engine\n", current_file, name,
                   steps, go_steps[i].steps);
            miscounted++;
        }
    }
}

static int run_program(filo_ctx *ctx, const filo_prog *prog, const filo_limits *limits,
                       filo_value *out) {
    if (!use_vm) {
        return filo_run(ctx, prog, limits, out);
    }
    const filo_unit *unit = NULL;
    size_t len = build_unit(ctx, prog);
    if (len == 0 || filo_bc_load_lazy(ctx, unit_mem, len, &unit) != FILO_OK) {
        return FILO_ERR;
    }
    if (units_dir != NULL) {
        write_file(".fbc", unit_mem, len);
        unit_written = true;
    }
    if (vm_run(ctx, unit, limits, out) == FILO_OK) {
        return FILO_OK;
    }
    char vm_error[FILO_ERROR_MAX];
    uint32_t vm_line = 0;
    uint32_t vm_col = 0;
    bool vm_at = filo_error_at(ctx, &vm_line, &vm_col);
    snprintf(vm_error, sizeof(vm_error), "%s", filo_error(ctx));
    if (strstr(vm_error, "step limit exceeded") != NULL) {
        return FILO_ERR;
    }
    filo_value ignored;
    uint32_t ir_line = 0;
    uint32_t ir_col = 0;
    (void)filo_run(ctx, prog, limits, &ignored);
    bool ir_at = filo_error_at(ctx, &ir_line, &ir_col);
    if (vm_at != ir_at || vm_line != ir_line || vm_col != ir_col) {
        printf("position: the VM says %u:%u, the IR %u:%u (%s)\n", vm_line, vm_col, ir_line, ir_col,
               vm_error);
        misplaced++;
    }
    snprintf(ctx->error, sizeof(ctx->error), "%s", vm_error);
    return FILO_ERR;
}

static void init_ctx(filo_ctx *ctx, void *p, size_t pcap, void *r, size_t rcap) {
    filo_init(ctx, use_nolibc ? &filo_nolibc_host : &filo_libc_host, p, pcap, r, rcap);
    if (want_math) {
        (void)filo_math_register(ctx, use_nolibc ? NULL : &filo_libc_math);
    }
    if (want_strings) {
        (void)filo_strings_register(ctx, use_nolibc ? &filo_nolibc_strings : &filo_libc_strings);
    }
}

/* Reads a "packs:" line; false when it names a pack this runtime lacks. */
static bool select_packs(const char *list) {
    want_math = false;
    want_strings = false;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", list);
    for (const char *tok = strtok(buf, " \t,"); tok != NULL; tok = strtok(NULL, " \t,")) {
        if (strcmp(tok, "math") == 0) {
            want_math = true;
        } else if (strcmp(tok, "strings") == 0) {
            want_strings = true;
        } else {
            return false;
        }
    }
    return true;
}

static bool same_value(const filo_value *a, const filo_value *b) {
    if (a->kind != b->kind) {
        return false;
    }
    if (a->kind == FILO_NUMBER && isnan(a->u.num) && isnan(b->u.num)) {
        return true;
    }
    if (a->kind == FILO_LIST || a->kind == FILO_TUPLE) {
        if (a->u.seq.len != b->u.seq.len) {
            return false;
        }
        for (uint32_t i = 0; i < a->u.seq.len; i++) {
            if (!same_value(&a->u.seq.items[i], &b->u.seq.items[i])) {
                return false;
            }
        }
        return true;
    }
    return filo_equal(a, b);
}

static void show(filo_ctx *ctx, const filo_value *v, char *buf, size_t cap) {
    size_t n = 0;
    if (filo_value_text(ctx, v, buf, cap, &n) != FILO_OK) {
        snprintf(buf, cap, "<%s>", filo_error(ctx));
        return;
    }
    buf[n < cap ? n : cap - 1] = '\0'; /* a longer value is shown cut */
}

/* Evaluates an expression on a fresh context; the value is copied into the
   caller's context as a global so it survives the callee's memory. given is
   the index of a given binding, whose unit is written with the case's; -1
   for anything else. */
static bool eval_into(filo_ctx *dst, const char *global, const char *expr, int given, char *why,
                      size_t cap) {
    static uint8_t p2[1U << 20U];
    static uint8_t r2[1U << 20U];
    filo_ctx *tmp = malloc(sizeof(filo_ctx));
    if (tmp == NULL) {
        snprintf(why, cap, "out of memory");
        return false;
    }
    init_ctx(tmp, p2, sizeof(p2), r2, sizeof(r2));
    filo_prog prog;
    filo_value v;
    bool ok = false;
    if (filo_compile(tmp, (const uint8_t *)expr, strlen(expr), &prog) == FILO_OK &&
        filo_run(tmp, &prog, NULL, &v) == FILO_OK) {
        ok = true;
    }
    if (ok && given >= 0 && units_dir != NULL) {
        size_t len = build_unit(tmp, &prog);
        char suffix[16];
        (void)snprintf(suffix, sizeof(suffix), ".g%d.fbc", given);
        write_file(suffix, unit_mem, len);
    }
    if (!ok) {
        snprintf(why, cap, "\"%s\" does not evaluate: %s", expr, filo_error(tmp));
        free(tmp);
        return false;
    }
    ok = filo_set_global(dst, global, v) == FILO_OK;
    if (!ok) {
        snprintf(why, cap, "%s", filo_error(dst));
    }
    free(tmp);
    return ok;
}

/* What the run gave, in the form device_test reads back: its limits, the
   names of its given values in order, "result <repr>" or "error" (with
   "at L:C" when the error says where), and
   "global <name> <repr>" for each global the case checks. */
static void write_expect(filo_ctx *ctx, const corpus_case *c, const filo_value *got) {
    static char text[TEXT_MAX];
    static char repr[TEXT_MAX];
    size_t n = 0;
    size_t len = 0;
    if (c->has_limits) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "limits %u %u\n", c->limits.step_limit,
                              c->limits.recursion_limit);
    }
    for (int i = 0; i < c->ngiven && n < sizeof(text); i++) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "given %s\n", c->given[i].name);
    }
    uint32_t line = 0;
    uint32_t col = 0;
    if (got == NULL && filo_error_at(ctx, &line, &col)) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "error at %u:%u\n", line, col);
    } else if (got == NULL) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "error\n");
    } else if (filo_value_repr(ctx, got, repr, sizeof(repr), &len) == FILO_OK && n < sizeof(text) &&
               len < sizeof(repr)) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "result %.*s\n", (int)len, repr);
    }
    for (int i = 0; i < c->nglobals && got != NULL && n < sizeof(text); i++) {
        filo_value v;
        if (filo_get_global(ctx, c->globals[i].name, &v) &&
            filo_value_repr(ctx, &v, repr, sizeof(repr), &len) == FILO_OK && len < sizeof(repr)) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "global %s %.*s\n",
                                  c->globals[i].name, (int)len, repr);
        }
    }
    if (n < sizeof(text)) {
        write_file(".expect", text, n);
    }
}

/* The error says it happened at want ("line:col"), or want is "". On the
   VM a step limit is the one error whose place may differ from the IR's:
   the two count steps differently. */
static bool error_at_matches(const filo_ctx *ctx, const char *want) {
    if (want[0] == '\0') {
        return true;
    }
    if (use_vm && strstr(filo_error(ctx), "step limit exceeded") != NULL) {
        return true;
    }
    uint32_t line = 0;
    uint32_t col = 0;
    if (!filo_error_at(ctx, &line, &col)) {
        return false;
    }
    char at[32];
    snprintf(at, sizeof(at), "%u:%u", line, col);
    return strcmp(at, want) == 0;
}

static bool run_case(const corpus_case *c, char *why, size_t cap) {
    filo_ctx *ctx = malloc(sizeof(filo_ctx));
    if (ctx == NULL) {
        snprintf(why, cap, "out of memory");
        return false;
    }
    init_ctx(ctx, persistent_mem, sizeof(persistent_mem), run_mem, sizeof(run_mem));
    for (int i = 0; i < c->ngiven; i++) {
        if (!eval_into(ctx, c->given[i].name, c->given[i].expr, i, why, cap)) {
            free(ctx);
            return false;
        }
    }
    filo_prog prog;
    filo_value got;
    bool failed = true;
    if (filo_compile(ctx, (const uint8_t *)c->script, strlen(c->script), &prog) == FILO_OK &&
        run_program(ctx, &prog, c->has_limits ? &c->limits : NULL, &got) == FILO_OK) {
        failed = false;
        if (!use_vm && ngo_steps > 0 && c->ngiven == 0) {
            check_steps(c->name, ctx->steps);
        }
    }
    if (unit_written) {
        write_expect(ctx, c, failed ? NULL : &got);
    }
    if (c->want_err) {
        bool ok = true;
        if (!failed) {
            char buf[512];
            show(ctx, &got, buf, sizeof(buf));
            snprintf(why, cap, "expected an error, got %s", buf);
            ok = false;
        }
        if (ok && !error_at_matches(ctx, c->at)) {
            uint32_t line = 0;
            uint32_t col = 0;
            (void)filo_error_at(ctx, &line, &col);
            snprintf(why, cap, "the error is at %u:%u, want %s (%s)", line, col, c->at,
                     filo_error(ctx));
            ok = false;
        }
        free(ctx);
        return ok;
    }
    if (failed) {
        snprintf(why, cap, "unexpected error: %s", filo_error(ctx));
        free(ctx);
        return false;
    }
    /* the expectation is evaluated with its own memory, then compared */
    if (!eval_into(ctx, "__want", c->want, -1, why, cap)) {
        free(ctx);
        return false;
    }
    filo_value want;
    (void)filo_get_global(ctx, "__want", &want);
    if (!same_value(&got, &want)) {
        char g[512];
        char w[512];
        show(ctx, &got, g, sizeof(g));
        show(ctx, &want, w, sizeof(w));
        snprintf(why, cap, "got %s, want %s", g, w);
        free(ctx);
        return false;
    }
    for (int i = 0; i < c->nglobals; i++) {
        filo_value v;
        if (!filo_get_global(ctx, c->globals[i].name, &v)) {
            snprintf(why, cap, "global %s not set after the run", c->globals[i].name);
            free(ctx);
            return false;
        }
        if (!eval_into(ctx, "__want", c->globals[i].expr, -1, why, cap)) {
            free(ctx);
            return false;
        }
        (void)filo_get_global(ctx, "__want", &want);
        if (!same_value(&v, &want)) {
            char g[512];
            char w[512];
            show(ctx, &v, g, sizeof(g));
            show(ctx, &want, w, sizeof(w));
            snprintf(why, cap, "global %s is %s, want %s", c->globals[i].name, g, w);
            free(ctx);
            return false;
        }
    }
    free(ctx);
    return true;
}

/* ---- the file format ---- */

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[n - 1] = '\0';
        n--;
    }
}

static bool parse_binding(const char *s, binding *b) {
    const char *eq = strchr(s, '=');
    if (eq == NULL) {
        return false;
    }
    size_t nl = (size_t)(eq - s);
    while (nl > 0 && (s[nl - 1] == ' ' || s[nl - 1] == '\t')) {
        nl--;
    }
    const char *e = eq + 1;
    while (*e == ' ' || *e == '\t') {
        e++;
    }
    if (nl == 0 || nl >= sizeof(b->name) || *e == '\0' || strlen(e) >= sizeof(b->expr)) {
        return false;
    }
    memcpy(b->name, s, nl);
    b->name[nl] = '\0';
    strcpy(b->expr, e);
    rstrip(b->expr);
    return true;
}

static bool parse_limits(const char *s, filo_limits *l) {
    memset(l, 0, sizeof(*l));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s);
    for (char *tok = strtok(buf, " \t"); tok != NULL; tok = strtok(NULL, " \t")) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            return false;
        }
        *eq = '\0';
        long v = strtol(eq + 1, NULL, 10);
        if (v <= 0) {
            return false;
        }
        if (strcmp(tok, "steps") == 0) {
            l->step_limit = (uint32_t)v;
        } else if (strcmp(tok, "recursion") == 0) {
            l->recursion_limit = (uint32_t)v;
        } else {
            return false;
        }
    }
    return true;
}

typedef enum { SEC_NONE, SEC_SCRIPT, SEC_WANT, SEC_GLOBALS } section;

static int failures = 0;
static int passed = 0;

static int skipped = 0;

static void finish_case(const char *file, corpus_case *c) {
    if (c->name[0] == '\0') {
        return;
    }
    if (use_nolibc && (c->needs_pow || c->needs_math)) {
        skipped++; /* this host cannot compute it, which is not a disagreement */
        memset(c, 0, sizeof(*c));
        return;
    }
    rstrip(c->script);
    rstrip(c->want);
    char why[1024] = {0};
    unit_written = false;
    bool ok = run_case(c, why, sizeof(why));
    if (unit_written) {
        case_no++;
    }
    if (ok) {
        passed++;
    } else {
        failures++;
        printf("FAIL %s/%s (line %d): %s\n", file, c->name, c->line, why);
    }
    memset(c, 0, sizeof(*c));
}

static void append_line(char *dst, size_t cap, const char *line) {
    size_t n = strlen(dst);
    size_t l = strlen(line);
    if (n + l + 2 > cap) {
        return;
    }
    if (n > 0) {
        dst[n] = '\n';
        n++;
    }
    memcpy(dst + n, line, l + 1);
}

static bool run_file(const char *path) {
    want_math = false;
    want_strings = false;
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        printf("cannot open %s\n", path);
        return false;
    }
    const char *base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    current_file = base;
    corpus_case *c = calloc(1, sizeof(corpus_case));
    if (c == NULL) {
        fclose(f);
        return false;
    }
    section sec = SEC_NONE;
    bool in_case = false;
    char line[LINE_MAX_LEN];
    int n = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        n++;
        rstrip(line);
        if (strncmp(line, "=== ", 4) == 0) {
            finish_case(base, c);
            snprintf(c->name, sizeof(c->name), "%s", line + 4);
            c->line = n;
            sec = SEC_SCRIPT;
            in_case = true;
            continue;
        }
        if (strncmp(line, "--- ", 4) == 0) {
            const char *s = line + 4;
            if (strcmp(s, "want") == 0) {
                sec = SEC_WANT;
            } else if (strcmp(s, "error") == 0) {
                c->want_err = true;
                sec = SEC_NONE;
            } else if (strcmp(s, "globals") == 0) {
                sec = SEC_GLOBALS;
            } else if (strncmp(s, "at ", 3) == 0) {
                snprintf(c->at, sizeof(c->at), "%s", s + 3);
                sec = SEC_NONE;
            }
            continue;
        }
        if (!in_case) {
            if (strncmp(line, "packs:", 6) == 0 && !select_packs(line + 6)) {
                printf("skip %s: needs packs%s\n", base, line + 6);
                free(c);
                fclose(f);
                return true;
            }
            continue;
        }
        switch (sec) {
        case SEC_SCRIPT:
            if (c->script[0] == '\0' && strncmp(line, "given ", 6) == 0 &&
                c->ngiven < BINDINGS_MAX) {
                (void)parse_binding(line + 6, &c->given[c->ngiven]);
                c->ngiven++;
                continue;
            }
            if (c->script[0] == '\0' && strncmp(line, "needs ", 6) == 0) {
                if (strcmp(line + 6, "host-pow") == 0) {
                    c->needs_pow = true;
                } else if (strcmp(line + 6, "host-math") == 0) {
                    c->needs_math = true;
                }
                continue;
            }
            if (c->script[0] == '\0' && strncmp(line, "limits ", 7) == 0) {
                c->has_limits = parse_limits(line + 7, &c->limits);
                continue;
            }
            append_line(c->script, sizeof(c->script), line);
            break;
        case SEC_WANT:
            if (line[0] == '\0') {
                if (c->want[0] != '\0') {
                    sec = SEC_NONE; /* the expectation ended; only comments may follow */
                }
                break;
            }
            append_line(c->want, sizeof(c->want), line);
            break;
        case SEC_GLOBALS:
            if (line[0] != '\0' && line[0] != '#' && c->nglobals < BINDINGS_MAX &&
                parse_binding(line, &c->globals[c->nglobals])) {
                c->nglobals++;
            }
            break;
        case SEC_NONE:
            break;
        }
    }
    finish_case(base, c);
    free(c);
    fclose(f);
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf(
            "usage: corpus_runner [--nolibc] [--vm [--pause N]] [--steps FILE] [--write-units DIR] "
            "FILE...\n");
        return 2;
    }
    int first = 1;
    while (first < argc && argv[first][0] == '-' && argv[first][1] == '-') {
        if (strcmp(argv[first], "--nolibc") == 0) {
            use_nolibc = true;
        } else if (strcmp(argv[first], "--vm") == 0) {
            use_vm = true;
        } else if (strcmp(argv[first], "--pause") == 0 && first + 1 < argc) {
            first++;
            pause_budget = (uint32_t)strtoul(argv[first], NULL, 10);
        } else if (strcmp(argv[first], "--steps") == 0 && first + 1 < argc) {
            first++;
            load_steps(argv[first]);
        } else if (strcmp(argv[first], "--write-units") == 0 && first + 1 < argc) {
            first++;
            units_dir = argv[first];
        } else {
            printf("unknown flag %s\n", argv[first]);
            return 2;
        }
        first++;
    }
    if (!use_nolibc) {
        filo_libc_install(); /* pow with a fractional exponent needs libm */
    }
    for (int i = first; i < argc; i++) {
        (void)run_file(argv[i]);
    }
    if (misplaced > 0) {
        printf("%d error(s) placed differently by the VM and the IR\n", misplaced);
        failures += misplaced;
    }
    if (miscounted > 0) {
        printf("%d case(s) take other steps than on the Go engine\n", miscounted);
        failures += miscounted;
    }
    if (skipped > 0) {
        printf("%d passed, %d failed, %d skipped (host cannot compute them)\n", passed, failures,
               skipped);
        return failures > 255 ? 255 : failures;
    }
    if (pauses > 0) {
        printf("(%lu pauses, every run resumed to its end)\n", pauses);
    }
    printf("%d passed, %d failed\n", passed, failures);
    return failures > 255 ? 255 : failures;
}
