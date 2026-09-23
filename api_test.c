/* Host API behavior the corpus cannot express: sealed globals, values
   crossing in and out, and calling a script function from C. Exit status is
   the number of failures. */
#include <stdio.h>
#include <string.h>

#include "filo.h"
#include "filo_libc.h"

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

enum {
    MEM_PERSISTENT = 1U << 20U,
    MEM_RUN = 1U << 20U,
};

static uint8_t persistent_mem[MEM_PERSISTENT];
static uint8_t run_mem[MEM_RUN];
static filo_ctx CTX;

static void start(void) {
    filo_init(&CTX, &filo_libc_host, persistent_mem, sizeof(persistent_mem), run_mem,
              sizeof(run_mem));
}

/* Compiles and runs src, returning FILO_OK or FILO_ERR. */
static int run(const char *src, filo_value *out) {
    filo_prog prog;
    if (filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) != FILO_OK) {
        return FILO_ERR;
    }
    return filo_run(&CTX, &prog, NULL, out);
}

static void test_globals_cross_both_ways(void) {
    start();
    CHECK(filo_set_global(&CTX, "score", filo_num(7)) == FILO_OK);
    filo_value v = {0};
    CHECK(run("(set score (* score 6))", &v) == FILO_OK);
    CHECK(filo_get_global(&CTX, "score", &v));
    CHECK(v.kind == FILO_NUMBER && v.u.num == 42);

    /* a failed run leaves the globals as they were */
    CHECK(run("(do (set score 1) (error \"stop\"))", &v) == FILO_ERR);
    CHECK(filo_get_global(&CTX, "score", &v));
    CHECK(v.u.num == 42);
}

static void test_seal_refuses_new_globals(void) {
    start();
    filo_value v = {0};
    CHECK(filo_set_global(&CTX, "w", filo_num(80)) == FILO_OK);
    filo_seal_globals(&CTX);

    /* what the host created still reads and writes */
    CHECK(run("(set w (- w 1))", &v) == FILO_OK);
    CHECK(v.u.num == 79);

    /* a name the host never created fails at compile time, not at the first
       time the line happens to run, and the message names it */
    filo_prog prog;
    const char *src = "(set wdith 10)";
    CHECK(filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "wdith") != NULL);
    CHECK(strstr(filo_error(&CTX), "undefined global") != NULL);

    /* reading an unknown global is refused the same way */
    src = "(+ w hieght)";
    CHECK(filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) == FILO_ERR);

    /* locals are not globals: let bindings and parameters still work, since
       the lowering resolves scope before it ever interns a name */
    CHECK(run("(let ((x 2) (y 3)) (* x y))", &v) == FILO_OK);
    CHECK(v.u.num == 6);
    CHECK(run("((fn (a b) (+ a b)) 1 2)", &v) == FILO_OK);
    CHECK(v.u.num == 3);
}

static void test_seal_is_off_until_asked(void) {
    start();
    filo_value v = {0};
    CHECK(run("(set fresh 5)", &v) == FILO_OK);
    CHECK(filo_get_global(&CTX, "fresh", &v));
    CHECK(v.u.num == 5);
}

static void test_host_calls_a_script_function(void) {
    start();
    filo_value v = {0};
    CHECK(run("(def double (fn (n) (* n 2)))", &v) == FILO_OK);
    filo_value fn = {0};
    CHECK(filo_get_global(&CTX, "double", &fn));
    CHECK(fn.kind == FILO_FUNC);

    /* the function survives its run: the value went to the persistent arena
       with the frame it captured */
    filo_prog prog;
    const char *src = "(double 21)";
    CHECK(filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) == FILO_OK);
    CHECK(filo_run(&CTX, &prog, NULL, &v) == FILO_OK);
    CHECK(v.u.num == 42);

    filo_value arg = filo_num(4);
    CHECK(filo_call(&CTX, &fn, &arg, 1, &v) == FILO_OK);
    CHECK(v.u.num == 8);
}

static int host_calls = 0;

static int b_set_key(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    (void)ctx;
    (void)args;
    (void)n;
    host_calls++;
    *out = filo_num(0);
    return FILO_OK;
}

