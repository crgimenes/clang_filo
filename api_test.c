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

/* The table holds FILO_SYMBOLS_MAX names, a build setting; one more is an
   error, not a write past the end. */
static void test_symbol_table_is_bounded(void) {
    start();
    char name[16];
    uint32_t made = CTX.nsymbols;
    for (uint32_t i = made; i < FILO_SYMBOLS_MAX; i++) {
        (void)snprintf(name, sizeof(name), "g%u", i);
        CHECK(filo_set_global(&CTX, name, filo_num(i)) == FILO_OK);
    }
    CHECK(filo_set_global(&CTX, "one_too_many", filo_num(0)) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "too many globals") != NULL);
    filo_value v = {0};
    CHECK(run("(def another 1)", &v) == FILO_ERR);
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
    CHECK(strcmp(filo_error(&CTX), "missing (1): twice") == 0);
}

static int thrice(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)ctx;
    (void)n;
    *out = filo_num(a[0].u.num * 3);
    return FILO_OK;
}

/* Everything a unit needs and the context lacks is refused together: the
   functions it calls and the globals it only reads. */
static void test_bc_load_names_all_that_is_missing(void) {
    start();
    CHECK(filo_register_builtin(&CTX, "twice", twice) == FILO_OK);
    CHECK(filo_register_builtin(&CTX, "thrice", thrice) == FILO_OK);
    const char *src[] = {"(+ (twice W) (thrice 1))"};
    size_t len = build(src, 1);
    const filo_unit *u = NULL;
    start();
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_ERR);
    CHECK(strcmp(filo_error(&CTX), "missing (3): twice thrice W") == 0);
}

/* A global the unit reads and never writes is the host's to provide: the
   load refuses without it; the lazy load leaves it to fail when read, as
   the interpreter does. One the unit writes is its own. */
static void test_bc_globals_read_only_are_externs(void) {
    start();
    const char *src[] = {"(+ W 1)"};
    size_t len = build(src, 1);
    const filo_unit *u = NULL;
    filo_value v;
    start();
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_ERR);
    CHECK(strcmp(filo_error(&CTX), "missing (1): W") == 0);
    CHECK(filo_bc_load_lazy(&CTX, unit_buf, len, &u) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "undefined global: W") != NULL);
    start();
    CHECK(filo_set_global(&CTX, "W", filo_num(41)) == FILO_OK);
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_OK && v.u.num == 42);

    start();
    const char *own[] = {"(def n 5)", "(+ n 1)"};
    len = build(own, 2);
    start();
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
}

/* Where a function comes from is the VM's business: compiled against a
   builtin, the unit runs where the name is a function in Filo, and the
   other way round, the function a value there too. */
static void test_bc_functions_resolve_whatever_their_origin(void) {
    start();
    CHECK(filo_register_builtin(&CTX, "twice", twice) == FILO_OK);
    const char *calls[] = {"(twice 21)"};
    size_t len = build(calls, 1);
    const filo_unit *u = NULL;
    filo_value v;
    start();
    CHECK(run("(def twice (fn (x) (* x 2)))", &v) == FILO_OK);
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_OK && v.u.num == 42);

    start();
    CHECK(run("(def twice (fn (x) (* x 2)))", &v) == FILO_OK);
    const char *uses[] = {"(twice 21)", "(fold (fn (a b) (+ a b)) 0 (map twice (list 1 2)))"};
    len = build(uses, 2);
    start();
    CHECK(filo_register_builtin(&CTX, "twice", twice) == FILO_OK);
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_OK && v.u.num == 42);
    CHECK(filo_bc_run(&CTX, u, "e1", NULL, &v) == FILO_OK && v.u.num == 6);
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_OK && v.u.num == 42);
}

/* What a unit declares is read without loading it: a context lacking what
   it imports can still tell a command from an app. */
