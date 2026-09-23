/* Filo runtime in C: the same language as the Go engine, lowered to the IR
   described in docs/ir.md and evaluated by the same rules, so a script's
   outcome is identical on both. Two files (filo.h, filo.c), no libc beyond
   memcpy/memcmp/strlen, no allocation after init: the host hands over two
   memory blocks and everything lives in them.

   Memory model. The persistent arena holds compiled programs, the symbol
   table and the globals; it only grows. The run arena holds everything a
   single run creates (frames, lists, strings, closures) and is reset at the
   start of the next run. Globals written by a run are copied out into the
   persistent arena when the run ends, so they survive; the run's result value
   stays valid until the next run or compile. A script that exhausts either
   arena fails with an error, never corrupts memory.

   Built with FILO_VM_ONLY defined (for the whole build, this header
   included), the runtime keeps only what runs a loaded unit: no parser, no
   IR, no compiler. That is the device build of docs/bytecode.md. */
#ifndef FILO_H
#define FILO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    FILO_OK = 0,
    FILO_ERR = 1, /* filo_error() describes it */
};

enum {
    FILO_ERROR_MAX = 256,
    FILO_BUILTINS_MAX = 96,
    FILO_STEP_LIMIT_DEFAULT = 100000,
    FILO_RECURSION_LIMIT_DEFAULT = 128,
    FILO_RANGE_MAX = 1048576,   /* 2^20 elements; same ceiling as the Go runtime */
    FILO_PARSE_DEPTH_MAX = 256, /* the Go engine allows 4096; this is a
                                   configuration, not part of the language */
    FILO_EVAL_DEPTH_MAX = 512,  /* nesting of eval() calls: bounds the C stack */
};

/* Globals a context can name. Each costs about 40 bytes on a 32-bit target
   in every context (the value, its saved copy, the name, three flags), so a
   board with little RAM sizes it to what its programs use. It shapes
   filo_ctx: the whole build must agree on it, as on FILO_VM_ONLY. */
#ifndef FILO_SYMBOLS_MAX
#define FILO_SYMBOLS_MAX 512
#endif

typedef enum {
    FILO_NUMBER = 0,
    FILO_BOOL,
    FILO_STRING,
    FILO_LIST,
    FILO_TUPLE,
    FILO_FUNC,
} filo_kind;

typedef struct filo_value filo_value;
typedef struct filo_func filo_func;
typedef struct filo_instr filo_instr;
typedef struct filo_ctx filo_ctx;

/* Strings are byte sequences with a length: a script may contain \0. */
typedef struct {
    const uint8_t *ptr;
    uint32_t len;
} filo_str;

typedef struct {
    filo_value *items;
    uint32_t len;
} filo_seq;

struct filo_value {
    uint8_t kind; /* filo_kind */
    union {
        double num;
        bool b;
        filo_str str;
        filo_seq seq; /* list and tuple */
        filo_func *fn;
    } u;
};

/* A builtin receives evaluated arguments and writes its result to out. On
   failure it sets the message with filo_fail() and returns FILO_ERR. */
typedef int (*filo_builtin)(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out);

/* One instruction of the bytecode machine, about to run: where it is in its
   unit's code section, the operand stack as it stands (bottom first), and
   how many calls deep the run is. The instruction itself is in the unit's
   bytes, which the host has; docs/bytecode.md says how to read it. */
typedef struct {
    uint32_t pc;
    const filo_value *stack;
    uint32_t depth;
    uint32_t calls;
} filo_trace;

/* Host hooks. Number formatting and parsing are hooks because doing them
   exactly (shortest round-trip text, Go's ParseFloat rules) needs either libc
   or a deliberate algorithm; filo_libc.c provides both on a libc host.
   should_stop, when set, is polled at every step and ends the run with an
   error when it returns true — the host's timeout or cancellation. trace,
   when set, sees every bytecode instruction before it runs: the machine
   shown at work, for someone learning how it runs. The IR has no trace. */
typedef struct {
    void *user;
    /* writes the shortest round-trip text of x; returns the length or 0 */
    size_t (*num_to_str)(void *user, double x, char *dst, size_t cap);
    /* parses the whole of s; returns false when it is not a number */
    bool (*str_to_num)(void *user, const uint8_t *s, size_t len, double *out);
    bool (*should_stop)(void *user);
    void (*trace)(void *user, const filo_trace *t);
} filo_host;

typedef struct {
    uint8_t *base;
    size_t cap;
    size_t used;
} filo_arena;

typedef struct {
    const char *name;
    filo_builtin fn;
} filo_builtin_entry;