/* A host that registers builtins must not have to tell "the script asked to
   stop" apart from "the script broke": exit is not an error, wherever it is
   raised, and the builtin whose argument raised it is never called. */
static void test_exit_reaches_the_host_as_a_value(void) {
    start();
    CHECK(filo_register_builtin(&CTX, "set-key", b_set_key) == FILO_OK);
    filo_value v = {0};

    host_calls = 0;
    CHECK(run("(let () (set-key \"q\" (exit 5)))", &v) == FILO_OK);
    CHECK(v.kind == FILO_NUMBER && v.u.num == 5);
    CHECK(host_calls == 0);

    host_calls = 0;
    CHECK(run("(set-key \"q\" 1)", &v) == FILO_OK);
    CHECK(host_calls == 1);

    /* and from inside a closure a builtin of the core runs for us */
    CHECK(run("(map (fn (x) (exit 9)) (list 1 2))", &v) == FILO_OK);
    CHECK(v.u.num == 9);
}

/* A host that has no libm supplies no table, and then the builtins it cannot
   back are not registered at all: a script naming one fails to compile, which
   is earlier and clearer than failing at the call. */
static void test_math_registers_only_what_the_host_backs(void) {
    start();
    CHECK(filo_math_register(&CTX, NULL) == FILO_OK);
    filo_value v = {0};
    CHECK(run("(floor 2.7)", &v) == FILO_OK);
    CHECK(v.u.num == 2);
    CHECK(run("(math-max 1 5 3)", &v) == FILO_OK);
    CHECK(v.u.num == 5);

    filo_prog prog;
    const char *src = "(sqrt 16)";
    CHECK(filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) == FILO_OK);
    CHECK(filo_run(&CTX, &prog, NULL, &v) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "sqrt") != NULL);

    /* with a table, the same name works */
    start();
    CHECK(filo_math_register(&CTX, &filo_libc_math) == FILO_OK);
    CHECK(run("(sqrt 16)", &v) == FILO_OK);
    CHECK(v.u.num == 4);
}

static void test_limits_are_enforced(void) {
    start();
    filo_prog prog;
    const char *src = "(let () (def loop (fn (n) (loop (+ n 1)))) (loop 0))";
    CHECK(filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) == FILO_OK);
    filo_limits tight = {500, 16};
    filo_value v = {0};
    CHECK(filo_run(&CTX, &prog, &tight, &v) == FILO_ERR);
    CHECK(filo_error(&CTX)[0] != '\0');
}

/* ---- bytecode ---- */

static uint8_t unit_buf[1U << 16U];

/* Builds a unit from sources compiled in CTX: entries named after the
   sources' order, "e0", "e1", … */
static size_t build(const char *const *srcs, uint32_t n) {
    static filo_prog progs[4];
    static const char *names[4] = {"e0", "e1", "e2", "e3"};
    filo_bc_entry entries[4];
    for (uint32_t i = 0; i < n; i++) {
        CHECK(filo_compile(&CTX, (const uint8_t *)srcs[i], strlen(srcs[i]), &progs[i]) == FILO_OK);
        entries[i].name = names[i];
        entries[i].prog = &progs[i];
    }
    size_t len = 0;
    CHECK(filo_bc_build(&CTX, entries, n, unit_buf, sizeof(unit_buf), &len) == FILO_OK);
    return len;
}

static void test_bc_short_buffer_says_the_size(void) {
    start();
    filo_prog prog;
    const char *src = "(+ 1 2)";
    CHECK(filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) == FILO_OK);
    filo_bc_entry e = {"main", &prog};
    size_t len = 0;
    CHECK(filo_bc_build(&CTX, &e, 1, unit_buf, 8, &len) == FILO_ERR);
    CHECK(len > 8);
    size_t need = len;
    CHECK(filo_bc_build(&CTX, &e, 1, unit_buf, need, &len) == FILO_OK);
    CHECK(len == need);
}

static void test_bc_refuses_a_damaged_unit(void) {
    start();
    const char *src[] = {"(+ 1 2)"};
    size_t len = build(src, 1);
    const filo_unit *u = NULL;
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    unit_buf[len - 1] ^= 1U; /* one bit anywhere */
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "checksum") != NULL);
    unit_buf[len - 1] ^= 1U;
    unit_buf[0] = 'X';
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "not a Filo unit") != NULL);
    unit_buf[0] = 0x7F;
    CHECK(filo_bc_load(&CTX, unit_buf, len - 1, &u) == FILO_ERR);
}