static void test_bc_declares_without_loading(void) {
    start();
    CHECK(filo_register_builtin(&CTX, "twice", twice) == FILO_OK);
    const char *src[] = {"(twice 1)", "2"};
    size_t len = build(src, 2);
    start(); /* no twice here: the load would refuse */
    CHECK(filo_bc_declares(unit_buf, len, "e0"));
    CHECK(filo_bc_declares(unit_buf, len, "e1"));
    CHECK(!filo_bc_declares(unit_buf, len, "main"));
    unit_buf[len - 1] ^= 1U; /* damaged: nothing is declared */
    CHECK(!filo_bc_declares(unit_buf, len, "e0"));
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

/* A bundle carries units whole: each member loads and runs as the unit it
   was, the bundle is checked whole, and a name it does not have says so. */
static uint8_t bundle_buf[1U << 16U];
static uint8_t second_unit[1U << 16U];

static void test_bundle_carries_units_whole(void) {
    start();
    const char *one[] = {"(* 6 7)"};
    size_t len_one = build(one, 1);
    memcpy(second_unit, unit_buf, len_one);
    const char *two[] = {"(string (+ 40 2))"};
    size_t len_two = build(two, 1);
    filo_bundle_member m[2] = {{"answer", second_unit, len_one}, {"text", unit_buf, len_two}};
    size_t len = 0;
    CHECK(filo_bundle_build(&CTX, m, 2, bundle_buf, 16, &len) == FILO_ERR); /* says the size */
    size_t need = len;
    CHECK(filo_bundle_build(&CTX, m, 2, bundle_buf, sizeof(bundle_buf), &len) == FILO_OK);
    CHECK(len == need);

    const uint8_t *u = NULL;
    size_t ulen = 0;
    CHECK(filo_bundle_find(&CTX, bundle_buf, len, "answer", &u, &ulen) == FILO_OK);
    CHECK(ulen == len_one && memcmp(u, second_unit, ulen) == 0); /* copied in unchanged */
    CHECK((size_t)(u - bundle_buf) % 8 == 0);
    const filo_unit *unit = NULL;
    filo_value v;
    CHECK(filo_bc_load(&CTX, u, ulen, &unit) == FILO_OK);
    CHECK(filo_bc_run(&CTX, unit, "e0", NULL, &v) == FILO_OK);
    CHECK(v.kind == FILO_NUMBER && v.u.num == 42);
    CHECK(filo_bundle_find(&CTX, bundle_buf, len, "text", &u, &ulen) == FILO_OK);
    CHECK(filo_bc_load(&CTX, u, ulen, &unit) == FILO_OK);
    CHECK(filo_bc_run(&CTX, unit, "e0", NULL, &v) == FILO_OK);
    CHECK(v.kind == FILO_STRING && v.u.str.len == 2 && memcmp(v.u.str.ptr, "42", 2) == 0);

    CHECK(filo_bundle_find(&CTX, bundle_buf, len, "nope", &u, &ulen) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "no bundle member named nope") != NULL);
    bundle_buf[len - 1] ^= 1U; /* a bit anywhere: the whole bundle is refused */
    CHECK(filo_bundle_find(&CTX, bundle_buf, len, "answer", &u, &ulen) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "checksum") != NULL);
    bundle_buf[len - 1] ^= 1U;
    CHECK(filo_bundle_find(&CTX, second_unit, len_one, "answer", &u, &ulen) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "not a Filo bundle") != NULL);

    filo_bundle_member same[2] = {{"x", second_unit, len_one}, {"x", unit_buf, len_two}};
    CHECK(filo_bundle_build(&CTX, same, 2, bundle_buf, sizeof(bundle_buf), &len) == FILO_ERR);
    filo_bundle_member bad[1] = {{"src", (const uint8_t *)"(+ 1 2) is not a unit", 21}};
    CHECK(filo_bundle_build(&CTX, bad, 1, bundle_buf, sizeof(bundle_buf), &len) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "not a unit") != NULL);
}

/* An error says where it happened, apart from its message: the IR from
   the node that failed, a unit from its debug section — the same place —
   and a stripped unit not at all. */