#ifndef FILO_VM_ONLY
/* A compiled script: IR in the persistent arena, bound to the context that
   compiled it. */
typedef struct {
    const filo_instr *root;
} filo_prog;
#endif

typedef struct {
    uint32_t step_limit;
    uint32_t recursion_limit;
} filo_limits;

/* The whole runtime state. Allocate one per independent interpreter — as a
   static, on the stack, wherever — and hand it two memory blocks. Nothing
   here is global. */
struct filo_ctx {
    filo_host host;
    filo_arena persistent;
    filo_arena run;

    filo_builtin_entry builtins[FILO_BUILTINS_MAX];
    uint32_t nbuiltins;

    const char *symbols[FILO_SYMBOLS_MAX]; /* id -> name, in the persistent arena */
    uint32_t nsymbols;
    filo_value globals[FILO_SYMBOLS_MAX];
    bool defined[FILO_SYMBOLS_MAX];
    /* a run writes globals in place and marks them; the run end copies the
       marked ones out, or puts the saved ones back when the run failed */
    bool dirty[FILO_SYMBOLS_MAX];
    filo_value saved[FILO_SYMBOLS_MAX];
    bool saved_defined[FILO_SYMBOLS_MAX];

    filo_limits limits;
    bool sealed; /* no new globals: only what the host created may be used */

    /* per-run state */
    uint32_t steps;
    uint32_t recursion;
    uint32_t depth;
    /* How many times a value has been published beyond the scope that made
       it — a closure capturing its frame, or a write to a global or an
       enclosing frame. A scope compares this before and after its work to
       know whether it may hand its memory back; see "regions" in filo.c. */
    uint64_t escapes;
    void *frame; /* current local scope (internal type) */
    char error[FILO_ERROR_MAX];
    /* where in the source the last error happened, 0 when not known: see
       filo_error_at */
    uint32_t error_line;
    uint32_t error_col;
    uint8_t signal;      /* internal: exit/return unwinding */
    filo_value signaled; /* value carried by the signal */
};

/* ---- lifecycle ---- */

/* Initializes ctx with the host hooks (may be NULL: no formatting, no stop
   polling) and the two memory blocks. Registers the core builtins. */
int filo_init(filo_ctx *ctx, const filo_host *host, void *persistent, size_t persistent_cap,
              void *run, size_t run_cap);

/* Registers a builtin under name. Fails when the table is full or the name
   is already taken. */
int filo_register_builtin(filo_ctx *ctx, const char *name, filo_builtin fn);

#ifndef FILO_VM_ONLY
/* Parses and lowers src; the program lives in the persistent arena. */
int filo_compile(filo_ctx *ctx, const uint8_t *src, size_t len, filo_prog *out);

/* Runs prog. Resets the run arena first; copies surviving globals out at the
   end. result may be NULL. The default limits apply when limits is NULL. */
int filo_run(filo_ctx *ctx, const filo_prog *prog, const filo_limits *limits, filo_value *result);
#endif

/* The message of the last failed call. */
const char *filo_error(const filo_ctx *ctx);

/* Where in the program's source the last error happened: line and column
   from 1, the column in bytes, as parse errors count them. The IR knows it
   from the node that failed, a unit from its debug section. False when it
   is not known (a unit without debug, an error before any code ran). The
   message itself does not carry it, so it reads as the Go engine's does. */
bool filo_error_at(const filo_ctx *ctx, uint32_t *line, uint32_t *col);

/* ---- globals ---- */

/* Copies v into the persistent arena under name (creating the symbol). */
int filo_set_global(filo_ctx *ctx, const char *name, filo_value v);

/* Reads a global; false when it was never set. */
bool filo_get_global(const filo_ctx *ctx, const char *name, filo_value *out);

/* Closes the set of globals: from here on a script that names a global the
   host did not create fails to compile, instead of silently creating one.
   A host that runs several scripts against shared state seals once the state
   is in place, so a typo in a later script is caught at load. One way: to
   open it again, initialize a fresh context. */
void filo_seal_globals(filo_ctx *ctx);

/* ---- values ---- */

filo_value filo_num(double x);
filo_value filo_bool(bool b);
/* The bytes are referenced, not copied: they must outlive their use. Values
   the host keeps should go through filo_set_global, which copies. */
filo_value filo_string(const uint8_t *ptr, uint32_t len);
filo_value filo_cstring(const char *s);