static int twice(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    double x = 0;
    if (n != 1 || filo_arg_num(ctx, &args[0], &x) != FILO_OK) {
        return filo_fail(ctx, "twice expects a number");
    }
    *out = filo_num(x * 2);
    return FILO_OK;
}

/* The imports are the capability list: a context without one of them does
   not load the unit at all, rather than failing at the call. */
static void test_bc_missing_builtin_fails_the_load(void) {
    start();
    CHECK(filo_register_builtin(&CTX, "twice", twice) == FILO_OK);
    const char *src[] = {"(twice 21)"};
    size_t len = build(src, 1);
    const filo_unit *u = NULL;
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    filo_value v;
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_OK);
    CHECK(v.kind == FILO_NUMBER && v.u.num == 42);
    CHECK(filo_bc_has(u, "e0"));
    CHECK(!filo_bc_has(u, "nope"));
    CHECK(filo_bc_run(&CTX, u, "nope", NULL, &v) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "no entry point") != NULL);
    start(); /* a context that never registered twice */
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_ERR);
    CHECK(strcmp(filo_error(&CTX), "missing builtin: twice") == 0);
}

/* Entry points of one unit share its globals, as a screen's hooks share
   what its init defined. */
static void test_bc_entries_share_globals(void) {
    start();
    const char *src[] = {"(def n 5)", "(set n (+ n 1))"};
    size_t len = build(src, 2);
    const filo_unit *u = NULL;
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    filo_value v;
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e1", NULL, &v) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e1", NULL, &v) == FILO_OK);
    CHECK(filo_get_global(&CTX, "n", &v) && v.u.num == 7);
}

/* The IR and the bytecode call each other both ways, and the host calls a
   bytecode function like any other: a closure is a closure. */
static void test_bc_and_ir_call_each_other(void) {
    start();
    filo_value v;
    CHECK(run("(def inc (fn (x) (+ x 1)))", &v) == FILO_OK);
    const char *src[] = {"(do (def dbl (fn (x) (* x 2))) (inc 41))"};
    size_t len = build(src, 1);
    const filo_unit *u = NULL;
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_OK);
    CHECK(v.kind == FILO_NUMBER && v.u.num == 42); /* bytecode called the IR's inc */
    CHECK(run("(fold (fn (acc x) (+ acc x)) 0 (map dbl (list 1 2 3)))", &v) == FILO_OK);
    CHECK(v.kind == FILO_NUMBER && v.u.num == 12); /* map called the bytecode's dbl */
    filo_value fn;
    CHECK(filo_get_global(&CTX, "dbl", &fn));
    filo_value arg = filo_num(5);
    CHECK(filo_call(&CTX, &fn, &arg, 1, &v) == FILO_OK);
    CHECK(v.kind == FILO_NUMBER && v.u.num == 10);
}

/* A sealed context names its globals once; a unit that needs another one
   is refused when it loads. */
static void test_bc_sealed_context_refuses_unknown_globals(void) {
    start();
    CHECK(filo_set_global(&CTX, "known", filo_num(1)) == FILO_OK);
    const char *src[] = {"(+ known other)"};
    size_t len = build(src, 1);
    start();
    CHECK(filo_set_global(&CTX, "known", filo_num(1)) == FILO_OK);
    filo_seal_globals(&CTX);
    const filo_unit *u = NULL;
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "other") != NULL);
}

int main(void) {
    filo_libc_install();
    test_globals_cross_both_ways();
    test_seal_refuses_new_globals();
    test_seal_is_off_until_asked();
    test_host_calls_a_script_function();
    test_exit_reaches_the_host_as_a_value();
    test_math_registers_only_what_the_host_backs();
    test_limits_are_enforced();
    test_bc_short_buffer_says_the_size();
    test_bc_refuses_a_damaged_unit();
    test_bc_missing_builtin_fails_the_load();
    test_bc_entries_share_globals();
    test_bc_and_ir_call_each_other();
    test_bc_sealed_context_refuses_unknown_globals();
    if (failures > 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("api tests passed\n");
    return 0;
}