static uint8_t stripped[1U << 16U];

static void test_errors_say_where(void) {
    start();
    uint32_t line = 0;
    uint32_t col = 0;
    filo_prog prog;
    const char *bad = "(list 1\n  2";
    CHECK(filo_compile(&CTX, (const uint8_t *)bad, strlen(bad), &prog) == FILO_ERR);
    CHECK(filo_error_at(&CTX, &line, &col) && line == 2 && col == 4);
    CHECK(strstr(filo_error(&CTX), "line 2, col 4") != NULL);

    const char *src = "(let ((x 1))\n  (+ x \"a\"))";
    filo_value v;
    CHECK(filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) == FILO_OK);
    CHECK(filo_run(&CTX, &prog, NULL, &v) == FILO_ERR);
    CHECK(filo_error_at(&CTX, &line, &col) && line == 2 && col == 3);
    CHECK(strstr(filo_error(&CTX), "line") == NULL); /* the message reads as Go's */

    const char *srcs[] = {src};
    size_t len = build(srcs, 1);
    const filo_unit *u = NULL;
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_ERR);
    line = 0;
    col = 0;
    CHECK(filo_error_at(&CTX, &line, &col) && line == 2 && col == 3);

    size_t slen = 0;
    CHECK(filo_bc_strip(&CTX, unit_buf, len, stripped, sizeof(stripped), &slen) == FILO_OK);
    CHECK(slen < len);
    CHECK(filo_bc_load(&CTX, stripped, slen, &u) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_ERR);
    CHECK(!filo_error_at(&CTX, &line, &col));
    size_t again = 0;
    CHECK(filo_bc_strip(&CTX, stripped, slen, unit_buf, sizeof(unit_buf), &again) == FILO_OK);
    CHECK(again == slen && memcmp(unit_buf, stripped, slen) == 0); /* nothing left to strip */

    const char *ok = "(+ 1 2)";
    CHECK(filo_compile(&CTX, (const uint8_t *)ok, strlen(ok), &prog) == FILO_OK);
    CHECK(filo_run(&CTX, &prog, NULL, &v) == FILO_OK);
    CHECK(!filo_error_at(&CTX, &line, &col)); /* a run that worked leaves no place behind */

    CHECK(filo_set_global(&CTX, "w", filo_num(1)) == FILO_OK);
    filo_seal_globals(&CTX);
    const char *typo = "(+ w\n   wdith)";
    CHECK(filo_compile(&CTX, (const uint8_t *)typo, strlen(typo), &prog) == FILO_ERR);
    CHECK(filo_error_at(&CTX, &line, &col) && line == 2 && col == 4);
}

/* Constants fold before lowering, as the Go engine folds them: a branch an
   if can never take is gone before a sealed table could refuse its names,
   and a call that fails is left to fail when it runs. */
static void test_constants_fold_as_on_go(void) {
    start();
    CHECK(filo_set_global(&CTX, "w", filo_num(1)) == FILO_OK);
    filo_seal_globals(&CTX);
    filo_value v = {0};
    CHECK(run("(if #t (+ w 1) wdith)", &v) == FILO_OK);
    CHECK(v.kind == FILO_NUMBER && v.u.num == 2);
    CHECK(run("(if (< 1 2) 10 wdith)", &v) == FILO_OK); /* (< 1 2) folds to #t first */
    CHECK(v.u.num == 10);
    CHECK(run("(if #f wdith)", &v) == FILO_OK); /* the empty list, as unfolded */
    CHECK(v.kind == FILO_LIST && v.u.seq.len == 0);
    filo_prog prog;
    const char *src = "(+ 1 \"a\")";
    CHECK(filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) == FILO_OK);
    CHECK(filo_run(&CTX, &prog, NULL, &v) == FILO_ERR);
    filo_limits one = {1, 0};
    src = "(* (+ 1 2) (- 10 4))"; /* one node once folded: one step */
    CHECK(filo_compile(&CTX, (const uint8_t *)src, strlen(src), &prog) == FILO_OK);
    CHECK(filo_run(&CTX, &prog, &one, &v) == FILO_OK && v.u.num == 18);
}