/* Builds a list or tuple of n items in the run arena (for builtins). */
int filo_list(filo_ctx *ctx, const filo_value *items, uint32_t n, filo_value *out);
int filo_tuple(filo_ctx *ctx, const filo_value *items, uint32_t n, filo_value *out);

/* Deep, exact equality: NaN is not equal to NaN, kinds must match. */
bool filo_equal(const filo_value *a, const filo_value *b);

/* Renders v the way (string v) does: strings verbatim, everything else in
   source form. Needs the num_to_str hook for numbers. */
int filo_value_text(filo_ctx *ctx, const filo_value *v, char *dst, size_t cap, size_t *len);

/* ---- for builtins ---- */

/* Calls a func value with evaluated arguments (map, fold and filter do). */
int filo_call(filo_ctx *ctx, const filo_value *fn, const filo_value *args, uint32_t n,
              filo_value *out);

/* Records an error message (printf-free: message and an optional detail). */
int filo_fail(filo_ctx *ctx, const char *msg);
int filo_fail2(filo_ctx *ctx, const char *msg, const char *detail);

/* Argument coercion with the core's own messages ("expected number, got
   string"), so a pack or host builtin fails the way a core builtin does. */
int filo_arg_num(filo_ctx *ctx, const filo_value *v, double *out);
int filo_arg_str(filo_ctx *ctx, const filo_value *v, filo_str *out);
int filo_arg_list(filo_ctx *ctx, const filo_value *v, filo_seq *out);

/* Run-arena memory for a builtin's result; gone when the run ends. NULL
   (with the error set) when the arena is exhausted. */
void *filo_alloc(filo_ctx *ctx, size_t n);

/* Renders v in source form: strings quoted, lists as (list ...). */
int filo_value_repr(filo_ctx *ctx, const filo_value *v, char *dst, size_t cap, size_t *len);

const char *filo_kind_name(uint8_t kind);

/* pow with a fractional exponent needs libm; a host that has it installs
   it here (filo_libc.c does). NULL restores the core's integral-only pow. */
void filo_set_pow(double (*fn)(double, double));

/* ---- bytecode (docs/bytecode.md) ---- */

/* A loaded unit: programs compiled to a stack machine's instructions, run
   in place from wherever their bytes live. */
typedef struct filo_unit filo_unit;

#ifndef FILO_VM_ONLY
typedef struct {
    const char *name;      /* the entry point's name */
    const filo_prog *prog; /* compiled in the same context */
} filo_bc_entry;

/* Compiles the programs into one unit, written to dst. The size goes to
   *len; when cap is short the result is FILO_ERR and *len is the size
   needed. Uses the run arena as scratch. */
int filo_bc_build(filo_ctx *ctx, const filo_bc_entry *entries, uint32_t n, uint8_t *dst, size_t cap,
                  size_t *len);
#endif

/* A bundle: several units, each whole and named, in one file that travels
   as one (docs/bytecode.md, "Bundles"). */
typedef struct {
    const char *name;
    const uint8_t *data; /* a unit, as filo_bc_build wrote it */
    size_t len;
} filo_bundle_member;

#ifndef FILO_VM_ONLY
/* Copies a unit without its debug section (the positions errors report),
   for a machine that does not need them; the size goes to *len as in
   filo_bc_build. */
int filo_bc_strip(filo_ctx *ctx, const uint8_t *src, size_t len, uint8_t *dst, size_t cap,
                  size_t *out_len);

/* Writes the units into one bundle at dst, as filo_bc_build writes a unit:
   the size to *len, and FILO_ERR with the size needed when cap is short. */
int filo_bundle_build(filo_ctx *ctx, const filo_bundle_member *members, uint32_t n, uint8_t *dst,
                      size_t cap, size_t *len);
#endif

/* Checks a bundle whole and finds the member named name: *unit points at
   its bytes inside data, ready for filo_bc_load. */
int filo_bundle_find(filo_ctx *ctx, const uint8_t *data, size_t len, const char *name,
                     const uint8_t **unit, size_t *unit_len);

/* Checks a unit and resolves its imports and globals by name against ctx.
   The bytes are used in place and must outlive ctx: on a microcontroller
   they stay in flash. */
int filo_bc_load(filo_ctx *ctx, const uint8_t *data, size_t len, const filo_unit **out);

/* Whether a loaded unit has an entry point of that name. */
bool filo_bc_has(const filo_unit *unit, const char *entry);

/* Runs an entry point of a loaded unit as filo_run runs a program. */
int filo_bc_run(filo_ctx *ctx, const filo_unit *unit, const char *entry, const filo_limits *limits,
                filo_value *result);

#endif