/* A run can stop between any two of its instructions and go on later: the
   same value, the same steps; a builtin calling back runs to its end first;
   anything else started on the context cancels the paused run. */
static void test_runs_pause_and_resume(void) {
    start();
    const char *sum[] = {"(def sum (fn (n) (if (= n 0) 0 (+ n (sum (- n 1)))))) (sum 50)"};
    size_t len = build(sum, 1);
    const filo_unit *u = NULL;
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    filo_value v = {0};
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_OK && v.u.num == 1275);
    uint32_t steps = CTX.steps;
    int pauses = 0;
    int rc = filo_bc_start(&CTX, u, "e0", NULL, 1, &v);
    while (rc == FILO_PAUSED) {
        pauses++;
        rc = filo_bc_resume(&CTX, 1, &v);
    }
    CHECK(rc == FILO_OK && v.kind == FILO_NUMBER && v.u.num == 1275);
    CHECK(CTX.steps == steps);
    CHECK((uint32_t)pauses + 1U == steps); /* every instruction boundary of its own */

    /* map calls the function back on the C stack: no pause inside it */
    const char *mapped[] = {"(length (map (fn (x) (* x x)) (range 100)))"};
    len = build(mapped, 1);
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    CHECK(filo_bc_run(&CTX, u, "e0", NULL, &v) == FILO_OK);
    steps = CTX.steps;
    pauses = 0;
    rc = filo_bc_start(&CTX, u, "e0", NULL, 1, &v);
    while (rc == FILO_PAUSED) {
        pauses++;
        rc = filo_bc_resume(&CTX, 1, &v);
    }
    CHECK(rc == FILO_OK && v.u.num == 100 && CTX.steps == steps);
    CHECK((uint32_t)pauses < steps / 10U);

    /* the step limit counts across resumes */
    filo_limits few = {100, 0};
    len = build(sum, 1);
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    rc = filo_bc_start(&CTX, u, "e0", &few, 10, &v);
    while (rc == FILO_PAUSED) {
        rc = filo_bc_resume(&CTX, 10, &v);
    }
    CHECK(rc == FILO_ERR && strstr(filo_error(&CTX), "step limit") != NULL);

    /* anything else started cancels it, and its globals go back */
    const char *setg[] = {"(def g 1) (sum 20)"};
    len = build(setg, 1);
    CHECK(filo_bc_load(&CTX, unit_buf, len, &u) == FILO_OK);
    CHECK(filo_bc_start(&CTX, u, "e0", NULL, 5, &v) == FILO_PAUSED);
    CHECK(run("(+ 1 1)", &v) == FILO_OK);
    CHECK(!filo_get_global(&CTX, "g", &v));
    CHECK(filo_bc_resume(&CTX, 5, &v) == FILO_ERR);
    CHECK(strstr(filo_error(&CTX), "no run is paused") != NULL);
}

int main(void) {
    filo_libc_install();
    test_globals_cross_both_ways();
    test_seal_refuses_new_globals();
    test_seal_is_off_until_asked();
    test_symbol_table_is_bounded();
    test_bundle_carries_units_whole();
    test_errors_say_where();
    test_constants_fold_as_on_go();
    test_runs_pause_and_resume();
    test_host_calls_a_script_function();
    test_exit_reaches_the_host_as_a_value();
    test_math_registers_only_what_the_host_backs();
    test_limits_are_enforced();
    test_bc_short_buffer_says_the_size();
    test_bc_refuses_a_damaged_unit();
    test_bc_missing_builtin_fails_the_load();
    test_bc_load_names_all_that_is_missing();
    test_bc_declares_without_loading();
    test_bc_globals_read_only_are_externs();
    test_bc_functions_resolve_whatever_their_origin();
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
