/* Filo runtime in C. See filo.h for the model and docs/ir.md for the
   contract this file implements. The file reads top to bottom: memory,
   values, errors, parser, lowering, evaluation, builtins, public API. */
#include "filo.h"

#include <string.h>

/* ---------------------------------------------------------------- memory */

static void *arena_alloc(filo_arena *a, size_t n) {
    size_t room = a->cap - a->used;
    /* compared before rounding up: n + 7 wraps near SIZE_MAX, and an
       impossible size would come out as zero bytes that fit */
    if (n > room) {
        return NULL;
    }
    size_t aligned = (n + 7U) & ~(size_t)7U;
    if (aligned > room) {
        return NULL;
    }
    void *p = a->base + a->used;
    a->used += aligned;
    return p;
}

static void arena_reset(filo_arena *a) {
    a->used = 0;
}

/* Every allocation that fails ends the current operation with this
   message; callers check for NULL and return FILO_ERR. */
/* Which arena is full and how big it is, so a board that refuses a program
   says what it would take: "out of memory: the run arena (64 KB) is full". */
static void fail_memory(filo_ctx *ctx, const char *which, size_t cap) {
    char msg[96];
    char num[24];
    size_t k = 0;
    const char *unit = " bytes";
    if (cap >= 1024 && cap % 1024 == 0) {
        cap /= 1024;
        unit = " KB";
    }
    size_t d = 0;
    do {
        num[d++] = (char)('0' + (cap % 10));
        cap /= 10;
    } while (cap > 0 && d < sizeof(num));
    const char *parts[] = {"out of memory: the ", which, " arena ("};
    for (size_t i = 0; i < 3; i++) {
        for (const char *c = parts[i]; *c != '\0'; c++) {
            msg[k++] = *c;
        }
    }
    while (d > 0) {
        msg[k++] = num[--d];
    }
    for (const char *c = unit; *c != '\0'; c++) {
        msg[k++] = *c;
    }
    for (const char *c = ") is full"; *c != '\0'; c++) {
        msg[k++] = *c;
    }
    msg[k] = '\0';
    (void)filo_fail(ctx, msg);
}

static void *ralloc(filo_ctx *ctx, size_t n) {
    void *p = arena_alloc(&ctx->run, n);
    if (p == NULL) {
        fail_memory(ctx, "run", ctx->run.cap);
    }
    return p;
}

static void *palloc(filo_ctx *ctx, size_t n) {
    void *p = arena_alloc(&ctx->persistent, n);
    if (p == NULL) {
        fail_memory(ctx, "persistent", ctx->persistent.cap);
    }
    return p;
}

/* --------------------------------------------------------------- errors */

static size_t cstr_copy(char *dst, size_t cap, const char *s) {
    size_t n = strlen(s);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
    return n;
}

int filo_fail(filo_ctx *ctx, const char *msg) {
    (void)cstr_copy(ctx->error, sizeof(ctx->error), msg);
    return FILO_ERR;
}

static void clear_error(filo_ctx *ctx) {
    ctx->error[0] = '\0';
    ctx->error_line = 0;
    ctx->error_col = 0;
}

int filo_fail2(filo_ctx *ctx, const char *msg, const char *detail) {
    size_t n = cstr_copy(ctx->error, sizeof(ctx->error), msg);
    if (detail != NULL) {
        (void)cstr_copy(ctx->error + n, sizeof(ctx->error) - n, detail);
    }
    return FILO_ERR;
}

#ifndef FILO_VM_ONLY
/* Prefixes the pending error with "in <ctx>: " — the context chain the Go
   engine builds by wrapping errors on the way out. */
static int fail_in(filo_ctx *ctx, const char *where) {
    char inner[FILO_ERROR_MAX];
    (void)cstr_copy(inner, sizeof(inner), ctx->error);
    size_t n = cstr_copy(ctx->error, sizeof(ctx->error), "in ");
    n += cstr_copy(ctx->error + n, sizeof(ctx->error) - n, where);
    n += cstr_copy(ctx->error + n, sizeof(ctx->error) - n, ": ");
    (void)cstr_copy(ctx->error + n, sizeof(ctx->error) - n, inner);
    return FILO_ERR;
}
#endif

/* Writes a decimal into a small buffer; used for argument indexes and
   arities in messages. */
static size_t u32_text(char *dst, size_t cap, uint32_t v) {
    char tmp[12];
    size_t n = 0;
    do {
        tmp[n] = (char)('0' + (v % 10U));
        n++;
        v /= 10U;
    } while (v > 0 && n < sizeof(tmp));
    if (n > cap - 1) {
        n = cap - 1;
    }
    for (size_t i = 0; i < n; i++) {
        dst[i] = tmp[n - 1 - i];
    }
    dst[n] = '\0';
    return n;
}

const char *filo_kind_name(uint8_t kind) {
    switch (kind) {
    case FILO_NUMBER:
        return "number";
    case FILO_BOOL:
        return "bool";
    case FILO_STRING:
        return "string";
    case FILO_LIST:
        return "list";
    case FILO_TUPLE:
        return "tuple";
    case FILO_FUNC:
        return "func";
    default:
        return "unknown";
    }
}

static int fail_expected(filo_ctx *ctx, const char *want, const filo_value *got) {
    char msg[FILO_ERROR_MAX];
    size_t n = cstr_copy(msg, sizeof(msg), "expected ");
    n += cstr_copy(msg + n, sizeof(msg) - n, want);
    n += cstr_copy(msg + n, sizeof(msg) - n, ", got ");
    (void)cstr_copy(msg + n, sizeof(msg) - n, filo_kind_name(got->kind));
    return filo_fail(ctx, msg);
}

/* --------------------------------------------------------------- values */

filo_value filo_num(double x) {
    filo_value v = {0};
    memset(&v, 0, sizeof(v));
    v.kind = FILO_NUMBER;
    v.u.num = x;
    return v;
}

filo_value filo_bool(bool b) {
    filo_value v = {0};
    memset(&v, 0, sizeof(v));
    v.kind = FILO_BOOL;
    v.u.b = b;
    return v;
}

filo_value filo_string(const uint8_t *ptr, uint32_t len) {
    filo_value v = {0};
    memset(&v, 0, sizeof(v));
    v.kind = FILO_STRING;
    v.u.str.ptr = ptr;
    v.u.str.len = len;
    return v;
}

filo_value filo_cstring(const char *s) {
    return filo_string((const uint8_t *)s, (uint32_t)strlen(s));
}

static filo_value empty_list(void) {
    filo_value v = {0};
    memset(&v, 0, sizeof(v));
    v.kind = FILO_LIST;
    return v;
}

static int make_seq(filo_ctx *ctx, uint8_t kind, const filo_value *items, uint32_t n,
                    filo_value *out) {
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->u.seq.len = n;
    if (n == 0) {
        return FILO_OK;
    }
    filo_value *dst = ralloc(ctx, sizeof(filo_value) * n);
    if (dst == NULL) {
        return FILO_ERR;
    }
    if (items != NULL) {
        memcpy(dst, items, sizeof(filo_value) * n);
    }
    out->u.seq.items = dst;
    return FILO_OK;
}

/* A range from zero holds no memory: items NULL with a length means the
   integers 0..len-1. It is a representation of this engine and nothing
   else, and two rules keep it that way: a range is never an element — it is
   materialised on its way into a list or a tuple — and a value crossing the
   public boundary is materialised first. The first is what makes the second
   enough: a shallow check reaches everything a host can see. */
static filo_value seq_at(const filo_seq *s, uint32_t i) {
    if (s->items == NULL) {
        return filo_num((double)i);
    }
    return s->items[i];
}

/* The vector itself, materialising the range for whoever needs a pointer
   rather than an element. NULL when there is no room. */
static filo_value *seq_items(filo_ctx *ctx, const filo_seq *s) {
    if (s->items != NULL || s->len == 0) {
        return (filo_value *)(void *)(uintptr_t)(const void *)s->items;
    }
    filo_value *v = ralloc(ctx, sizeof(filo_value) * s->len);
    if (v == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < s->len; i++) {
        v[i] = filo_num((double)i);
    }
    return v;
}

static int materialise(filo_ctx *ctx, filo_value *v) {
    if (v->kind != FILO_LIST && v->kind != FILO_TUPLE) {
        return FILO_OK;
    }
    if (v->u.seq.items != NULL || v->u.seq.len == 0) {
        return FILO_OK;
    }
    filo_value *items = seq_items(ctx, &v->u.seq);
    if (items == NULL) {
        return FILO_ERR;
    }
    v->u.seq.items = items;
    return FILO_OK;
}

/* ------------------------------------------------------------------ regions

   The run arena only ever bumps a pointer, so a scope can hand its memory
   back by moving that pointer down — but only when nothing that outlives
   the scope points into the part being given back. Two things publish a
   value beyond the scope that made it: a closure, which captures its frame
   by reference, and a write to a global or to an enclosing frame. Both
   bump ctx->escapes, and a scope whose work changed it keeps its memory.

   The result itself has to come down with the pointer. A scalar carries
   nothing. A string is a flat block, so it slides. A list or a tuple is
   copied compactly and then relocated, which is why the copy is made
   contiguous. A closure keeps its region: the frame it captured may form a
   cycle, and untangling that is the job of the copier that persists
   globals, not of this one.

   Everything here is an optimisation with no effect a script can observe:
   the value, the error, the globals and the step count are the same with
   it and without it. */

enum {
    /* A copy walks the value with the C stack. The evaluator bounds its own
       recursion; this bounds ours, and a structure deeper than this simply
       keeps its region instead of being recovered. */
    REGION_COPY_DEPTH_MAX = 64,
};

static bool region_copy(filo_ctx *ctx, const filo_value *src, filo_value *dst, uint32_t depth) {
    *dst = *src;
    if (src->kind == FILO_NUMBER || src->kind == FILO_BOOL) {
        return true;
    }
    if (depth > REGION_COPY_DEPTH_MAX) {
        return false;
    }
    if (src->kind == FILO_STRING) {
        if (src->u.str.len == 0) {
            return true;
        }
        uint8_t *bytes = ralloc(ctx, src->u.str.len);
        if (bytes == NULL) {
            return false;
        }
        memcpy(bytes, src->u.str.ptr, src->u.str.len);
        dst->u.str.ptr = bytes;
        return true;
    }
    if (src->kind != FILO_LIST && src->kind != FILO_TUPLE) {
        return false; /* a closure: see the note above */
    }
    uint32_t n = src->u.seq.len;
    if (n == 0 || src->u.seq.items == NULL) {
        return true; /* empty, or a range that was never materialised */
    }
    filo_value *items = ralloc(ctx, sizeof(filo_value) * n);
    if (items == NULL) {
        return false;
    }
    dst->u.seq.items = items;
    for (uint32_t i = 0; i < n; i++) {
        if (!region_copy(ctx, &src->u.seq.items[i], &items[i], depth + 1)) {
            return false;
        }
    }
    return true;
}

/* Once the copy slides down, every pointer that aimed inside it moves by
   the same delta. A pointer that aimed outside — a string literal in the
   persistent arena — stays where it is, which is what the range test is
   for. */
static void region_relocate(filo_value *v, const uint8_t *lo, const uint8_t *hi, ptrdiff_t delta) {
    if (v->kind == FILO_STRING) {
        const uint8_t *p = v->u.str.ptr;
        if (p >= lo && p < hi) {
            v->u.str.ptr = p + delta;
        }
        return;
    }
    if (v->kind != FILO_LIST && v->kind != FILO_TUPLE) {
        return;
    }
    filo_value *items = v->u.seq.items;
    if (items == NULL || v->u.seq.len == 0) {
        return;
    }
    const uint8_t *p = (const uint8_t *)items;
    if (p >= lo && p < hi) {
        items = (filo_value *)(void *)(p + delta);
        v->u.seq.items = items;
    }
    for (uint32_t i = 0; i < v->u.seq.len; i++) {
        region_relocate(&items[i], lo, hi, delta);
    }
}

/* The blocks always travel downward — from where the copy was made to the
   mark below it — and they may overlap, so the copy runs forward. memmove
   would do it, and it is the one libc function this file would have had to
   add to its six. */
static void region_move_down(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* Gives the arena back to mark, bringing out with it. */
static void region_release(filo_ctx *ctx, size_t mark, filo_value *out) {
    uint8_t *base = ctx->run.base;
    const uint8_t *hi = base + ctx->run.cap;
    if (out->kind == FILO_NUMBER || out->kind == FILO_BOOL) {
        ctx->run.used = mark;
        return;
    }
    if (out->kind == FILO_LIST || out->kind == FILO_TUPLE) {
        size_t top = ctx->run.used;
        filo_value copy = {0};
        if (!region_copy(ctx, out, &copy, 0)) {
            /* could not: the region stays as it was, and the run goes on as
               if nothing was tried — the failed allocation's message must not
               outlive it (this runs only where nothing has failed) */
            ctx->run.used = top;
            clear_error(ctx);
            return;
        }
        size_t size = ctx->run.used - top;
        const uint8_t *from = base + top;
        region_move_down(base + mark, from, size);
        region_relocate(&copy, from, from + size, (ptrdiff_t)mark - (ptrdiff_t)top);
        ctx->run.used = mark + size;
        *out = copy;
        return;
    }
    if (out->kind != FILO_STRING) {
        return; /* a closure keeps its region */
    }
    const uint8_t *p = out->u.str.ptr;
    if (p == NULL || p < base || p >= hi || p < base + mark) {
        /* a literal, or older than the mark: valid wherever it is */
        ctx->run.used = mark;
        return;
    }
    uint32_t len = out->u.str.len;
    region_move_down(base + mark, p, len);
    out->u.str.ptr = base + mark;
    ctx->run.used = mark + ((len + 7U) & ~(size_t)7U);
}

/* What list, tuple and every host builtin build from values they were
   handed: each one goes in as an element, so each one is materialised. */
static int make_elements(filo_ctx *ctx, uint8_t kind, const filo_value *items, uint32_t n,
                         filo_value *out) {
    if (make_seq(ctx, kind, items, n, out) != FILO_OK) {
        return FILO_ERR;
    }
    if (items == NULL) {
        return FILO_OK;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (materialise(ctx, &out->u.seq.items[i]) != FILO_OK) {
            return FILO_ERR;
        }
    }
    return FILO_OK;
}

int filo_list(filo_ctx *ctx, const filo_value *items, uint32_t n, filo_value *out) {
    return make_elements(ctx, FILO_LIST, items, n, out);
}

int filo_tuple(filo_ctx *ctx, const filo_value *items, uint32_t n, filo_value *out) {
    return make_elements(ctx, FILO_TUPLE, items, n, out);
}

bool filo_equal(const filo_value *a, const filo_value *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case FILO_NUMBER:
        return a->u.num == b->u.num; /* desvio: comparação exata de float é a semântica de = */
    case FILO_BOOL:
        return a->u.b == b->u.b;
    case FILO_STRING:
        if (a->u.str.len != b->u.str.len) {
            return false;
        }
        if (a->u.str.len == 0) {
            return true;
        }
        return memcmp(a->u.str.ptr, b->u.str.ptr, a->u.str.len) == 0;
    case FILO_LIST:
    case FILO_TUPLE:
        if (a->u.seq.len != b->u.seq.len) {
            return false;
        }
        for (uint32_t i = 0; i < a->u.seq.len; i++) {
            filo_value av = seq_at(&a->u.seq, i);
            filo_value bv = seq_at(&b->u.seq, i);
            if (!filo_equal(&av, &bv)) {
                return false;
            }
        }
        return true;
    case FILO_FUNC:
        return a->u.fn == b->u.fn;
    default:
        return false;
    }
}

/* --------------------------------------------------------------- parser */

#ifndef FILO_VM_ONLY

/* The parse tree is a temporary in the run arena: lowering reads it and
   only the IR survives, in the persistent arena. */
typedef enum {
    N_NUMBER,
    N_BOOL,
    N_STRING,
    N_SYMBOL,
    N_LIST,
} node_kind;

typedef struct node node;
struct node {
    uint8_t kind;
    uint32_t line; /* where it starts, counted as parse errors count */
    uint32_t col;
    double num;
    bool b;
    filo_str text; /* string bytes or symbol name */
    node **elems;  /* list */
    uint32_t nelems;
};

typedef struct {
    filo_ctx *ctx;
    const uint8_t *src;
    size_t len;
    size_t i;
    uint32_t depth;
    size_t start; /* where the node being read starts */
    size_t at;    /* how far line and col have been counted */
    uint32_t line;
    uint32_t col;
} parser;

static bool is_ws(uint8_t c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        return true;
    }
    return false;
}

static bool is_digit(uint8_t c) {
    if (c >= '0' && c <= '9') {
        return true;
    }
    return false;
}

static void skip_ws(parser *p) {
    while (p->i < p->len) {
        uint8_t c = p->src[p->i];
        if (is_ws(c)) {
            p->i++;
            continue;
        }
        if (c == ';') {
            while (p->i < p->len && p->src[p->i] != '\n') {
                p->i++;
            }
            continue;
        }
        return;
    }
}

/* Line and column of the byte at off. The parser only moves forward, and
   so does the count. */
static void parser_pos(parser *p, size_t off, uint32_t *line, uint32_t *col) {
    for (; p->at < off && p->at < p->len; p->at++) {
        if (p->src[p->at] == '\n') {
            p->line++;
            p->col = 1;
        } else {
            p->col++;
        }
    }
    *line = p->line;
    *col = p->col;
}

static node *new_node(parser *p, uint8_t kind) {
    node *n = ralloc(p->ctx, sizeof(node));
    if (n == NULL) {
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    parser_pos(p, p->start, &n->line, &n->col);
    return n;
}

/* A parse error at the byte at off: where a list or a string that never
   closes was opened, as the Go engine says it, or else where reading
   stopped. */
static int parse_fail_at(parser *p, size_t off, const char *what) {
    uint32_t line = 1;
    uint32_t col = 1;
    for (size_t k = 0; k < off && k < p->len; k++) {
        if (p->src[k] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    char prefix[64];
    size_t n = cstr_copy(prefix, sizeof(prefix), "parse error at line ");
    n += u32_text(prefix + n, sizeof(prefix) - n, line);
    n += cstr_copy(prefix + n, sizeof(prefix) - n, ", col ");
    n += u32_text(prefix + n, sizeof(prefix) - n, col);
    (void)cstr_copy(prefix + n, sizeof(prefix) - n, ": ");
    (void)filo_fail2(p->ctx, prefix, what);
    p->ctx->error_line = line;
    p->ctx->error_col = col;
    return FILO_ERR;
}

static int parse_fail(parser *p, const char *what) {
    return parse_fail_at(p, p->i, what);
}

static node *read_node(parser *p);

static node *read_list(parser *p) {
    size_t open = p->start; /* the '(' */
    p->depth++;
    if (p->depth > FILO_PARSE_DEPTH_MAX) {
        (void)parse_fail_at(p, open, "nesting too deep");
        return NULL;
    }
    node *list = new_node(p, N_LIST);
    if (list == NULL) {
        return NULL;
    }
    /* elements are collected into a growing run-arena block: lists are
       short, so doubling is fine and the waste dies with the run */
    uint32_t cap = 0;
    for (;;) {
        skip_ws(p);
        if (p->i >= p->len) {
            (void)parse_fail_at(p, open, "unterminated list");
            return NULL;
        }
        if (p->src[p->i] == ')') {
            p->i++;
            break;
        }
        node *e = read_node(p);
        if (e == NULL) {
            return NULL;
        }
        if (list->nelems == cap) {
            uint32_t ncap = cap == 0 ? 4 : cap * 2;
            node **grown = (node **)ralloc(p->ctx, sizeof(node *) * ncap);
            if (grown == NULL) {
                return NULL;
            }
            if (list->nelems > 0) {
                memcpy((void *)grown, (const void *)list->elems, sizeof(node *) * list->nelems);
            }
            list->elems = grown;
            cap = ncap;
        }
        list->elems[list->nelems] = e;
        list->nelems++;
    }
    p->depth--;
    return list;
}

/* The escapes a string may hold, the letter after the backslash. */
static bool escape_known(uint8_t c) {
    switch (c) {
    case '"':
    case 'n':
    case 't':
    case 'r':
    case '\\':
    case '0':
    case 'a':
    case 'b':
    case 'f':
    case 'v':
        return true;
    default:
        return false;
    }
}

static node *read_string(parser *p) {
    size_t open = p->i;
    p->i++; /* opening quote */
    /* decoded bytes go to a run-arena buffer no longer than the source span */
    size_t start = p->i;
    size_t scan = start;
    while (scan < p->len && p->src[scan] != '"') {
        if (p->src[scan] == '\\') {
            scan++;
            /* checked in reading order, as the Go engine checks it: an
               unknown escape fails before a missing closing quote does */
            if (scan < p->len && !escape_known(p->src[scan])) {
                (void)parse_fail_at(p, scan, "unsupported escape");
                return NULL;
            }
        }
        scan++;
    }
    if (scan > p->len) {
        (void)parse_fail_at(p, open, "unterminated escape sequence");
        return NULL;
    }
    if (scan >= p->len) {
        (void)parse_fail_at(p, open, "unterminated string literal");
        return NULL;
    }
    uint8_t *buf = ralloc(p->ctx, scan - start + 1);
    if (buf == NULL) {
        return NULL;
    }
    uint32_t n = 0;
    while (p->i < scan) {
        uint8_t c = p->src[p->i];
        p->i++;
        if (c != '\\') {
            buf[n] = c;
            n++;
            continue;
        }
        uint8_t e = p->src[p->i];
        p->i++;
        uint8_t decoded = 0;
        switch (e) {
        case '"':
            decoded = '"';
            break;
        case 'n':
            decoded = '\n';
            break;
        case 't':
            decoded = '\t';
            break;
        case 'r':
            decoded = '\r';
            break;
        case '\\':
            decoded = '\\';
            break;
        case '0':
            decoded = '\0';
            break;
        case 'a':
            decoded = '\a';
            break;
        case 'b':
            decoded = '\b';
            break;
        case 'f':
            decoded = '\f';
            break;
        case 'v':
            decoded = '\v';
            break;
        default:
            (void)parse_fail(p, "unsupported escape");
            return NULL;
        }
        buf[n] = decoded;
        n++;
    }
    p->i = scan + 1; /* closing quote */
    node *s = new_node(p, N_STRING);
    if (s == NULL) {
        return NULL;
    }
    s->text.ptr = buf;
    s->text.len = n;
    return s;
}

static bool at_boundary(const parser *p, size_t i) {
    if (i >= p->len) {
        return true;
    }
    uint8_t c = p->src[i];
    if (is_ws(c) || c == ')' || c == '(' || c == ';' || c == '"') {
        return true;
    }
    return false;
}

static node *read_bool(parser *p) {
    /* exactly #t or #f, then a token boundary: #true is an error */
    if (p->i + 1 < p->len && (p->src[p->i + 1] == 't' || p->src[p->i + 1] == 'f') &&
        at_boundary(p, p->i + 2)) {
        node *b = new_node(p, N_BOOL);
        if (b == NULL) {
            return NULL;
        }
        b->b = p->src[p->i + 1] == 't';
        p->i += 2;
        return b;
    }
    (void)parse_fail(p, "invalid boolean literal");
    return NULL;
}

/* Source grammar of a number literal: [+-]?(digits[.digits*]|.digits)
   ([eE][+-]?digits)?. Any other atom is a symbol, whatever the host's
   str_to_num would accept (inf, nan, 1_000). */
static bool number_shape(const uint8_t *s, size_t len) {
    size_t i = 0;
    if (i < len && (s[i] == '+' || s[i] == '-')) {
        i++;
    }
    size_t digits = 0;
    while (i < len && is_digit(s[i])) {
        i++;
        digits++;
    }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && is_digit(s[i])) {
            i++;
            digits++;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        size_t exp = 0;
        while (i < len && is_digit(s[i])) {
            i++;
            exp++;
        }
        if (exp == 0) {
            return false;
        }
    }
    return i == len;
}

static node *read_atom(parser *p) {
    size_t start = p->i;
    while (p->i < p->len) {
        uint8_t c = p->src[p->i];
        if (is_ws(c) || c == '(' || c == ')') {
            break;
        }
        p->i++;
    }
    if (p->i == start) {
        (void)parse_fail(p, "expected token");
        return NULL;
    }
    double x = 0;
    if (number_shape(p->src + start, p->i - start) && p->ctx->host.str_to_num != NULL &&
        p->ctx->host.str_to_num(p->ctx->host.user, p->src + start, p->i - start, &x)) {
        node *n = new_node(p, N_NUMBER);
        if (n == NULL) {
            return NULL;
        }
        n->num = x;
        return n;
    }
    node *s = new_node(p, N_SYMBOL);
    if (s == NULL) {
        return NULL;
    }
    s->text.ptr = p->src + start;
    s->text.len = (uint32_t)(p->i - start);
    return s;
}

static node *read_node(parser *p) {
    skip_ws(p);
    if (p->i >= p->len) {
        (void)parse_fail(p, "unexpected end of input");
        return NULL;
    }
    p->start = p->i;
    uint8_t c = p->src[p->i];
    if (c == '(') {
        p->i++;
        return read_list(p);
    }
    if (c == ')') {
        (void)parse_fail(p, "expected token");
        return NULL;
    }
    if (c == '"') {
        return read_string(p);
    }
    if (c == '#') {
        return read_bool(p);
    }
    return read_atom(p);
}

/* Parses the whole source. Several top-level expressions are wrapped in an
   implicit (let () ...), exactly as the Go parser does. */
static node *parse_all(filo_ctx *ctx, const uint8_t *src, size_t len) {
    /* a BOM is not part of the text: nor of the first token, nor of the
       columns, which count from after it as the Go engine counts them */
    if (len >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) {
        src += 3;
        len -= 3;
    }
    parser p = {ctx, src, len, 0, 0, 0, 0, 1, 1};
    node *first = NULL;
    node *wrapper = NULL;
    uint32_t cap = 0;
    for (;;) {
        skip_ws(&p);
        if (p.i >= p.len) {
            break;
        }
        node *n = read_node(&p);
        if (n == NULL) {
            return NULL;
        }
        if (first == NULL) {
            first = n;
            continue;
        }
        if (wrapper == NULL) {
            wrapper = new_node(&p, N_LIST);
            node *let = new_node(&p, N_SYMBOL);
            node *bindings = new_node(&p, N_LIST);
            if (wrapper == NULL || let == NULL || bindings == NULL) {
                return NULL;
            }
            let->text = filo_cstring("let").u.str;
            wrapper->line = first->line; /* the implicit let is where the program starts */
            wrapper->col = first->col;
            cap = 8;
            wrapper->elems = (node **)ralloc(ctx, sizeof(node *) * cap);
            if (wrapper->elems == NULL) {
                return NULL;
            }
            wrapper->elems[0] = let;
            wrapper->elems[1] = bindings;
            wrapper->elems[2] = first;
            wrapper->nelems = 3;
        }
        if (wrapper->nelems == cap) {
            uint32_t ncap = cap * 2;
            node **grown = (node **)ralloc(ctx, sizeof(node *) * ncap);
            if (grown == NULL) {
                return NULL;
            }
            memcpy((void *)grown, (const void *)wrapper->elems, sizeof(node *) * wrapper->nelems);
            wrapper->elems = grown;
            cap = ncap;
        }
        wrapper->elems[wrapper->nelems] = n;
        wrapper->nelems++;
    }
    if (first == NULL) {
        (void)parse_fail_at(&p, 0, "empty script"); /* as the Go engine places it */
        return NULL;
    }
    return wrapper != NULL ? wrapper : first;
}

/* ------------------------------------------------------------------- IR */

typedef enum {
    OP_CONST,
    OP_LOCAL,
    OP_GLOBAL,
    OP_DYNAMIC,
    OP_BUILTIN,
    OP_EMPTY,
    OP_INVALID,
    OP_IF,
    OP_COND,
    OP_DO,
    OP_AND,
    OP_OR,
    OP_LET,
    OP_LETV,
    OP_SET,
    OP_FN,
    OP_DEF,
    OP_TUPLE,
    OP_EXIT,
    OP_RETURN,
    OP_CALLB,
    OP_CALL,
} opcode;

typedef struct {
    bool invalid;
    bool is_else;
    const filo_instr *test;
    const filo_instr *const *body;
    uint32_t nbody;
} clause;

/* One instruction; which fields matter depends on op, exactly as the Go
   Instr (ir.go in the Go repository). Everything it points to is in the
   persistent arena. */
struct filo_instr {
    uint8_t op;
    uint32_t line; /* of the node it was lowered from; 0 when made up */
    uint32_t col;
    uint32_t a;
    uint32_t b;
    const char *name;
    const char *msg;
    filo_value val;
    const char *const *names;
    uint32_t nnames;
    const filo_instr *const *args;
    uint32_t nargs;
    const clause *clauses;
    uint32_t nclauses;
    filo_builtin fn;
};

#endif

typedef struct frame frame;
struct frame {
    filo_value *slots;
    uint32_t n;
    frame *parent;
};

typedef struct bc_fn bc_fn;

struct filo_func {
    const char *const *params;
    uint32_t nparams;
    const filo_instr *const *body;
    uint32_t nbody;
    frame *frame;
    const bc_fn *bc; /* set when the body is bytecode instead of IR */
    /* set when a unit's global resolved to a builtin at load: the program
       was compiled where the name was a function in Filo */
    const filo_builtin_entry *builtin;
};

/* --------------------------------------------------------------- lowering */

/* Copies a name into the persistent arena, NUL-terminated. */
static const char *pstr(filo_ctx *ctx, const uint8_t *ptr, uint32_t len) {
    char *s = palloc(ctx, (size_t)len + 1);
    if (s == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(s, ptr, len);
    }
    s[len] = '\0';
    return s;
}

static bool cname_eq(const char *name, const uint8_t *ptr, uint32_t len) {
    if (strlen(name) != len) {
        return false;
    }
    return memcmp(name, ptr, len) == 0;
}

/* Symbol ids are interned per context and never change, like the Go
   SymbolTable. */
static int32_t symbol_id(filo_ctx *ctx, const uint8_t *ptr, uint32_t len) {
    for (uint32_t i = 0; i < ctx->nsymbols; i++) {
        if (cname_eq(ctx->symbols[i], ptr, len)) {
            return (int32_t)i;
        }
    }
    if (ctx->sealed) {
        char msg[FILO_ERROR_MAX];
        size_t k = cstr_copy(msg, sizeof(msg), "undefined global: ");
        size_t take = len;
        if (take > sizeof(msg) - 1 - k) {
            take = sizeof(msg) - 1 - k;
        }
        memcpy(msg + k, ptr, take);
        msg[k + take] = '\0';
        (void)filo_fail(ctx, msg);
        return -1;
    }
    if (ctx->nsymbols >= FILO_SYMBOLS_MAX) {
        (void)filo_fail(ctx, "too many globals");
        return -1;
    }
    const char *name = pstr(ctx, ptr, len);
    if (name == NULL) {
        return -1;
    }
    ctx->symbols[ctx->nsymbols] = name;
    ctx->nsymbols++;
    return (int32_t)(ctx->nsymbols - 1);
}

static const filo_builtin_entry *builtin_by_name(const filo_ctx *ctx, const uint8_t *ptr,
                                                 uint32_t len) {
    for (uint32_t i = 0; i < ctx->nbuiltins; i++) {
        if (cname_eq(ctx->builtins[i].name, ptr, len)) {
            return &ctx->builtins[i];
        }
    }
    return NULL;
}

/* The builtin e as a function value: one per builtin, made the first time a
   script names it outside a call and kept with the builtins, so every use of
   it is the same function: (= floor floor) is true. */
static int builtin_value(filo_ctx *ctx, const filo_builtin_entry *e, filo_value *out) {
    filo_builtin_entry *m = &ctx->builtins[e - ctx->builtins];
    if (m->value == NULL) {
        filo_func *fn = palloc(ctx, sizeof(filo_func));
        if (fn == NULL) {
            return FILO_ERR;
        }
        memset(fn, 0, sizeof(*fn));
        fn->builtin = m;
        m->value = fn;
    }
    memset(out, 0, sizeof(*out));
    out->kind = FILO_FUNC;
    out->u.fn = m->value;
    return FILO_OK;
}

#ifndef FILO_VM_ONLY

static bool name_is(const filo_str *s, const char *lit) {
    size_t n = strlen(lit);
    if (s->len != n) {
        return false;
    }
    return memcmp(s->ptr, lit, n) == 0;
}

/* ---- folding: the Go engine's FoldConstants, on the parse tree ---- */

/* The builtins a fold may run: pure, atom in and atom out. The same list as
   the Go engine's pureFunctions; "list" is not in it, since no literal node
   could hold a list. */
static const char *const pure_builtins[] = {
    "+",        "-",      "*",      "/",    "%",    "pow",    "=",      "!=",
    "<",        ">",      "<=",     ">=",   "not",  "string", "number", "type-of",
    "is-empty", "is-nil", "length", "head", "tail", "nth",
};

static bool is_literal(const node *n, filo_value *out) {
    switch (n->kind) {
    case N_NUMBER:
        *out = filo_num(n->num);
        return true;
    case N_BOOL:
        *out = filo_bool(n->b);
        return true;
    case N_STRING:
        *out = filo_string(n->text.ptr, n->text.len);
        return true;
    default:
        return false;
    }
}

static node *fold_node(filo_ctx *ctx, const node *at, uint8_t kind) {
    node *n = ralloc(ctx, sizeof(node));
    if (n != NULL) {
        memset(n, 0, sizeof(*n));
        n->kind = kind;
        n->line = at->line;
        n->col = at->col;
    }
    return n;
}

/* (name literal...) evaluated now when name is pure and the call works; an
   error is left to happen when the program runs, as it would have. */
static node *fold_call(filo_ctx *ctx, node *list, const filo_str *name) {
    const filo_builtin_entry *bi = NULL;
    for (size_t i = 0; i < sizeof(pure_builtins) / sizeof(pure_builtins[0]); i++) {
        if (name_is(name, pure_builtins[i])) {
            bi = builtin_by_name(ctx, name->ptr, name->len);
        }
    }
    uint32_t n = list->nelems - 1;
    if (bi == NULL) {
        return list;
    }
    filo_value *args = NULL;
    if (n > 0) {
        args = ralloc(ctx, sizeof(filo_value) * n);
        if (args == NULL) {
            clear_error(ctx);
            return list;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (!is_literal(list->elems[i + 1], &args[i])) {
            return list;
        }
    }
    filo_value v = {0};
    if (bi->fn(ctx, args, n, &v) != FILO_OK) {
        clear_error(ctx);
        return list;
    }
    uint8_t kind = N_NUMBER;
    if (v.kind == FILO_BOOL) {
        kind = N_BOOL;
    } else if (v.kind == FILO_STRING) {
        kind = N_STRING;
    } else if (v.kind != FILO_NUMBER) {
        return list; /* a list or a tuple has no literal to become */
    }
    node *lit = fold_node(ctx, list, kind);
    if (lit == NULL) {
        return list;
    }
    if (v.kind == FILO_NUMBER) {
        lit->num = v.u.num;
    } else if (v.kind == FILO_BOOL) {
        lit->b = v.u.b;
    } else {
        lit->text = v.u.str;
    }
    return lit;
}

/* Children first, then the list itself: an if whose condition is a literal
   becomes the branch it takes, and a pure call on literals its value. The
   Go engine folds every program it compiles, IR included, so this one does
   too: a program keeps the same steps and the same compile-time errors on
   both. */
static node *fold(filo_ctx *ctx, node *n) {
    if (n->kind != N_LIST) {
        return n;
    }
    for (uint32_t i = 0; i < n->nelems; i++) {
        n->elems[i] = fold(ctx, n->elems[i]);
    }
    if (n->nelems == 0 || n->elems[0]->kind != N_SYMBOL) {
        return n;
    }
    const filo_str *head = &n->elems[0]->text;
    if (!name_is(head, "if")) {
        return fold_call(ctx, n, head);
    }
    if (n->nelems < 3 || n->nelems > 4 || n->elems[1]->kind != N_BOOL) {
        return n;
    }
    if (n->elems[1]->b) {
        return n->elems[2];
    }
    if (n->nelems == 4) {
        return n->elems[3];
    }
    /* (if #f x) is the empty list: the call (list), since a bare () would be
       an error where the if was not */
    node *call = fold_node(ctx, n, N_LIST);
    node *sym = fold_node(ctx, n, N_SYMBOL);
    node **elems = (node **)ralloc(ctx, sizeof(node *));
    if (call == NULL || sym == NULL || elems == NULL) {
        return n;
    }
    sym->text = filo_cstring("list").u.str;
    elems[0] = sym;
    call->elems = elems;
    call->nelems = 1;
    return call;
}

/* A lexical scope while lowering; lives in the run arena and dies with the
   compile. */
typedef struct scope scope;
struct scope {
    filo_str *vars;
    uint32_t n;
    uint32_t cap;
    scope *parent;
};

typedef struct {
    filo_ctx *ctx;
    scope *scope;
    uint32_t line; /* of the node being lowered: what new instructions carry */
    uint32_t col;
} lowerer;

static scope *scope_enter(lowerer *lw) {
    scope *s = ralloc(lw->ctx, sizeof(scope));
    if (s == NULL) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->parent = lw->scope;
    lw->scope = s;
    return s;
}

static void scope_leave(lowerer *lw) {
    lw->scope = lw->scope->parent;
}

static int scope_define(lowerer *lw, filo_str name) {
    scope *s = lw->scope;
    if (s->n == s->cap) {
        uint32_t ncap = s->cap == 0 ? 8 : s->cap * 2;
        filo_str *grown = ralloc(lw->ctx, sizeof(filo_str) * ncap);
        if (grown == NULL) {
            return FILO_ERR;
        }
        if (s->n > 0) {
            memcpy(grown, s->vars, sizeof(filo_str) * s->n);
        }
        s->vars = grown;
        s->cap = ncap;
    }
    s->vars[s->n] = name;
    s->n++;
    return FILO_OK;
}

static bool scope_resolve(const lowerer *lw, const filo_str *name, uint32_t *depth,
                          uint32_t *index) {
    uint32_t d = 0;
    for (const scope *s = lw->scope; s != NULL; s = s->parent) {
        for (uint32_t i = 0; i < s->n; i++) {
            if (s->vars[i].len == name->len && memcmp(s->vars[i].ptr, name->ptr, name->len) == 0) {
                *depth = d;
                *index = i;
                return true;
            }
        }
        d++;
    }
    return false;
}

static filo_instr *new_instr(lowerer *lw, uint8_t op) {
    filo_instr *in = palloc(lw->ctx, sizeof(filo_instr));
    if (in == NULL) {
        return NULL;
    }
    memset(in, 0, sizeof(*in));
    in->op = op;
    in->line = lw->line;
    in->col = lw->col;
    return in;
}

static filo_instr *invalid_instr(lowerer *lw, const char *where, const char *msg) {
    filo_instr *in = new_instr(lw, OP_INVALID);
    if (in == NULL) {
        return NULL;
    }
    in->name = where;
    in->msg = msg;
    return in;
}

static const filo_instr *lower(lowerer *lw, const node *n);

/* Lowers nodes[from..) into a persistent array. */
static int lower_all(lowerer *lw, node *const *nodes, uint32_t from, uint32_t to,
                     const filo_instr *const **out, uint32_t *nout) {
    uint32_t n = to > from ? to - from : 0;
    *nout = n;
    *out = NULL;
    if (n == 0) {
        return FILO_OK;
    }
    const filo_instr **arr = (const filo_instr **)palloc(lw->ctx, sizeof(filo_instr *) * n);
    if (arr == NULL) {
        return FILO_ERR;
    }
    for (uint32_t i = 0; i < n; i++) {
        arr[i] = lower(lw, nodes[from + i]);
        if (arr[i] == NULL) {
            return FILO_ERR;
        }
    }
    *out = arr;
    return FILO_OK;
}

static const filo_instr *lower_symbol(lowerer *lw, const filo_str *name) {
    uint32_t depth = 0;
    uint32_t index = 0;
    if (scope_resolve(lw, name, &depth, &index)) {
        filo_instr *in = new_instr(lw, OP_LOCAL);
        if (in == NULL) {
            return NULL;
        }
        in->a = depth;
        in->b = index;
        in->name = pstr(lw->ctx, name->ptr, name->len);
        return in->name != NULL ? in : NULL;
    }
    const filo_builtin_entry *bi = builtin_by_name(lw->ctx, name->ptr, name->len);
    if (bi != NULL) {
        filo_instr *in = new_instr(lw, OP_BUILTIN);
        if (in == NULL) {
            return NULL;
        }
        in->name = bi->name;
        in->fn = bi->fn;
        return in;
    }
    int32_t id = symbol_id(lw->ctx, name->ptr, name->len);
    if (id < 0) {
        return NULL;
    }
    filo_instr *in = new_instr(lw, OP_GLOBAL);
    if (in == NULL) {
        return NULL;
    }
    in->a = (uint32_t)id;
    in->name = lw->ctx->symbols[id];
    return in;
}

static const filo_instr *lower_simple(lowerer *lw, uint8_t op, const char *name, const node *list) {
    filo_instr *in = new_instr(lw, op);
    if (in == NULL) {
        return NULL;
    }
    in->name = name;
    if (lower_all(lw, list->elems, 1, list->nelems, &in->args, &in->nargs) != FILO_OK) {
        return NULL;
    }
    return in;
}

static const filo_instr *lower_cond(lowerer *lw, const node *list) {
    filo_instr *in = new_instr(lw, OP_COND);
    if (in == NULL) {
        return NULL;
    }
    uint32_t n = list->nelems - 1;
    clause *cls = NULL;
    if (n > 0) {
        cls = palloc(lw->ctx, sizeof(clause) * n);
        if (cls == NULL) {
            return NULL;
        }
        memset(cls, 0, sizeof(clause) * n);
    }
    for (uint32_t i = 0; i < n; i++) {
        const node *c = list->elems[i + 1];
        if (c->kind != N_LIST || c->nelems < 2) {
            cls[i].invalid = true;
            continue;
        }
        const node *head = c->elems[0];
        if (head->kind == N_SYMBOL && name_is(&head->text, "else")) {
            cls[i].is_else = true;
        } else {
            cls[i].test = lower(lw, head);
            if (cls[i].test == NULL) {
                return NULL;
            }
        }
        if (lower_all(lw, c->elems, 1, c->nelems, &cls[i].body, &cls[i].nbody) != FILO_OK) {
            return NULL;
        }
    }
    in->clauses = cls;
    in->nclauses = n;
    return in;
}

static const filo_instr *lower_let(lowerer *lw, const node *list) {
    uint32_t nargs = list->nelems - 1;
    if (nargs == 0) {
        return invalid_instr(lw, "let", "let expects bindings and body");
    }
    const node *bindings = list->elems[1];
    if (bindings->kind != N_LIST) {
        if (nargs < 2) {
            return invalid_instr(lw, "let", "let expects bindings and body");
        }
        return invalid_instr(lw, "let", "let expects binding list");
    }
    if (scope_enter(lw) == NULL) {
        return NULL;
    }
    uint32_t nb = bindings->nelems;
    uint32_t total = nb + (nargs - 1);
    const filo_instr **arr =
        (const filo_instr **)palloc(lw->ctx, sizeof(filo_instr *) * (total + 1));
    if (arr == NULL) {
        return NULL;
    }
    /* sequential like let*: each value sees the bindings before it */
    for (uint32_t i = 0; i < nb; i++) {
        const node *pair = bindings->elems[i];
        if (pair->kind != N_LIST || pair->nelems != 2) {
            (void)filo_fail(lw->ctx, "invalid let binding");
            return NULL;
        }
        if (pair->elems[0]->kind != N_SYMBOL) {
            (void)filo_fail(lw->ctx, "let binding name must be symbol");
            return NULL;
        }
        arr[i] = lower(lw, pair->elems[1]);
        if (arr[i] == NULL) {
            return NULL;
        }
        if (scope_define(lw, pair->elems[0]->text) != FILO_OK) {
            return NULL;
        }
    }
    for (uint32_t i = 2; i < list->nelems; i++) {
        arr[nb + i - 2] = lower(lw, list->elems[i]);
        if (arr[nb + i - 2] == NULL) {
            return NULL;
        }
    }
    scope_leave(lw);
    if (nargs < 2) {
        return invalid_instr(lw, "let", "let expects bindings and body");
    }
    filo_instr *in = new_instr(lw, OP_LET);
    if (in == NULL) {
        return NULL;
    }
    in->a = nb;
    in->args = arr;
    in->nargs = total;
    return in;
}

static const filo_instr *lower_letv(lowerer *lw, const node *list) {
    uint32_t nargs = list->nelems - 1;
    if (nargs < 2) {
        return invalid_instr(lw, "letv", "letv expects bindings and body");
    }
    const node *names = list->elems[1];
    if (names->kind != N_LIST) {
        return invalid_instr(lw, "letv", "letv expects name list");
    }
    /* the tuple is evaluated outside the new scope, so it is lowered there */
    const filo_instr *tuple = lower(lw, list->elems[2]);
    if (tuple == NULL) {
        return NULL;
    }
    if (scope_enter(lw) == NULL) {
        return NULL;
    }
    const char **nm = NULL;
    if (names->nelems > 0) {
        nm = (const char **)palloc(lw->ctx, sizeof(char *) * names->nelems);
        if (nm == NULL) {
            return NULL;
        }
    }
    for (uint32_t i = 0; i < names->nelems; i++) {
        if (names->elems[i]->kind != N_SYMBOL) {
            (void)filo_fail(lw->ctx, "letv names must be symbols");
            return NULL;
        }
        nm[i] = pstr(lw->ctx, names->elems[i]->text.ptr, names->elems[i]->text.len);
        if (nm[i] == NULL || scope_define(lw, names->elems[i]->text) != FILO_OK) {
            return NULL;
        }
    }
    uint32_t nbody = list->nelems - 3;
    const filo_instr **arr =
        (const filo_instr **)palloc(lw->ctx, sizeof(filo_instr *) * (nbody + 1));
    if (arr == NULL) {
        return NULL;
    }
    arr[0] = tuple;
    for (uint32_t i = 0; i < nbody; i++) {
        arr[i + 1] = lower(lw, list->elems[i + 3]);
        if (arr[i + 1] == NULL) {
            return NULL;
        }
    }
    scope_leave(lw);
    filo_instr *in = new_instr(lw, OP_LETV);
    if (in == NULL) {
        return NULL;
    }
    in->names = nm;
    in->nnames = names->nelems;
    in->args = arr;
    in->nargs = nbody + 1;
    return in;
}

static const filo_instr *lower_fn(lowerer *lw, const node *list) {
    uint32_t nargs = list->nelems - 1;
    if (nargs == 0) {
        return invalid_instr(lw, "fn", "fn expects parameters and body");
    }
    const node *params = list->elems[1];
    if (params->kind != N_LIST) {
        if (nargs < 2) {
            return invalid_instr(lw, "fn", "fn expects parameters and body");
        }
        return invalid_instr(lw, "fn", "fn expects parameter list");
    }
    if (scope_enter(lw) == NULL) {
        return NULL;
    }
    const char **nm = NULL;
    if (params->nelems > 0) {
        nm = (const char **)palloc(lw->ctx, sizeof(char *) * params->nelems);
        if (nm == NULL) {
            return NULL;
        }
    }
    for (uint32_t i = 0; i < params->nelems; i++) {
        if (params->elems[i]->kind != N_SYMBOL) {
            (void)filo_fail(lw->ctx, "fn params must be symbols");
            return NULL;
        }
        nm[i] = pstr(lw->ctx, params->elems[i]->text.ptr, params->elems[i]->text.len);
        if (nm[i] == NULL || scope_define(lw, params->elems[i]->text) != FILO_OK) {
            return NULL;
        }
    }
    const filo_instr *const *body = NULL;
    uint32_t nbody = 0;
    if (lower_all(lw, list->elems, 2, list->nelems, &body, &nbody) != FILO_OK) {
        return NULL;
    }
    scope_leave(lw);
    if (nbody == 0) {
        return invalid_instr(lw, "fn", "fn expects parameters and body");
    }
    filo_instr *in = new_instr(lw, OP_FN);
    if (in == NULL) {
        return NULL;
    }
    in->names = nm;
    in->nnames = params->nelems;
    in->args = body;
    in->nargs = nbody;
    return in;
}

static const filo_instr *lower_def(lowerer *lw, const node *list) {
    if (list->nelems != 3) {
        return invalid_instr(lw, "def", "def expects name and expression");
    }
    const filo_instr *value = lower(lw, list->elems[2]);
    if (value == NULL) {
        return NULL;
    }
    filo_instr *in = new_instr(lw, OP_DEF);
    if (in == NULL) {
        return NULL;
    }
    const filo_instr **arr = (const filo_instr **)palloc(lw->ctx, sizeof(filo_instr *));
    if (arr == NULL) {
        return NULL;
    }
    arr[0] = value;
    in->args = arr;
    in->nargs = 1;
    if (list->elems[1]->kind != N_SYMBOL) {
        in->msg = "def name must be symbol";
        return in;
    }
    in->name = pstr(lw->ctx, list->elems[1]->text.ptr, list->elems[1]->text.len);
    return in->name != NULL ? in : NULL;
}

static const filo_instr *lower_list(lowerer *lw, const node *list) {
    if (list->nelems == 0) {
        return new_instr(lw, OP_EMPTY);
    }
    const node *head = list->elems[0];
    if (head->kind == N_SYMBOL) {
        const filo_str *h = &head->text;
        if (name_is(h, "if")) {
            return lower_simple(lw, OP_IF, NULL, list);
        }
        if (name_is(h, "do")) {
            return lower_simple(lw, OP_DO, NULL, list);
        }
        if (name_is(h, "and")) {
            return lower_simple(lw, OP_AND, NULL, list);
        }
        if (name_is(h, "or")) {
            return lower_simple(lw, OP_OR, NULL, list);
        }
        if (name_is(h, "set")) {
            return lower_simple(lw, OP_SET, NULL, list);
        }
        if (name_is(h, "exit")) {
            return lower_simple(lw, OP_EXIT, "exit", list);
        }
        if (name_is(h, "return")) {
            return lower_simple(lw, OP_RETURN, "return", list);
        }
        if (name_is(h, "values")) {
            return lower_simple(lw, OP_TUPLE, "values", list);
        }
        if (name_is(h, "tuple")) {
            return lower_simple(lw, OP_TUPLE, "tuple", list);
        }
        if (name_is(h, "cond")) {
            return lower_cond(lw, list);
        }
        if (name_is(h, "let")) {
            return lower_let(lw, list);
        }
        if (name_is(h, "letv")) {
            return lower_letv(lw, list);
        }
        if (name_is(h, "fn")) {
            return lower_fn(lw, list);
        }
        if (name_is(h, "def")) {
            return lower_def(lw, list);
        }
    }
    const filo_instr *const *elems = NULL;
    uint32_t n = 0;
    if (lower_all(lw, list->elems, 0, list->nelems, &elems, &n) != FILO_OK) {
        return NULL;
    }
    if (elems[0]->op == OP_BUILTIN) {
        filo_instr *in = new_instr(lw, OP_CALLB);
        if (in == NULL) {
            return NULL;
        }
        in->name = elems[0]->name;
        in->fn = elems[0]->fn;
        in->args = elems + 1;
        in->nargs = n - 1;
        return in;
    }
    filo_instr *in = new_instr(lw, OP_CALL);
    if (in == NULL) {
        return NULL;
    }
    in->args = elems;
    in->nargs = n;
    return in;
}

static const filo_instr *lower_node(lowerer *lw, const node *n);

/* Lowers n with its position in effect, so everything made for it carries
   it, and puts the enclosing one back. */
static const filo_instr *lower(lowerer *lw, const node *n) {
    uint32_t line = lw->line;
    uint32_t col = lw->col;
    lw->line = n->line;
    lw->col = n->col;
    const filo_instr *in = lower_node(lw, n);
    if (in == NULL && lw->ctx->error_line == 0) {
        lw->ctx->error_line = n->line; /* a lowering error: the node it was about */
        lw->ctx->error_col = n->col;
    }
    lw->line = line;
    lw->col = col;
    return in;
}

static const filo_instr *lower_node(lowerer *lw, const node *n) {
    switch (n->kind) {
    case N_NUMBER: {
        filo_instr *in = new_instr(lw, OP_CONST);
        if (in == NULL) {
            return NULL;
        }
        in->val = filo_num(n->num);
        return in;
    }
    case N_BOOL: {
        filo_instr *in = new_instr(lw, OP_CONST);
        if (in == NULL) {
            return NULL;
        }
        in->val = filo_bool(n->b);
        return in;
    }
    case N_STRING: {
        filo_instr *in = new_instr(lw, OP_CONST);
        if (in == NULL) {
            return NULL;
        }
        /* the bytes were decoded into the run arena; constants must outlive it */
        const char *copy = pstr(lw->ctx, n->text.ptr, n->text.len);
        if (copy == NULL) {
            return NULL;
        }
        in->val = filo_string((const uint8_t *)copy, n->text.len);
        return in;
    }
    case N_SYMBOL:
        return lower_symbol(lw, &n->text);
    case N_LIST:
        return lower_list(lw, n);
    default:
        (void)filo_fail(lw->ctx, "unknown node");
        return NULL;
    }
}

#endif

/* ------------------------------------------------------------------ calls

   What a call needs whichever code runs it: the IR evaluator below, or the
   bytecode machine further down, which is all a device build keeps. */

enum {
    SIG_NONE = 0,
    SIG_EXIT,
    SIG_RETURN,
};

static int prefix_error(filo_ctx *ctx, const char *prefix) {
    char inner[FILO_ERROR_MAX];
    (void)cstr_copy(inner, sizeof(inner), ctx->error);
    size_t n = cstr_copy(ctx->error, sizeof(ctx->error), prefix);
    (void)cstr_copy(ctx->error + n, sizeof(ctx->error) - n, inner);
    return FILO_ERR;
}

static int as_bool(filo_ctx *ctx, const filo_value *v, bool *out) {
    if (v->kind != FILO_BOOL) {
        return fail_expected(ctx, "bool", v);
    }
    *out = v->u.b;
    return FILO_OK;
}

static int fail_arity(filo_ctx *ctx, uint32_t want, uint32_t got) {
    char msg[64];
    size_t n = cstr_copy(msg, sizeof(msg), "function expects ");
    n += u32_text(msg + n, sizeof(msg) - n, want);
    n += cstr_copy(msg + n, sizeof(msg) - n, " arguments, got ");
    (void)u32_text(msg + n, sizeof(msg) - n, got);
    return filo_fail(ctx, msg);
}

static int enter_call(filo_ctx *ctx) {
    ctx->recursion++;
    if (ctx->limits.recursion_limit > 0 && ctx->recursion > ctx->limits.recursion_limit) {
        ctx->recursion--;
        return filo_fail(ctx, "recursion limit exceeded");
    }
    return FILO_OK;
}

static int vm_run_func(filo_ctx *ctx, const filo_func *fn, const filo_value *slots, uint32_t n,
                       filo_value *out);
#ifndef FILO_VM_ONLY
static int ir_run_func(filo_ctx *ctx, const filo_func *fn, filo_value *slots, uint32_t n,
                       filo_value *out);
#endif

/* A builtin called by entry, its failure prefixed with its name as the
   interpreter prefixes it. */
static int call_entry(filo_ctx *ctx, const filo_builtin_entry *e, const filo_value *args,
                      uint32_t n, filo_value *out) {
    if (e->fn(ctx, args, n, out) != FILO_OK) {
        if (ctx->signal == SIG_NONE) {
            char prefix[FILO_ERROR_MAX];
            size_t k = cstr_copy(prefix, sizeof(prefix), "in builtin \"");
            k += cstr_copy(prefix + k, sizeof(prefix) - k, e->name);
            (void)cstr_copy(prefix + k, sizeof(prefix) - k, "\": ");
            (void)prefix_error(ctx, prefix);
        }
        return FILO_ERR;
    }
    return FILO_OK;
}

/* A builtin behind a global takes what its own arity check takes. */
static bool arity_ok(const filo_func *fn, uint32_t n) {
    if (fn->builtin != NULL) {
        return true;
    }
    return fn->nparams == n;
}

/* The recursion counter was incremented by the caller; the callee releases
   it. */
static int run_func(filo_ctx *ctx, const filo_func *fn, filo_value *slots, uint32_t n,
                    filo_value *out) {
    if (fn->builtin != NULL) {
        int rc = call_entry(ctx, fn->builtin, slots, n, out);
        ctx->recursion--;
        return rc;
    }
#ifndef FILO_VM_ONLY
    if (fn->bc == NULL) {
        return ir_run_func(ctx, fn, slots, n, out);
    }
#endif
    return vm_run_func(ctx, fn, slots, n, out);
}

int filo_call(filo_ctx *ctx, const filo_value *fnv, const filo_value *args, uint32_t n,
              filo_value *out) {
    if (fnv->kind != FILO_FUNC) {
        return fail_expected(ctx, "func", fnv);
    }
    const filo_func *fn = fnv->u.fn;
    if (!arity_ok(fn, n)) {
        return fail_arity(ctx, fn->nparams, n);
    }
    if (enter_call(ctx) != FILO_OK) {
        return FILO_ERR;
    }
    size_t mark = ctx->run.used;
    uint64_t escapes = ctx->escapes;
    filo_value *slots = NULL;
    if (n > 0) {
        slots = ralloc(ctx, sizeof(filo_value) * n);
        if (slots == NULL) {
            ctx->recursion--;
            return FILO_ERR;
        }
        memcpy(slots, args, sizeof(filo_value) * n);
    }
    int rc = run_func(ctx, fn, slots, n, out);
    if (rc == FILO_OK && ctx->escapes == escapes) {
        region_release(ctx, mark, out);
    }
    return rc;
}

/* ------------------------------------------------------------- evaluation */

#ifndef FILO_VM_ONLY
static int eval(filo_ctx *ctx, const filo_instr *in, filo_value *out);

/* Adds the "in <where>: " context unless a signal is unwinding: a signal is
   not an error and never gains context. */
static int wrap(filo_ctx *ctx, const char *where, int rc) {
    if (rc == FILO_OK || ctx->signal != SIG_NONE) {
        return rc;
    }
    return fail_in(ctx, where);
}

/* "argument <i>: " */
static int fail_argument(filo_ctx *ctx, uint32_t i) {
    char prefix[32];
    size_t n = cstr_copy(prefix, sizeof(prefix), "argument ");
    n += u32_text(prefix + n, sizeof(prefix) - n, i);
    (void)cstr_copy(prefix + n, sizeof(prefix) - n, ": ");
    return prefix_error(ctx, prefix);
}

static int eval_args(filo_ctx *ctx, const filo_instr *const *args, uint32_t n, filo_value **out) {
    *out = NULL;
    if (n == 0) {
        return FILO_OK;
    }
    filo_value *vals = ralloc(ctx, sizeof(filo_value) * n);
    if (vals == NULL) {
        return FILO_ERR;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (eval(ctx, args[i], &vals[i]) != FILO_OK) {
            if (ctx->signal == SIG_NONE) {
                (void)fail_argument(ctx, i);
            }
            return FILO_ERR;
        }
    }
    *out = vals;
    return FILO_OK;
}

static int eval_body(filo_ctx *ctx, const filo_instr *const *body, uint32_t n, filo_value *out) {
    if (n == 0) {
        return filo_fail(ctx, "empty body");
    }
    for (uint32_t i = 0; i < n; i++) {
        if (eval(ctx, body[i], out) != FILO_OK) {
            return FILO_ERR;
        }
    }
    return FILO_OK;
}

static int call_builtin(filo_ctx *ctx, const char *name, filo_builtin fn,
                        const filo_instr *const *args, uint32_t n, filo_value *out) {
    size_t mark = ctx->run.used;
    uint64_t escapes = ctx->escapes;
    filo_value *vals = NULL;
    if (eval_args(ctx, args, n, &vals) != FILO_OK) {
        if (ctx->signal == SIG_NONE) {
            char prefix[FILO_ERROR_MAX];
            size_t k = cstr_copy(prefix, sizeof(prefix), "while evaluating arguments for \"");
            k += cstr_copy(prefix + k, sizeof(prefix) - k, name);
            (void)cstr_copy(prefix + k, sizeof(prefix) - k, "\": ");
            (void)prefix_error(ctx, prefix);
        }
        return FILO_ERR;
    }
    if (fn(ctx, vals, n, out) != FILO_OK) {
        if (ctx->signal == SIG_NONE) {
            char prefix[FILO_ERROR_MAX];
            size_t k = cstr_copy(prefix, sizeof(prefix), "in builtin \"");
            k += cstr_copy(prefix + k, sizeof(prefix) - k, name);
            (void)cstr_copy(prefix + k, sizeof(prefix) - k, "\": ");
            (void)prefix_error(ctx, prefix);
        }
        return FILO_ERR;
    }
    if (ctx->escapes == escapes) {
        region_release(ctx, mark, out);
    }
    return FILO_OK;
}

/* Runs fn's body in a frame of slots whose parent is the captured frame; the
   recursion counter was incremented by the caller. A return signal becomes
   the value. */
static int ir_run_func(filo_ctx *ctx, const filo_func *fn, filo_value *slots, uint32_t n,
                       filo_value *out) {
    frame *f = ralloc(ctx, sizeof(frame));
    if (f == NULL) {
        ctx->recursion--;
        return FILO_ERR;
    }
    f->slots = slots;
    f->n = n;
    f->parent = fn->frame;
    frame *old = ctx->frame;
    ctx->frame = f;
    int rc = eval_body(ctx, fn->body, fn->nbody, out);
    ctx->frame = old;
    ctx->recursion--;
    if (rc != FILO_OK && ctx->signal == SIG_RETURN) {
        *out = ctx->signaled;
        ctx->signal = SIG_NONE;
        return FILO_OK;
    }
    return rc;
}

static int eval_call(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    const filo_instr *head = in->args[0];
    if (head->op == OP_DYNAMIC) {
        const filo_builtin_entry *bi =
            builtin_by_name(ctx, (const uint8_t *)head->name, (uint32_t)strlen(head->name));
        if (bi != NULL) {
            return call_builtin(ctx, bi->name, bi->fn, in->args + 1, in->nargs - 1, out);
        }
    }
    filo_value fnv = {0};
    if (eval(ctx, head, &fnv) != FILO_OK) {
        return wrap(ctx, "call", FILO_ERR);
    }
    if (fnv.kind != FILO_FUNC) {
        char msg[64];
        size_t k = cstr_copy(msg, sizeof(msg), "attempt to call non-function (got ");
        k += cstr_copy(msg + k, sizeof(msg) - k, filo_kind_name(fnv.kind));
        (void)cstr_copy(msg + k, sizeof(msg) - k, ")");
        return filo_fail(ctx, msg);
    }
    const filo_func *fn = fnv.u.fn;
    uint32_t n = in->nargs - 1;
    if (!arity_ok(fn, n)) {
        (void)fail_arity(ctx, fn->nparams, n);
        return wrap(ctx, "function call", FILO_ERR);
    }
    if (enter_call(ctx) != FILO_OK) {
        return wrap(ctx, "function call", FILO_ERR);
    }
    size_t mark = ctx->run.used;
    uint64_t escapes = ctx->escapes;
    filo_value *slots = NULL;
    if (n > 0) {
        slots = ralloc(ctx, sizeof(filo_value) * n);
        if (slots == NULL) {
            ctx->recursion--;
            return FILO_ERR;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (eval(ctx, in->args[i + 1], &slots[i]) != FILO_OK) {
            ctx->recursion--;
            if (ctx->signal == SIG_NONE) {
                (void)fail_argument(ctx, i);
                (void)prefix_error(ctx, "in call arguments: ");
            }
            return wrap(ctx, "function call", FILO_ERR);
        }
    }
    int rc = wrap(ctx, "function call", run_func(ctx, fn, slots, n, out));
    if (rc == FILO_OK && ctx->escapes == escapes) {
        region_release(ctx, mark, out);
    }
    return rc;
}

static int eval_if(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    if (in->nargs < 2 || in->nargs > 3) {
        return filo_fail(ctx, "if expects 2 or 3 arguments (condition then [else])");
    }
    filo_value c = {0};
    if (eval(ctx, in->args[0], &c) != FILO_OK) {
        return FILO_ERR;
    }
    bool b = false;
    if (as_bool(ctx, &c, &b) != FILO_OK) {
        return FILO_ERR;
    }
    if (b) {
        return eval(ctx, in->args[1], out);
    }
    if (in->nargs == 2) {
        *out = empty_list();
        return FILO_OK;
    }
    return eval(ctx, in->args[2], out);
}

static int eval_cond(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    for (uint32_t i = 0; i < in->nclauses; i++) {
        const clause *c = &in->clauses[i];
        if (c->invalid) {
            return filo_fail(ctx, "cond clause must be a list of a test and a body");
        }
        if (c->is_else) {
            return eval_body(ctx, c->body, c->nbody, out);
        }
        filo_value t = {0};
        if (eval(ctx, c->test, &t) != FILO_OK) {
            return FILO_ERR;
        }
        bool b = false;
        if (as_bool(ctx, &t, &b) != FILO_OK) {
            return FILO_ERR;
        }
        if (b) {
            return eval_body(ctx, c->body, c->nbody, out);
        }
    }
    *out = empty_list();
    return FILO_OK;
}

static int eval_do(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    if (in->nargs == 0) {
        return filo_fail(ctx, "do expects at least 1 expression");
    }
    return eval_body(ctx, in->args, in->nargs, out);
}

static int eval_logic(filo_ctx *ctx, const filo_instr *in, bool is_and, filo_value *out) {
    for (uint32_t i = 0; i < in->nargs; i++) {
        filo_value v = {0};
        if (eval(ctx, in->args[i], &v) != FILO_OK) {
            return FILO_ERR;
        }
        bool b = false;
        if (as_bool(ctx, &v, &b) != FILO_OK) {
            return FILO_ERR;
        }
        if (b != is_and) { /* and stops at the first false, or at the first true */
            *out = filo_bool(b);
            return FILO_OK;
        }
    }
    *out = filo_bool(is_and);
    return FILO_OK;
}

static int eval_let(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    uint32_t n = in->a;
    if (in->nargs <= n) {
        return filo_fail(ctx, "let expects bindings and body");
    }
    size_t mark = ctx->run.used;
    uint64_t escapes = ctx->escapes;
    frame *f = ralloc(ctx, sizeof(frame));
    if (f == NULL) {
        return FILO_ERR;
    }
    f->slots = NULL;
    f->n = n;
    if (n > 0) {
        f->slots = ralloc(ctx, sizeof(filo_value) * n);
        if (f->slots == NULL) {
            return FILO_ERR;
        }
        memset(f->slots, 0, sizeof(filo_value) * n);
    }
    f->parent = ctx->frame;
    frame *old = ctx->frame;
    ctx->frame = f; /* active while the values run: a binding may read earlier ones */
    for (uint32_t i = 0; i < n; i++) {
        if (eval(ctx, in->args[i], &f->slots[i]) != FILO_OK) {
            ctx->frame = old;
            return FILO_ERR;
        }
    }
    int rc = eval_body(ctx, in->args + n, in->nargs - n, out);
    ctx->frame = old;
    if (rc == FILO_OK && ctx->escapes == escapes) {
        region_release(ctx, mark, out);
    }
    return rc;
}

static int eval_letv(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    filo_value t = {0};
    if (eval(ctx, in->args[0], &t) != FILO_OK) { /* in the outer scope */
        return FILO_ERR;
    }
    if (t.kind != FILO_TUPLE) {
        return filo_fail(ctx, "letv expects tuple expression");
    }
    if (t.u.seq.len != in->nnames) {
        return filo_fail(ctx, "letv arity mismatch");
    }
    frame *f = ralloc(ctx, sizeof(frame));
    if (f == NULL) {
        return FILO_ERR;
    }
    f->n = in->nnames;
    f->slots = NULL;
    if (f->n > 0) {
        /* a copy: a later set must not write into the tuple */
        f->slots = ralloc(ctx, sizeof(filo_value) * f->n);
        if (f->slots == NULL) {
            return FILO_ERR;
        }
        for (uint32_t i = 0; i < f->n; i++) {
            f->slots[i] = seq_at(&t.u.seq, i);
        }
    }
    f->parent = ctx->frame;
    frame *old = ctx->frame;
    ctx->frame = f;
    int rc = eval_body(ctx, in->args + 1, in->nargs - 1, out);
    ctx->frame = old;
    return rc;
}

static int set_global_id(filo_ctx *ctx, uint32_t id, filo_value v);

static int eval_set(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    if (in->nargs != 2) {
        return filo_fail(ctx, "set expects name and expression");
    }
    filo_value v = {0};
    if (eval(ctx, in->args[1], &v) != FILO_OK) {
        return FILO_ERR;
    }
    const filo_instr *target = in->args[0];
    switch (target->op) {
    case OP_LOCAL: {
        frame *f = ctx->frame;
        for (uint32_t i = 0; i < target->a; i++) {
            f = f->parent;
        }
        if (target->a > 0) {
            ctx->escapes++; /* an enclosing scope now holds it */
        }
        f->slots[target->b] = v;
        *out = v;
        return FILO_OK;
    }
    case OP_GLOBAL:
        *out = v;
        return set_global_id(ctx, target->a, v);
    case OP_DYNAMIC: {
        int32_t id = symbol_id(ctx, (const uint8_t *)target->name, (uint32_t)strlen(target->name));
        if (id < 0) {
            return FILO_ERR;
        }
        *out = v;
        return set_global_id(ctx, (uint32_t)id, v);
    }
    default:
        return filo_fail(ctx, "set name must be symbol");
    }
}

static int eval_fn(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    if (in->nargs == 0) {
        return filo_fail(ctx, "fn expects parameters and body");
    }
    filo_func *fn = ralloc(ctx, sizeof(filo_func));
    if (fn == NULL) {
        return FILO_ERR;
    }
    fn->params = in->names;
    fn->nparams = in->nnames;
    fn->body = in->args;
    fn->nbody = in->nargs;
    fn->frame = ctx->frame;
    fn->bc = NULL;
    fn->builtin = NULL;
    memset(out, 0, sizeof(*out));
    out->kind = FILO_FUNC;
    out->u.fn = fn;
    return FILO_OK;
}

static int eval_def(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    if (in->msg != NULL) {
        return filo_fail(ctx, in->msg);
    }
    filo_value v = {0};
    if (eval(ctx, in->args[0], &v) != FILO_OK) {
        return FILO_ERR;
    }
    int32_t id = symbol_id(ctx, (const uint8_t *)in->name, (uint32_t)strlen(in->name));
    if (id < 0) {
        return FILO_ERR;
    }
    *out = v;
    return set_global_id(ctx, (uint32_t)id, v);
}

static int eval_signal(filo_ctx *ctx, const filo_instr *in, uint8_t sig) {
    if (in->nargs > 1) {
        return filo_fail2(ctx, in->name, " expects 0 or 1 argument");
    }
    filo_value v = empty_list();
    if (in->nargs == 1 && eval(ctx, in->args[0], &v) != FILO_OK) {
        return FILO_ERR;
    }
    ctx->signal = sig;
    ctx->signaled = v;
    return FILO_ERR;
}

/* The innermost node that failed is where the error happened; the ones
   around it only add their "in ..." on the way out. */
static int failed_at(filo_ctx *ctx, const filo_instr *in, int rc) {
    if (rc != FILO_OK && ctx->signal == SIG_NONE && ctx->error_line == 0) {
        ctx->error_line = in->line;
        ctx->error_col = in->col;
    }
    return rc;
}

static int eval(filo_ctx *ctx, const filo_instr *in, filo_value *out) {
    if (ctx->host.should_stop != NULL && ctx->host.should_stop(ctx->host.user)) {
        return failed_at(ctx, in, filo_fail(ctx, "execution cancelled"));
    }
    ctx->steps++;
    if (ctx->limits.step_limit > 0 && ctx->steps > ctx->limits.step_limit) {
        return failed_at(ctx, in, filo_fail(ctx, "step limit exceeded"));
    }
    if (ctx->depth >= FILO_EVAL_DEPTH_MAX) {
        return failed_at(ctx, in, filo_fail(ctx, "evaluation too deep"));
    }
    ctx->depth++;
    int rc = FILO_ERR;
    switch (in->op) {
    case OP_CONST:
        *out = in->val;
        rc = FILO_OK;
        break;
    case OP_LOCAL: {
        /* lowering emits a local only inside the frames it counted */
        frame *f = ctx->frame;
        for (uint32_t i = 0; i < in->a; i++) {
            f = f->parent; // NOLINT(clang-analyzer-core.NullDereference)
        }
        *out = f->slots[in->b]; // NOLINT(clang-analyzer-core.NullDereference)
        rc = FILO_OK;
        break;
    }
    case OP_GLOBAL:
        if (!ctx->defined[in->a]) {
            rc = filo_fail2(ctx, "undefined global: ", in->name);
            break;
        }
        *out = ctx->globals[in->a];
        rc = FILO_OK;
        break;
    case OP_DYNAMIC:
        rc = filo_fail2(ctx, "undefined symbol: ", in->name);
        for (uint32_t i = 0; i < ctx->nsymbols; i++) {
            if (strcmp(ctx->symbols[i], in->name) == 0 && ctx->defined[i]) {
                *out = ctx->globals[i];
                rc = FILO_OK;
                break;
            }
        }
        break;
    case OP_BUILTIN: {
        const filo_builtin_entry *e =
            builtin_by_name(ctx, (const uint8_t *)in->name, (uint32_t)strlen(in->name));
        rc = e != NULL ? builtin_value(ctx, e, out)
                       : filo_fail2(ctx, "undefined global: ", in->name);
        break;
    }
    case OP_EMPTY:
        rc = filo_fail(ctx, "empty list expression");
        break;
    case OP_INVALID:
        (void)filo_fail(ctx, in->msg);
        rc = fail_in(ctx, in->name);
        break;
    case OP_IF:
        rc = wrap(ctx, "if", eval_if(ctx, in, out));
        break;
    case OP_COND:
        rc = wrap(ctx, "cond", eval_cond(ctx, in, out));
        break;
    case OP_DO:
        rc = wrap(ctx, "do", eval_do(ctx, in, out));
        break;
    case OP_AND:
        rc = wrap(ctx, "and", eval_logic(ctx, in, true, out));
        break;
    case OP_OR:
        rc = wrap(ctx, "or", eval_logic(ctx, in, false, out));
        break;
    case OP_LET:
        rc = wrap(ctx, "let", eval_let(ctx, in, out));
        break;
    case OP_LETV:
        rc = wrap(ctx, "letv", eval_letv(ctx, in, out));
        break;
    case OP_SET:
        rc = wrap(ctx, "set", eval_set(ctx, in, out));
        break;
    case OP_FN:
        ctx->escapes++; /* it captures the frame in effect, by reference */
        rc = wrap(ctx, "fn", eval_fn(ctx, in, out));
        break;
    case OP_DEF:
        rc = wrap(ctx, "def", eval_def(ctx, in, out));
        break;
    case OP_TUPLE: {
        filo_value *vals = NULL;
        rc = eval_args(ctx, in->args, in->nargs, &vals);
        if (rc == FILO_OK) {
            rc = filo_tuple(ctx, vals, in->nargs, out);
        }
        rc = wrap(ctx, in->name, rc);
        break;
    }
    case OP_EXIT:
        rc = eval_signal(ctx, in, SIG_EXIT);
        break;
    case OP_RETURN:
        rc = eval_signal(ctx, in, SIG_RETURN);
        break;
    case OP_CALLB:
        rc = call_builtin(ctx, in->name, in->fn, in->args, in->nargs, out);
        break;
    case OP_CALL:
        rc = eval_call(ctx, in, out);
        break;
    default:
        rc = filo_fail(ctx, "unknown instruction");
        break;
    }
    ctx->depth--;
    return failed_at(ctx, in, rc);
}

#endif

/* ---------------------------------------------------------------- globals */

static bool in_persistent(const filo_ctx *ctx, const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    if (b < ctx->persistent.base) {
        return false;
    }
    return b < ctx->persistent.base + ctx->persistent.cap;
}

/* Frames reachable from a persisted closure may form cycles (a closure
   stored in the frame it captured); the memo breaks them and bounds the
   copy. */
enum { COPY_MEMO_MAX = 128 };

typedef struct {
    const frame *from[COPY_MEMO_MAX];
    frame *to[COPY_MEMO_MAX];
    uint32_t n;
} copy_memo;

static int copy_value(filo_ctx *ctx, const filo_value *src, filo_value *dst, copy_memo *memo);

static frame *copy_frame(filo_ctx *ctx, frame *old, copy_memo *memo) {
    if (old == NULL || in_persistent(ctx, old)) {
        return old;
    }
    for (uint32_t i = 0; i < memo->n; i++) {
        if (memo->from[i] == old) {
            return memo->to[i];
        }
    }
    if (memo->n >= COPY_MEMO_MAX) {
        (void)filo_fail(ctx, "cannot persist function: too many frames");
        return NULL;
    }
    frame *f = palloc(ctx, sizeof(frame));
    if (f == NULL) {
        return NULL;
    }
    memo->from[memo->n] = old;
    memo->to[memo->n] = f;
    memo->n++;
    f->n = old->n;
    f->slots = NULL;
    if (f->n > 0) {
        f->slots = palloc(ctx, sizeof(filo_value) * f->n);
        if (f->slots == NULL) {
            return NULL;
        }
        for (uint32_t i = 0; i < f->n; i++) {
            if (copy_value(ctx, &old->slots[i], &f->slots[i], memo) != FILO_OK) {
                return NULL;
            }
        }
    }
    f->parent = copy_frame(ctx, old->parent, memo);
    if (old->parent != NULL && f->parent == NULL) {
        return NULL;
    }
    return f;
}

/* Deep-copies a value into the persistent arena. Anything already there is
   shared, not copied: string constants come from the IR, and a persisted
   list only ever holds persisted values. */
static int copy_value(filo_ctx *ctx, const filo_value *src, filo_value *dst, copy_memo *memo) {
    *dst = *src;
    switch (src->kind) {
    case FILO_STRING: {
        if (src->u.str.len == 0 || in_persistent(ctx, src->u.str.ptr)) {
            return FILO_OK;
        }
        uint8_t *copy = palloc(ctx, src->u.str.len);
        if (copy == NULL) {
            return FILO_ERR;
        }
        memcpy(copy, src->u.str.ptr, src->u.str.len);
        dst->u.str.ptr = copy;
        return FILO_OK;
    }
    case FILO_LIST:
    case FILO_TUPLE: {
        if (src->u.seq.len == 0 || src->u.seq.items == NULL ||
            in_persistent(ctx, src->u.seq.items)) {
            return FILO_OK;
        }
        filo_value *items = palloc(ctx, sizeof(filo_value) * src->u.seq.len);
        if (items == NULL) {
            return FILO_ERR;
        }
        for (uint32_t i = 0; i < src->u.seq.len; i++) {
            if (copy_value(ctx, &src->u.seq.items[i], &items[i], memo) != FILO_OK) {
                return FILO_ERR;
            }
        }
        dst->u.seq.items = items;
        return FILO_OK;
    }
    case FILO_FUNC: {
        if (in_persistent(ctx, src->u.fn)) {
            return FILO_OK;
        }
        filo_func *fn = palloc(ctx, sizeof(filo_func));
        if (fn == NULL) {
            return FILO_ERR;
        }
        *fn = *src->u.fn;
        fn->frame = copy_frame(ctx, src->u.fn->frame, memo);
        if (src->u.fn->frame != NULL && fn->frame == NULL) {
            return FILO_ERR;
        }
        dst->u.fn = fn;
        return FILO_OK;
    }
    default:
        return FILO_OK;
    }
}

/* What a global held before the run first wrote it, for a failed run to
   put back: one for each global written, in the run arena, instead of a copy
   of every global in every context. */
typedef struct saved_global {
    struct saved_global *next;
    filo_value value;
    uint32_t id;
    bool defined;
} saved_global;

/* During a run a global holds run-arena data and is marked dirty; the run
   end copies dirty globals out (or restores them when the run failed). The
   escape comes first: no scope then gives back the memory the journal
   entry takes. */
static int set_global_id(filo_ctx *ctx, uint32_t id, filo_value v) {
    ctx->escapes++; /* the value outlives whatever scope is running */
    if (!ctx->dirty[id]) {
        saved_global *s = ralloc(ctx, sizeof(saved_global));
        if (s == NULL) {
            return FILO_ERR;
        }
        s->next = ctx->journal;
        s->value = ctx->globals[id];
        s->id = id;
        s->defined = ctx->defined[id];
        ctx->journal = s;
        ctx->dirty[id] = true;
    }
    ctx->globals[id] = v;
    ctx->defined[id] = true;
    return FILO_OK;
}

static void rollback_globals(filo_ctx *ctx) {
    for (const saved_global *s = ctx->journal; s != NULL; s = s->next) {
        ctx->dirty[s->id] = false;
        ctx->globals[s->id] = s->value;
        ctx->defined[s->id] = s->defined;
    }
    ctx->journal = NULL;
}

/* All or nothing: a global left pointing into the run arena would outlive
   it, so when the persistent arena fills halfway every written global goes
   back, and the persistent arena to where it was. */
static int commit_globals(filo_ctx *ctx) {
    copy_memo memo;
    memo.n = 0;
    size_t mark = ctx->persistent.used;
    for (const saved_global *s = ctx->journal; s != NULL; s = s->next) {
        filo_value persisted = {0};
        if (copy_value(ctx, &ctx->globals[s->id], &persisted, &memo) != FILO_OK) {
            rollback_globals(ctx);
            ctx->persistent.used = mark;
            return FILO_ERR;
        }
        ctx->globals[s->id] = persisted;
    }
    for (const saved_global *s = ctx->journal; s != NULL; s = s->next) {
        ctx->dirty[s->id] = false;
    }
    ctx->journal = NULL;
    return FILO_OK;
}

/* A paused run lives in the run arena, so whatever resets it ends the run
   first, as a failed one ends: its globals go back. */
static void cancel_paused(filo_ctx *ctx) {
    if (ctx->paused == NULL) {
        return;
    }
    ctx->paused = NULL;
    ctx->limits = ctx->paused_limits;
    rollback_globals(ctx);
}

int filo_set_global(filo_ctx *ctx, const char *name, filo_value v) {
    int32_t id = symbol_id(ctx, (const uint8_t *)name, (uint32_t)strlen(name));
    if (id < 0) {
        return FILO_ERR;
    }
    copy_memo memo;
    memo.n = 0;
    filo_value persisted = {0};
    if (copy_value(ctx, &v, &persisted, &memo) != FILO_OK) {
        return FILO_ERR;
    }
    ctx->globals[id] = persisted;
    ctx->defined[id] = true;
    return FILO_OK;
}

void filo_seal_globals(filo_ctx *ctx) {
    ctx->sealed = true;
}

bool filo_get_global(const filo_ctx *ctx, const char *name, filo_value *out) {
    size_t len = strlen(name);
    for (uint32_t i = 0; i < ctx->nsymbols; i++) {
        if (cname_eq(ctx->symbols[i], (const uint8_t *)name, (uint32_t)len)) {
            if (!ctx->defined[i]) {
                return false;
            }
            *out = ctx->globals[i];
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------- bytecode

   docs/bytecode.md is the contract. The compiler walks the IR and writes the
   instruction stream of a stack machine; the loader checks a unit and
   resolves its names against a context; the VM runs it in place. A call
   from bytecode to bytecode is a new activation record in the run arena,
   not a C call, so the machine could be paused between any two
   instructions. A builtin that calls a script back (map, fold) still goes
   through filo_call and the C stack. */

enum {
    BC_PUSH_K = 0,
    BC_PUSH_G,
    BC_STORE_G,
    BC_PUSH_L,
    BC_STORE_L,
    BC_PUSH_UP,
    BC_STORE_UP,
    BC_POP,
    BC_JMP,
    BC_CALL,
    BC_CALLB,
    BC_RET,
    BC_CLOSURE,
    BC_TUPLE,
    BC_UNPACK,
    BC_TRAP,
    BC_PUSH_B,
};

enum {
    BC_J_ALWAYS = 0,
    BC_J_FALSE,
    BC_J_AND,
    BC_J_OR,
    BC_J_CHECK,
};

enum {
    BC_SEC_IMPORTS = 1,
    BC_SEC_GLOBALS,
    BC_SEC_CONSTANTS,
    BC_SEC_FUNCTIONS,
    BC_SEC_CODE,
    BC_SEC_EXPORTS,
    BC_SEC_DEBUG,
    BC_SEC_EXTERNS,
};

enum {
    BC_K_NUMBER = 1,
    BC_K_STRING,
    BC_K_TRUE,
    BC_K_FALSE,
    BC_K_EMPTY,
};

enum {
    BC_HEADER = 20,
    BC_SECTION = 12,
    BC_SECTIONS = 8,
    BC_VERSION = 1,
    BC_KIND_UNIT = 1,
    BC_KIND_BUNDLE = 2,
    BC_MEMBER = 12,       /* a bundle's table entry */
    BC_MEMBERS_MAX = 256, /* units one bundle may hold */
    BC_ALIGN = 8,         /* where a member starts, from the start of the file */
    /* what one unit may hold: the compiler stops there, the loader refuses
       past it */
    BC_CONSTS_MAX = 4096,
    BC_FNS_MAX = 1024,
    BC_EXPORTS_MAX = 64,
    BC_SCOPES_MAX = 64,
    BC_SCOPE_POOL = 4096,
    BC_CODE_MAX = 1048576,
    BC_STACK_MAX = 4096,
    BC_NAME_MAX = 256,
};

struct bc_fn {
    const filo_unit *unit;
    uint32_t off; /* in the code section */
    uint32_t len;
    uint32_t nparams;
    uint32_t nslots; /* parameters first, then one slot per let binding */
    uint32_t maxstack;
};

/* What an import resolved to when the unit was loaded: a builtin, or the
   global that holds a function of that name (the program was compiled
   where the name was a builtin; this VM has it in Filo). */
typedef struct {
    const filo_builtin_entry *builtin;
    uint32_t sym; /* when builtin is NULL */
} bc_imported;

struct filo_unit {
    const uint8_t *code;
    uint32_t code_len;
    filo_value *consts;
    uint32_t nconsts;
    uint32_t *globals; /* unit index to symbol id in the loading context */
    uint32_t nglobals;
    bc_imported *imports;
    uint32_t nimports;
    bc_fn *fns;
    uint32_t nfns;
    filo_str *export_names;
    uint32_t *export_fns;
    uint32_t nexports;
    const uint8_t *debug; /* pc to line and column, read only when an error needs it */
    uint32_t debug_len;
};

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

static uint32_t get_u16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U);
}

static uint32_t get_u32(const uint8_t *p) {
    return get_u16(p) | (get_u16(p + 2) << 16U);
}

static int bc_bad(filo_ctx *ctx, const char *what) {
    return filo_fail2(ctx, "bytecode: malformed ", what);
}

/* The checksum of a unit: all of it, with its own field read as zero. */
static uint32_t bc_checksum(const uint8_t *data, size_t len) {
    static const uint8_t zero[4] = {0, 0, 0, 0};
    uint32_t h = fnv1a(2166136261U, data, 8);
    h = fnv1a(h, zero, 4);
    return fnv1a(h, data + 12, len - 12);
}

/* ---- the compiler ---- */

#ifndef FILO_VM_ONLY

typedef struct {
    bool fn; /* a function's own frame, or a let inside it */
    uint32_t base;
} bc_scope;

typedef struct {
    const filo_instr *root; /* an entry point's program */
    const filo_instr *const *body;
    uint32_t nbody;
    uint32_t nparams;
    uint32_t scope_at; /* the scopes around it when it was created, in the pool */
    uint32_t nscopes;
    uint32_t off;
    uint32_t len;
    uint32_t nslots;
    uint32_t maxstack;
} bc_fnrec;

typedef struct {
    filo_ctx *ctx;
    uint8_t *code;
    size_t len;
    size_t cap;
    filo_value consts[BC_CONSTS_MAX];
    uint32_t nconsts;
    uint32_t globals[FILO_SYMBOLS_MAX];
    uint8_t gused[FILO_SYMBOLS_MAX]; /* BC_G_READ, BC_G_WRITTEN */
    uint32_t nglobals;
    const filo_builtin_entry *imports[FILO_BUILTINS_MAX];
    uint32_t nimports;
    bc_fnrec fns[BC_FNS_MAX];
    uint32_t nfns;
    bc_scope pool[BC_SCOPE_POOL];
    uint32_t npool;
    bc_scope scopes[BC_SCOPES_MAX]; /* of the function being compiled */
    uint32_t nscopes;
    uint32_t nslots;
    uint32_t depth; /* values on its stack at this point */
    uint32_t maxdepth;
    bool failed; /* the message is in ctx */
    /* the debug section as it is written: the position in effect, and the
       last one recorded with the pc it starts at */
    uint8_t *dbg;
    size_t dlen;
    size_t dcap;
    uint32_t line;
    uint32_t col;
    uint32_t dbg_pc;
    uint32_t dbg_line;
    uint32_t dbg_col;
} bcc;

static void bc_fail(bcc *c, const char *msg) {
    if (!c->failed) {
        (void)filo_fail(c->ctx, msg);
        c->failed = true;
    }
}

static void bc_byte(bcc *c, uint32_t b) {
    if (c->len >= c->cap) {
        bc_fail(c, "bytecode: the code does not fit (a unit holds up to 1 MB, and the run arena "
                   "bounds it)");
        return;
    }
    c->code[c->len] = (uint8_t)b;
    c->len++;
}

static void dbg_uleb(bcc *c, uint32_t v) {
    for (;;) {
        if (c->dlen >= c->dcap) {
            bc_fail(c, "bytecode: the debug table does not fit the run arena");
            return;
        }
        uint32_t b = v & 0x7FU;
        v >>= 7U;
        c->dbg[c->dlen] = (uint8_t)(v != 0 ? b | 0x80U : b);
        c->dlen++;
        if (v == 0) {
            return;
        }
    }
}

/* An instruction starts here: when the position in effect is not the last
   one recorded, the debug table gets an entry — the pc since the last
   entry, the line as a signed difference (zigzag), the column as is. */
static void bc_mark(bcc *c) {
    if (c->line == 0 || (c->line == c->dbg_line && c->col == c->dbg_col)) {
        return;
    }
    uint32_t pc = (uint32_t)c->len;
    int64_t dl = (int64_t)c->line - (int64_t)c->dbg_line;
    dbg_uleb(c, pc - c->dbg_pc);
    dbg_uleb(c, (uint32_t)(dl < 0 ? ((uint64_t)(-dl) * 2U) - 1U : (uint64_t)dl * 2U));
    dbg_uleb(c, c->col);
    c->dbg_pc = pc;
    c->dbg_line = c->line;
    c->dbg_col = c->col;
}

static void bc_uleb(bcc *c, uint32_t v) {
    while (v >= 0x80U) {
        bc_byte(c, (v & 0x7FU) | 0x80U);
        v >>= 7U;
    }
    bc_byte(c, v);
}

/* The first byte, and the operand after it when three bits cannot hold it. */
static void bc_op(bcc *c, uint32_t op, uint32_t x) {
    bc_mark(c);
    if (x < 7U) {
        bc_byte(c, (op << 3U) | x);
        return;
    }
    bc_byte(c, (op << 3U) | 7U);
    bc_uleb(c, x);
}

static void bc_stack(bcc *c, int32_t delta) {
    if (delta < 0 && (uint32_t)(-delta) > c->depth) {
        bc_fail(c, "bytecode: stack underflow while compiling");
        return;
    }
    c->depth = (uint32_t)((int32_t)c->depth + delta);
    if (c->depth > c->maxdepth) {
        c->maxdepth = c->depth;
    }
}

/* A jump forward to a place not known yet: returns where its offset goes. */
static size_t bc_jump(bcc *c, uint32_t cond) {
    bc_mark(c);
    bc_byte(c, ((uint32_t)BC_JMP << 3U) | cond);
    size_t at = c->len;
    bc_byte(c, 0);
    bc_byte(c, 0);
    return at;
}

static void bc_land(bcc *c, size_t at) {
    if (c->failed) {
        return;
    }
    size_t dist = c->len - (at + 2U);
    if (dist > 32767U) {
        bc_fail(c, "bytecode: a jump longer than 32767 bytes");
        return;
    }
    c->code[at] = (uint8_t)dist;
    c->code[at + 1] = (uint8_t)(dist >> 8U);
}

static bool same_const(const filo_value *a, const filo_value *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case FILO_NUMBER: {
        uint64_t x = 0;
        uint64_t y = 0;
        memcpy(&x, &a->u.num, sizeof(x));
        memcpy(&y, &b->u.num, sizeof(y));
        return x == y;
    }
    case FILO_BOOL:
        return a->u.b == b->u.b;
    case FILO_STRING:
        if (a->u.str.len != b->u.str.len) {
            return false;
        }
        if (a->u.str.len == 0) {
            return true;
        }
        return memcmp(a->u.str.ptr, b->u.str.ptr, a->u.str.len) == 0;
    case FILO_LIST: /* only the empty list is ever a constant */
        if (a->u.seq.len != 0) {
            return false;
        }
        return b->u.seq.len == 0;
    default:
        return false;
    }
}

static uint32_t bc_const(bcc *c, filo_value v) {
    if (v.kind == FILO_NUMBER && v.u.num != v.u.num) {
        /* one NaN, the same bytes on every machine and from either compiler */
        uint64_t bits = 0x7FF8000000000000ULL;
        memcpy(&v.u.num, &bits, sizeof(bits));
    }
    bool ok = false;
    if (v.kind == FILO_NUMBER || v.kind == FILO_BOOL || v.kind == FILO_STRING) {
        ok = true;
    }
    if (v.kind == FILO_LIST && v.u.seq.len == 0) {
        ok = true;
    }
    if (!ok) {
        bc_fail(c, "bytecode: a constant that is not a number, string, bool or ()");
        return 0;
    }
    for (uint32_t i = 0; i < c->nconsts; i++) {
        if (same_const(&c->consts[i], &v)) {
            return i;
        }
    }
    if (c->nconsts >= BC_CONSTS_MAX) {
        bc_fail(c, "bytecode: more constants than one unit holds");
        return 0;
    }
    c->consts[c->nconsts] = v;
    c->nconsts++;
    return c->nconsts - 1;
}

static uint32_t bc_global(bcc *c, uint32_t id) {
    for (uint32_t i = 0; i < c->nglobals; i++) {
        if (c->globals[i] == id) {
            return i;
        }
    }
    if (c->nglobals >= FILO_SYMBOLS_MAX) {
        bc_fail(c, "bytecode: more globals than one unit holds");
        return 0;
    }
    c->globals[c->nglobals] = id;
    c->nglobals++;
    return c->nglobals - 1;
}

enum { BC_G_READ = 1, BC_G_WRITTEN = 2 };

/* The unit's index of global id, marked as read or written: a global the
   unit reads and never writes is an extern, which the loading VM provides. */
static uint32_t bc_global_use(bcc *c, uint32_t id, uint8_t use) {
    uint32_t g = bc_global(c, id);
    if (!c->failed) {
        c->gused[g] |= use;
    }
    return g;
}

static uint32_t bc_import(bcc *c, const char *name) {
    const filo_builtin_entry *e =
        builtin_by_name(c->ctx, (const uint8_t *)name, (uint32_t)strlen(name));
    if (e == NULL) {
        bc_fail(c, "bytecode: a builtin the context does not have");
        return 0;
    }
    for (uint32_t i = 0; i < c->nimports; i++) {
        if (c->imports[i] == e) {
            return i;
        }
    }
    if (c->nimports >= FILO_BUILTINS_MAX) {
        bc_fail(c, "bytecode: more imports than one unit holds");
        return 0;
    }
    c->imports[c->nimports] = e;
    c->nimports++;
    return c->nimports - 1;
}

/* Text for a TRAP, in the run arena with the rest of the compiler's
   scratch. */
static const char *bc_text(bcc *c, const char *a, const char *b, const char *d) {
    size_t na = strlen(a);
    size_t nb = strlen(b);
    size_t nd = strlen(d);
    char *s = ralloc(c->ctx, na + nb + nd + 1);
    if (s == NULL) {
        c->failed = true;
        return "";
    }
    memcpy(s, a, na);
    memcpy(s + na, b, nb);
    memcpy(s + na + nb, d, nd);
    s[na + nb + nd] = '\0';
    return s;
}

/* An error where the IR would raise it; in the structure around it the
   instruction stands for a value, though it never produces one. */
static void bc_trap(bcc *c, const char *msg) {
    uint32_t k = bc_const(c, filo_cstring(msg));
    bc_op(c, BC_TRAP, k);
    bc_stack(c, 1);
}

static uint32_t bc_queue(bcc *c, const filo_instr *root, const filo_instr *const *body,
                         uint32_t nbody, uint32_t nparams) {
    if (c->nfns >= BC_FNS_MAX || c->npool + c->nscopes > BC_SCOPE_POOL) {
        bc_fail(c, "bytecode: more functions than one unit holds");
        return 0;
    }
    bc_fnrec *f = &c->fns[c->nfns];
    memset(f, 0, sizeof(*f));
    f->root = root;
    f->body = body;
    f->nbody = nbody;
    f->nparams = nparams;
    f->scope_at = c->npool;
    f->nscopes = c->nscopes;
    if (c->nscopes > 0) {
        memcpy(&c->pool[c->npool], c->scopes, sizeof(bc_scope) * c->nscopes);
    }
    c->npool += c->nscopes;
    c->nfns++;
    return c->nfns - 1;
}

static bool bc_push_scope(bcc *c, bool fn, uint32_t base) {
    if (c->nscopes >= BC_SCOPES_MAX) {
        bc_fail(c, "bytecode: scopes nested deeper than one function holds");
        return false;
    }
    c->scopes[c->nscopes].fn = fn;
    c->scopes[c->nscopes].base = base;
    c->nscopes++;
    return true;
}

/* A local of the IR — depth counts every let and function scope — as a
   slot of the frame of the function that owns it. */
static void bc_local(bcc *c, uint32_t depth, uint32_t slot, bool store) {
    if (depth >= c->nscopes) {
        bc_fail(c, "bytecode: a local outside every scope");
        return;
    }
    uint32_t t = c->nscopes - 1 - depth;
    uint32_t crossed = 0;
    for (uint32_t i = c->nscopes - 1; i > t; i--) {
        if (c->scopes[i].fn) {
            crossed++;
        }
    }
    uint32_t flat = c->scopes[t].base + slot;
    if (crossed == 0) {
        bc_op(c, store ? BC_STORE_L : BC_PUSH_L, flat);
        return;
    }
    bc_op(c, store ? BC_STORE_UP : BC_PUSH_UP, crossed);
    bc_uleb(c, flat);
}

static void bc_expr(bcc *c, const filo_instr *in);

static void bc_seq(bcc *c, const filo_instr *const *body, uint32_t n) {
    if (n == 0) {
        bc_trap(c, "empty body");
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        bc_expr(c, body[i]);
        if (i + 1 < n) {
            bc_op(c, BC_POP, 1);
            bc_stack(c, -1);
        }
    }
}

static void bc_empty(bcc *c) {
    bc_op(c, BC_PUSH_K, bc_const(c, empty_list()));
    bc_stack(c, 1);
}

static void bc_if(bcc *c, const filo_instr *in) {
    if (in->nargs < 2 || in->nargs > 3) {
        bc_trap(c, "if expects 2 or 3 arguments (condition then [else])");
        return;
    }
    bc_expr(c, in->args[0]);
    size_t to_else = bc_jump(c, BC_J_FALSE);
    bc_stack(c, -1);
    uint32_t base = c->depth;
    bc_expr(c, in->args[1]);
    size_t to_end = bc_jump(c, BC_J_ALWAYS);
    c->depth = base;
    bc_land(c, to_else);
    if (in->nargs == 3) {
        bc_expr(c, in->args[2]);
    } else {
        bc_empty(c);
    }
    bc_land(c, to_end);
}

static void bc_cond(bcc *c, const filo_instr *in) {
    size_t *ends = NULL;
    if (in->nclauses > 0) {
        ends = ralloc(c->ctx, sizeof(size_t) * in->nclauses);
        if (ends == NULL) {
            c->failed = true;
            return;
        }
    }
    uint32_t nends = 0;
    uint32_t base = c->depth;
    bool closed = false; /* an else or a bad clause: nothing after it runs */
    for (uint32_t i = 0; i < in->nclauses && !closed; i++) {
        const clause *cl = &in->clauses[i];
        if (cl->invalid) {
            bc_trap(c, "cond clause must be a list of a test and a body");
            closed = true;
            continue;
        }
        if (cl->is_else) {
            bc_seq(c, cl->body, cl->nbody);
            closed = true;
            continue;
        }
        bc_expr(c, cl->test);
        size_t next = bc_jump(c, BC_J_FALSE);
        bc_stack(c, -1);
        bc_seq(c, cl->body, cl->nbody);
        ends[nends] = bc_jump(c, BC_J_ALWAYS);
        nends++;
        c->depth = base;
        bc_land(c, next);
    }
    if (!closed) {
        bc_empty(c);
    }
    for (uint32_t i = 0; i < nends; i++) {
        bc_land(c, ends[i]);
    }
}

static void bc_logic(bcc *c, const filo_instr *in, bool is_and) {
    if (in->nargs == 0) {
        bc_op(c, BC_PUSH_K, bc_const(c, filo_bool(is_and)));
        bc_stack(c, 1);
        return;
    }
    size_t *outs = ralloc(c->ctx, sizeof(size_t) * in->nargs);
    if (outs == NULL) {
        c->failed = true;
        return;
    }
    for (uint32_t i = 0; i < in->nargs; i++) {
        bc_expr(c, in->args[i]);
        if (i + 1 < in->nargs) {
            outs[i] = bc_jump(c, is_and ? BC_J_AND : BC_J_OR);
            bc_stack(c, -1); /* on through, the value was consumed */
        }
    }
    /* the last operand must be a bool too, and it is the result */
    size_t check = bc_jump(c, BC_J_CHECK);
    bc_land(c, check);
    for (uint32_t i = 0; i + 1 < in->nargs; i++) {
        bc_land(c, outs[i]);
    }
}

static void bc_let(bcc *c, const filo_instr *in) {
    uint32_t n = in->a;
    if (in->nargs <= n) {
        bc_trap(c, "let expects bindings and body");
        return;
    }
    uint32_t base = c->nslots;
    c->nslots += n; /* never reused: a closure keeps its let's slot */
    if (!bc_push_scope(c, false, base)) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        bc_expr(c, in->args[i]);
        bc_op(c, BC_STORE_L, base + i);
        bc_op(c, BC_POP, 1);
        bc_stack(c, -1);
    }
    bc_seq(c, in->args + n, in->nargs - n);
    c->nscopes--;
}

static void bc_letv(bcc *c, const filo_instr *in) {
    uint32_t n = in->nnames;
    bc_expr(c, in->args[0]); /* in the scope around the letv */
    bc_op(c, BC_UNPACK, n);
    bc_stack(c, (int32_t)n - 1);
    uint32_t base = c->nslots;
    c->nslots += n;
    if (!bc_push_scope(c, false, base)) {
        return;
    }
    for (uint32_t i = n; i > 0; i--) {
        bc_op(c, BC_STORE_L, base + i - 1);
        bc_op(c, BC_POP, 1);
        bc_stack(c, -1);
    }
    bc_seq(c, in->args + 1, in->nargs - 1);
    c->nscopes--;
}

static void bc_set(bcc *c, const filo_instr *in) {
    if (in->nargs != 2) {
        bc_trap(c, "set expects name and expression");
        return;
    }
    bc_expr(c, in->args[1]);
    const filo_instr *target = in->args[0];
    if (target->op == OP_LOCAL) {
        bc_local(c, target->a, target->b, true);
        return;
    }
    if (target->op == OP_GLOBAL) {
        bc_op(c, BC_STORE_G, bc_global_use(c, target->a, BC_G_WRITTEN));
        return;
    }
    bc_op(c, BC_POP, 1);
    bc_stack(c, -1);
    bc_trap(c, "set name must be symbol");
}

static void bc_def(bcc *c, const filo_instr *in) {
    if (in->msg != NULL) {
        bc_trap(c, in->msg);
        return;
    }
    bc_expr(c, in->args[0]);
    int32_t id = symbol_id(c->ctx, (const uint8_t *)in->name, (uint32_t)strlen(in->name));
    if (id < 0) {
        /* the IR fails here when the def runs, and only then */
        const char *why = bc_text(c, c->ctx->error, "", "");
        c->ctx->error[0] = '\0';
        bc_op(c, BC_POP, 1);
        bc_stack(c, -1);
        bc_trap(c, why);
        return;
    }
    bc_op(c, BC_STORE_G, bc_global_use(c, (uint32_t)id, BC_G_WRITTEN));
}

static void bc_signal(bcc *c, const filo_instr *in, uint32_t exit) {
    if (in->nargs > 1) {
        bc_trap(c, bc_text(c, in->name, " expects 0 or 1 argument", ""));
        return;
    }
    if (in->nargs == 1) {
        bc_expr(c, in->args[0]);
    } else {
        bc_empty(c);
    }
    bc_op(c, BC_RET, exit);
}

static void bc_call(bcc *c, const filo_instr *in) {
    for (uint32_t i = 0; i < in->nargs; i++) {
        bc_expr(c, in->args[i]);
    }
    uint32_t argc = in->nargs > 0 ? in->nargs - 1 : 0;
    bc_op(c, BC_CALL, argc);
    bc_stack(c, -(int32_t)argc);
}

static void bc_node(bcc *c, const filo_instr *in);

/* Compiles in with its position in effect, so the instructions made for it
   are marked with it, and puts the enclosing one back. */
static void bc_expr(bcc *c, const filo_instr *in) {
    uint32_t line = c->line;
    uint32_t col = c->col;
    if (in->line != 0) {
        c->line = in->line;
        c->col = in->col;
    }
    bc_node(c, in);
    c->line = line;
    c->col = col;
}

static void bc_node(bcc *c, const filo_instr *in) {
    if (c->failed) {
        return;
    }
    switch (in->op) {
    case OP_CONST:
        bc_op(c, BC_PUSH_K, bc_const(c, in->val));
        bc_stack(c, 1);
        return;
    case OP_LOCAL:
        bc_local(c, in->a, in->b, false);
        bc_stack(c, 1);
        return;
    case OP_GLOBAL:
        bc_op(c, BC_PUSH_G, bc_global_use(c, in->a, BC_G_READ));
        bc_stack(c, 1);
        return;
    case OP_DYNAMIC:
        bc_trap(c, bc_text(c, "undefined symbol: ", in->name, ""));
        return;
    case OP_BUILTIN:
        bc_op(c, BC_PUSH_B, bc_import(c, in->name));
        bc_stack(c, 1);
        return;
    case OP_EMPTY:
        bc_trap(c, "empty list expression");
        return;
    case OP_INVALID:
        bc_trap(c, bc_text(c, "in ", in->name, bc_text(c, ": ", in->msg, "")));
        return;
    case OP_IF:
        bc_if(c, in);
        return;
    case OP_COND:
        bc_cond(c, in);
        return;
    case OP_DO:
        if (in->nargs == 0) {
            bc_trap(c, "do expects at least 1 expression");
            return;
        }
        bc_seq(c, in->args, in->nargs);
        return;
    case OP_AND:
        bc_logic(c, in, true);
        return;
    case OP_OR:
        bc_logic(c, in, false);
        return;
    case OP_LET:
        bc_let(c, in);
        return;
    case OP_LETV:
        bc_letv(c, in);
        return;
    case OP_SET:
        bc_set(c, in);
        return;
    case OP_FN:
        if (in->nargs == 0) {
            bc_trap(c, "fn expects parameters and body");
            return;
        }
        bc_op(c, BC_CLOSURE, bc_queue(c, NULL, in->args, in->nargs, in->nnames));
        bc_stack(c, 1);
        return;
    case OP_DEF:
        bc_def(c, in);
        return;
    case OP_TUPLE:
        for (uint32_t i = 0; i < in->nargs; i++) {
            bc_expr(c, in->args[i]);
        }
        bc_op(c, BC_TUPLE, in->nargs);
        bc_stack(c, 1 - (int32_t)in->nargs);
        return;
    case OP_EXIT:
        bc_signal(c, in, 1);
        return;
    case OP_RETURN:
        bc_signal(c, in, 0);
        return;
    case OP_CALLB:
        for (uint32_t i = 0; i < in->nargs; i++) {
            bc_expr(c, in->args[i]);
        }
        bc_op(c, BC_CALLB, in->nargs);
        bc_uleb(c, bc_import(c, in->name));
        bc_stack(c, 1 - (int32_t)in->nargs);
        return;
    case OP_CALL:
        bc_call(c, in);
        return;
    default:
        bc_trap(c, "unknown instruction");
        return;
    }
}

static void bc_function(bcc *c, uint32_t idx) {
    bc_fnrec *f = &c->fns[idx];
    c->nscopes = f->nscopes;
    if (f->nscopes > 0) {
        memcpy(c->scopes, &c->pool[f->scope_at], sizeof(bc_scope) * f->nscopes);
    }
    if (!bc_push_scope(c, true, 0)) {
        return;
    }
    c->nslots = f->nparams;
    c->depth = 0;
    c->maxdepth = 0;
    f->off = (uint32_t)c->len;
    if (f->root != NULL) {
        bc_expr(c, f->root);
    } else {
        bc_seq(c, f->body, f->nbody);
    }
    bc_op(c, BC_RET, 0);
    f->len = (uint32_t)(c->len - f->off);
    f->nslots = c->nslots;
    f->maxstack = c->maxdepth;
}

/* The file, written or only measured: past cap nothing is written, but
   the length still counts, so the caller learns the size needed. */
typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
} bc_out;

static void out_byte(bc_out *o, uint32_t b) {
    if (o->len < o->cap) {
        o->p[o->len] = (uint8_t)b;
    }
    o->len++;
}

static void out_uleb(bc_out *o, uint32_t v) {
    while (v >= 0x80U) {
        out_byte(o, (v & 0x7FU) | 0x80U);
        v >>= 7U;
    }
    out_byte(o, v);
}

static void out_bytes(bc_out *o, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out_byte(o, p[i]);
    }
}

static void out_name(bc_out *o, const char *s) {
    size_t n = strlen(s);
    out_uleb(o, (uint32_t)n);
    out_bytes(o, (const uint8_t *)s, n);
}

static void put_u16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
}

static void put_u32(uint8_t *p, uint32_t v) {
    put_u16(p, v);
    put_u16(p + 2, v >> 16U);
}

static void bc_write_const(bc_out *o, const filo_value *v) {
    switch (v->kind) {
    case FILO_NUMBER: {
        uint64_t bits = 0;
        memcpy(&bits, &v->u.num, sizeof(bits));
        out_byte(o, BC_K_NUMBER);
        for (unsigned i = 0; i < 8U; i++) {
            out_byte(o, (uint32_t)(bits >> (8U * i)) & 0xFFU);
        }
        return;
    }
    case FILO_STRING:
        out_byte(o, BC_K_STRING);
        out_uleb(o, v->u.str.len);
        out_bytes(o, v->u.str.ptr, v->u.str.len);
        return;
    case FILO_BOOL:
        out_byte(o, v->u.b ? BC_K_TRUE : BC_K_FALSE);
        return;
    default:
        out_byte(o, BC_K_EMPTY);
        return;
    }
}

static void bc_write(bcc *c, const filo_bc_entry *entries, uint32_t n, bc_out *o) {
    const size_t table = BC_HEADER;
    const size_t hsize = BC_HEADER + ((size_t)BC_SECTIONS * BC_SECTION);
    uint32_t off[BC_SECTIONS];
    uint32_t len[BC_SECTIONS];
    for (size_t i = 0; i < hsize; i++) {
        out_byte(o, 0);
    }
    off[0] = (uint32_t)o->len;
    out_uleb(o, c->nimports);
    for (uint32_t i = 0; i < c->nimports; i++) {
        out_name(o, c->imports[i]->name);
    }
    off[1] = (uint32_t)o->len;
    out_uleb(o, c->nglobals);
    for (uint32_t i = 0; i < c->nglobals; i++) {
        out_name(o, c->ctx->symbols[c->globals[i]]);
    }
    off[2] = (uint32_t)o->len;
    out_uleb(o, c->nconsts);
    for (uint32_t i = 0; i < c->nconsts; i++) {
        bc_write_const(o, &c->consts[i]);
    }
    off[3] = (uint32_t)o->len;
    out_uleb(o, c->nfns);
    uint32_t widest_stack = 0;
    uint32_t widest_frame = 0;
    for (uint32_t i = 0; i < c->nfns; i++) {
        const bc_fnrec *f = &c->fns[i];
        out_uleb(o, f->off);
        out_uleb(o, f->len);
        out_uleb(o, f->nparams);
        out_uleb(o, f->nslots);
        out_uleb(o, f->maxstack);
        widest_stack = f->maxstack > widest_stack ? f->maxstack : widest_stack;
        widest_frame = f->nslots > widest_frame ? f->nslots : widest_frame;
    }
    off[4] = (uint32_t)o->len;
    out_bytes(o, c->code, c->len);
    off[5] = (uint32_t)o->len;
    out_uleb(o, n);
    for (uint32_t i = 0; i < n; i++) {
        out_name(o, entries[i].name);
        out_uleb(o, i);
    }
    off[6] = (uint32_t)o->len;
    out_bytes(o, c->dbg, c->dlen);
    off[7] = (uint32_t)o->len;
    uint32_t nexterns = 0;
    for (uint32_t i = 0; i < c->nglobals; i++) {
        nexterns += c->gused[i] == BC_G_READ ? 1U : 0U;
    }
    out_uleb(o, nexterns);
    for (uint32_t i = 0; i < c->nglobals; i++) {
        if (c->gused[i] == BC_G_READ) {
            out_uleb(o, i);
        }
    }
    for (int i = 0; i < BC_SECTIONS - 1; i++) {
        len[i] = off[i + 1] - off[i];
    }
    len[BC_SECTIONS - 1] = (uint32_t)o->len - off[BC_SECTIONS - 1];
    if (o->len > o->cap) {
        return;
    }
    uint8_t *h = o->p;
    h[0] = 0x7F;
    h[1] = 'F';
    h[2] = 'B';
    h[3] = 'C';
    h[4] = BC_KIND_UNIT;
    h[5] = BC_VERSION;
    put_u16(h + 6, (uint32_t)hsize);
    put_u16(h + 12, widest_stack > 0xFFFFU ? 0xFFFFU : widest_stack);
    put_u16(h + 14, widest_frame > 0xFFFFU ? 0xFFFFU : widest_frame);
    put_u16(h + 16, BC_SECTIONS);
    for (uint32_t i = 0; i < BC_SECTIONS; i++) {
        uint8_t *e = h + table + ((size_t)i * BC_SECTION);
        put_u16(e, i + 1);
        put_u32(e + 4, off[i]);
        put_u32(e + 8, len[i]);
    }
    put_u32(h + 8, bc_checksum(o->p, o->len));
}

int filo_bc_build(filo_ctx *ctx, const filo_bc_entry *entries, uint32_t n, uint8_t *dst, size_t cap,
                  size_t *len) {
    *len = 0;
    cancel_paused(ctx);
    arena_reset(&ctx->run);
    clear_error(ctx);
    if (n == 0 || n > BC_EXPORTS_MAX) {
        return filo_fail(ctx, "bytecode: a unit has 1 to 64 entry points");
    }
    bcc *c = ralloc(ctx, sizeof(bcc));
    if (c == NULL) {
        return FILO_ERR;
    }
    memset(c, 0, sizeof(*c));
    c->ctx = ctx;
    /* the code takes two thirds of what the run arena has left and the
       debug table one, keeping an eighth for the rest of the scratch (cond's
       jump lists, the texts of traps) */
    size_t room = ctx->run.cap - ctx->run.used;
    size_t avail = room - (room / 8U);
    c->cap = avail - (avail / 3U);
    c->dcap = avail / 3U;
    if (c->cap > BC_CODE_MAX) {
        c->cap = BC_CODE_MAX;
    }
    if (c->dcap > BC_CODE_MAX) {
        c->dcap = BC_CODE_MAX;
    }
    c->code = ralloc(ctx, c->cap);
    c->dbg = ralloc(ctx, c->dcap);
    if (c->code == NULL || c->dbg == NULL) {
        return FILO_ERR;
    }
    for (uint32_t i = 0; i < n; i++) {
        (void)bc_queue(c, entries[i].prog->root, NULL, 0, 0);
    }
    for (uint32_t i = 0; i < c->nfns && !c->failed; i++) {
        bc_function(c, i); /* compiling one may queue the closures it makes */
    }
    if (c->failed) {
        return FILO_ERR;
    }
    bc_out o = {dst, 0, cap};
    bc_write(c, entries, n, &o);
    *len = o.len;
    if (o.len > cap) {
        return filo_fail(ctx, "bytecode: the unit does not fit the buffer");
    }
    return FILO_OK;
}

/* The unit without its debug section, for a machine that has no use for
   positions: every other section byte for byte, the table one entry
   shorter and the checksum recomputed. A unit without one is copied. */
int filo_bc_strip(filo_ctx *ctx, const uint8_t *src, size_t len, uint8_t *dst, size_t cap,
                  size_t *out_len) {
    *out_len = 0;
    clear_error(ctx);
    if (len < BC_HEADER || src[0] != 0x7F || src[1] != 'F' || src[2] != 'B' || src[3] != 'C' ||
        src[4] != BC_KIND_UNIT) {
        return filo_fail(ctx, "bytecode: not a Filo unit");
    }
    uint32_t hsize = get_u16(src + 6);
    uint32_t nsec = get_u16(src + 16);
    if (hsize < BC_HEADER || hsize > len || nsec > 64 ||
        (size_t)BC_HEADER + ((size_t)nsec * BC_SECTION) > hsize) {
        return bc_bad(ctx, "header");
    }
    uint32_t keep = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *e = src + BC_HEADER + ((size_t)i * BC_SECTION);
        uint32_t off = get_u32(e + 4);
        uint32_t n = get_u32(e + 8);
        if (off < hsize || off > len || n > len - off) {
            return bc_bad(ctx, "section table");
        }
        if (get_u16(e) != BC_SEC_DEBUG) {
            keep++;
        }
    }
    size_t nh = BC_HEADER + ((size_t)keep * BC_SECTION);
    bc_out o = {dst, 0, cap};
    for (size_t i = 0; i < nh; i++) {
        out_byte(&o, i < BC_HEADER ? src[i] : 0U);
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *e = src + BC_HEADER + ((size_t)i * BC_SECTION);
        if (get_u16(e) == BC_SEC_DEBUG) {
            continue;
        }
        uint32_t off = get_u32(e + 4);
        uint32_t n = get_u32(e + 8);
        size_t at = o.len;
        out_bytes(&o, src + off, n);
        if (o.len <= cap) {
            uint8_t *d = dst + BC_HEADER + ((size_t)k * BC_SECTION);
            memcpy(d, e, 4);
            put_u32(d + 4, (uint32_t)at);
            put_u32(d + 8, n);
        }
        k++;
    }
    *out_len = o.len;
    if (o.len > cap) {
        return filo_fail(ctx, "bytecode: the unit does not fit the buffer");
    }
    put_u16(dst + 6, (uint32_t)nh);
    put_u16(dst + 16, keep);
    put_u32(dst + 8, bc_checksum(dst, o.len));
    return FILO_OK;
}

/* Several units, each whole and named, in one file (docs/bytecode.md,
   "Bundles"). A member is a unit as filo_bc_build wrote it, copied in
   unchanged, so it loads in place from the bundle's bytes. */
int filo_bundle_build(filo_ctx *ctx, const filo_bundle_member *members, uint32_t n, uint8_t *dst,
                      size_t cap, size_t *len) {
    *len = 0;
    clear_error(ctx);
    if (n == 0 || n > BC_MEMBERS_MAX) {
        return filo_fail(ctx, "bytecode: a bundle holds 1 to 256 units");
    }
    uint32_t name_at[BC_MEMBERS_MAX];
    uint32_t unit_at[BC_MEMBERS_MAX];
    uint32_t widest_stack = 0;
    uint32_t widest_frame = 0;
    const size_t hsize = BC_HEADER + ((size_t)n * BC_MEMBER);
    bc_out o = {dst, 0, cap};
    for (size_t i = 0; i < hsize; i++) {
        out_byte(&o, 0);
    }
    for (uint32_t i = 0; i < n; i++) {
        const filo_bundle_member *m = &members[i];
        size_t nl = strlen(m->name);
        if (nl == 0 || nl > BC_NAME_MAX) {
            return filo_fail(ctx, "bytecode: a bundle member needs a name of 1 to 256 bytes");
        }
        for (uint32_t k = 0; k < i; k++) {
            if (strcmp(members[k].name, m->name) == 0) {
                return filo_fail2(ctx, "bytecode: two bundle members named ", m->name);
            }
        }
        const uint8_t *u = m->data;
        if (m->len < BC_HEADER || u[0] != 0x7F || u[1] != 'F' || u[2] != 'B' || u[3] != 'C' ||
            u[4] != BC_KIND_UNIT) {
            return filo_fail2(ctx, "bytecode: a bundle member is not a unit: ", m->name);
        }
        widest_stack = get_u16(u + 12) > widest_stack ? get_u16(u + 12) : widest_stack;
        widest_frame = get_u16(u + 14) > widest_frame ? get_u16(u + 14) : widest_frame;
        name_at[i] = (uint32_t)o.len;
        out_name(&o, m->name);
    }
    for (uint32_t i = 0; i < n; i++) {
        while (o.len % BC_ALIGN != 0) {
            out_byte(&o, 0);
        }
        unit_at[i] = (uint32_t)o.len;
        out_bytes(&o, members[i].data, members[i].len);
    }
    *len = o.len;
    if (o.len > cap) {
        return filo_fail(ctx, "bytecode: the bundle does not fit the buffer");
    }
    dst[0] = 0x7F;
    dst[1] = 'F';
    dst[2] = 'B';
    dst[3] = 'C';
    dst[4] = BC_KIND_BUNDLE;
    dst[5] = BC_VERSION;
    put_u16(dst + 6, (uint32_t)hsize);
    put_u16(dst + 12, widest_stack);
    put_u16(dst + 14, widest_frame);
    put_u16(dst + 16, n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = dst + BC_HEADER + ((size_t)i * BC_MEMBER);
        put_u32(e, unit_at[i]);
        put_u32(e + 4, (uint32_t)members[i].len);
        put_u32(e + 8, name_at[i]);
    }
    put_u32(dst + 8, bc_checksum(dst, o.len));
    return FILO_OK;
}

#endif

/* ---- the loader ---- */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool bad;
} bc_rd;

static uint32_t rd_byte(bc_rd *r) {
    if (r->bad || r->p >= r->end) {
        r->bad = true;
        return 0;
    }
    uint32_t b = *r->p;
    r->p++;
    return b;
}

static uint32_t rd_uleb(bc_rd *r) {
    uint32_t v = 0;
    for (unsigned shift = 0; shift < 35U; shift += 7U) {
        uint32_t b = rd_byte(r);
        if (shift == 28U && b > 0x0FU) {
            r->bad = true; /* past 32 bits */
            return 0;
        }
        v |= (b & 0x7FU) << shift;
        if ((b & 0x80U) == 0) {
            return v;
        }
    }
    r->bad = true;
    return 0;
}

static const uint8_t *rd_bytes(bc_rd *r, uint32_t n) {
    if (r->bad || (size_t)(r->end - r->p) < n) {
        r->bad = true;
        return NULL;
    }
    const uint8_t *p = r->p;
    r->p += n;
    return p;
}

static filo_str rd_name(bc_rd *r) {
    filo_str s = {NULL, 0};
    uint32_t n = rd_uleb(r);
    if (n > BC_NAME_MAX) {
        r->bad = true;
        return s;
    }
    s.ptr = rd_bytes(r, n);
    s.len = n;
    /* names live as C strings in the context: one with a zero byte in it
       would never find itself, and could match another */
    for (uint32_t i = 0; s.ptr != NULL && i < n; i++) {
        if (s.ptr[i] == 0) {
            r->bad = true;
        }
    }
    return s;
}

static void *bc_palloc(filo_ctx *ctx, size_t each, uint32_t n) {
    if (n == 0) {
        return NULL;
    }
    return palloc(ctx, each * n);
}

/* The names a unit needs and this context does not have, gathered so the
   load can refuse with all of them at once: "missing (3): fg bg fill", as
   many as the message holds. */
typedef struct {
    char text[FILO_ERROR_MAX];
    size_t len;
    uint32_t n;
    bool cut;
} bc_missing;

enum { BC_MISSING_HEAD = 24 }; /* room for "missing (4294967295):" */

static void bc_miss(bc_missing *m, const uint8_t *name, uint32_t len) {
    m->n++;
    if (m->cut) {
        return;
    }
    if (m->len + 1 + len > sizeof(m->text) - BC_MISSING_HEAD - sizeof(" ...")) {
        m->cut = true;
        return;
    }
    m->text[m->len++] = ' ';
    memcpy(m->text + m->len, name, len);
    m->len += len;
    m->text[m->len] = '\0';
}

static int bc_refuse(filo_ctx *ctx, const bc_missing *m) {
    char msg[FILO_ERROR_MAX];
    char num[16];
    size_t d = 0;
    for (uint32_t v = m->n; v > 0 || d == 0; v /= 10U) {
        num[d++] = (char)('0' + (v % 10U));
    }
    size_t k = cstr_copy(msg, sizeof(msg), "missing (");
    while (d > 0) {
        msg[k++] = num[--d];
    }
    msg[k] = '\0';
    k += cstr_copy(msg + k, sizeof(msg) - k, "):");
    k += cstr_copy(msg + k, sizeof(msg) - k, m->text);
    if (m->cut) {
        (void)cstr_copy(msg + k, sizeof(msg) - k, " ...");
    }
    return filo_fail(ctx, msg);
}

/* A symbol already in the context, without making one. */
static int32_t symbol_find(const filo_ctx *ctx, const uint8_t *ptr, uint32_t len) {
    for (uint32_t i = 0; i < ctx->nsymbols; i++) {
        if (cname_eq(ctx->symbols[i], ptr, len)) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* Imports: each a builtin, or else a global holding a function. */
static int bc_load_imports(filo_ctx *ctx, bc_rd *r, filo_unit *u, bc_missing *m) {
    uint32_t n = rd_uleb(r);
    if (r->bad || n > FILO_BUILTINS_MAX) {
        return bc_bad(ctx, "imports");
    }
    u->imports = bc_palloc(ctx, sizeof(*u->imports), n);
    u->nimports = n;
    if (n > 0 && u->imports == NULL) {
        return FILO_ERR;
    }
    for (uint32_t i = 0; i < n; i++) {
        filo_str name = rd_name(r);
        if (r->bad) {
            return bc_bad(ctx, "imports");
        }
        u->imports[i].builtin = builtin_by_name(ctx, name.ptr, name.len);
        u->imports[i].sym = 0;
        if (u->imports[i].builtin != NULL) {
            continue;
        }
        int32_t id = symbol_find(ctx, name.ptr, name.len);
        if (id >= 0 && ctx->defined[id] && ctx->globals[id].kind == FILO_FUNC) {
            u->imports[i].sym = (uint32_t)id;
            continue;
        }
        bc_miss(m, name.ptr, name.len);
    }
    return FILO_OK;
}

static int bc_load_globals(filo_ctx *ctx, bc_rd *r, filo_unit *u, bc_missing *m) {
    uint32_t n = rd_uleb(r);
    if (r->bad || n > FILO_SYMBOLS_MAX) {
        return bc_bad(ctx, "globals");
    }
    u->globals = bc_palloc(ctx, sizeof(*u->globals), n);
    u->nglobals = n;
    if (n > 0 && u->globals == NULL) {
        return FILO_ERR;
    }
    for (uint32_t i = 0; i < n; i++) {
        filo_str name = rd_name(r);
        if (r->bad) {
            return bc_bad(ctx, "globals");
        }
        int32_t id = symbol_id(ctx, name.ptr, name.len);
        if (id < 0 && ctx->sealed) {
            clear_error(ctx);
            bc_miss(m, name.ptr, name.len); /* a sealed context makes no new names */
            u->globals[i] = UINT32_MAX;
            continue;
        }
        if (id < 0) {
            return FILO_ERR;
        }
        u->globals[i] = (uint32_t)id;
    }
    return FILO_OK;
}

/* Externs: the globals the unit reads and never writes, which the loading
   context holds — a value the host set, a function in Filo, or a builtin of
   that name, bound to the global here. Lazy, one it does not hold is left
   to fail when read, as the interpreter fails. */
static int bc_load_externs(filo_ctx *ctx, bc_rd *r, filo_unit *u, bool lazy, bc_missing *m) {
    uint32_t n = rd_uleb(r);
    if (r->bad || n > u->nglobals) {
        return bc_bad(ctx, "externs");
    }
    uint32_t last = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t g = rd_uleb(r);
        if (r->bad || g >= u->nglobals || (i > 0 && g <= last)) {
            return bc_bad(ctx, "externs");
        }
        last = g;
        uint32_t id = u->globals[g];
        if (id == UINT32_MAX || ctx->defined[id]) {
            continue; /* held, or already missing */
        }
        const char *name = ctx->symbols[id];
        const filo_builtin_entry *b =
            builtin_by_name(ctx, (const uint8_t *)name, (uint32_t)strlen(name));
        if (b != NULL) {
            if (builtin_value(ctx, b, &ctx->globals[id]) != FILO_OK) {
                return FILO_ERR;
            }
            ctx->defined[id] = true;
            continue;
        }
        if (!lazy) {
            bc_miss(m, (const uint8_t *)name, (uint32_t)strlen(name));
        }
    }
    return FILO_OK;
}

static int bc_load_consts(filo_ctx *ctx, bc_rd *r, filo_unit *u) {
    uint32_t n = rd_uleb(r);
    if (r->bad || n > BC_CONSTS_MAX) {
        return bc_bad(ctx, "constants");
    }
    u->consts = bc_palloc(ctx, sizeof(filo_value), n);
    if (n > 0 && u->consts == NULL) {
        return FILO_ERR;
    }
    u->nconsts = n;
    for (uint32_t i = 0; i < n; i++) {
        filo_value *v = &u->consts[i];
        memset(v, 0, sizeof(*v));
        uint32_t tag = rd_byte(r);
        switch (tag) {
        case BC_K_NUMBER: {
            const uint8_t *b = rd_bytes(r, 8);
            if (b == NULL) {
                return bc_bad(ctx, "constants");
            }
            uint64_t bits = 0;
            for (unsigned k = 8; k > 0; k--) {
                bits = (bits << 8U) | b[k - 1];
            }
            double x = 0;
            memcpy(&x, &bits, sizeof(x));
            *v = filo_num(x);
            break;
        }
        case BC_K_STRING: {
            uint32_t len = rd_uleb(r);
            const uint8_t *s = rd_bytes(r, len);
            if (s == NULL) {
                return bc_bad(ctx, "constants");
            }
            *v = filo_string(s, len);
            break;
        }
        case BC_K_TRUE:
            *v = filo_bool(true);
            break;
        case BC_K_FALSE:
            *v = filo_bool(false);
            break;
        case BC_K_EMPTY:
            *v = empty_list();
            break;
        default:
            return bc_bad(ctx, "constants");
        }
    }
    return FILO_OK;
}

static int bc_load_fns(filo_ctx *ctx, bc_rd *r, filo_unit *u) {
    uint32_t n = rd_uleb(r);
    if (r->bad || n > BC_FNS_MAX) {
        return bc_bad(ctx, "functions");
    }
    u->fns = bc_palloc(ctx, sizeof(bc_fn), n);
    if (n > 0 && u->fns == NULL) {
        return FILO_ERR;
    }
    u->nfns = n;
    for (uint32_t i = 0; i < n; i++) {
        bc_fn *f = &u->fns[i];
        f->unit = u;
        f->off = rd_uleb(r);
        f->len = rd_uleb(r);
        f->nparams = rd_uleb(r);
        f->nslots = rd_uleb(r);
        f->maxstack = rd_uleb(r);
        if (r->bad || f->off > u->code_len || f->len > u->code_len - f->off ||
            f->nparams > f->nslots || f->nslots > 0xFFFFU || f->maxstack > BC_STACK_MAX) {
            return bc_bad(ctx, "functions");
        }
    }
    return FILO_OK;
}

static int bc_load_exports(filo_ctx *ctx, bc_rd *r, filo_unit *u) {
    uint32_t n = rd_uleb(r);
    if (r->bad || n == 0 || n > BC_EXPORTS_MAX) {
        return bc_bad(ctx, "exports");
    }
    u->export_names = bc_palloc(ctx, sizeof(filo_str), n);
    u->export_fns = bc_palloc(ctx, sizeof(uint32_t), n);
    if (u->export_names == NULL || u->export_fns == NULL) {
        return FILO_ERR;
    }
    u->nexports = n;
    for (uint32_t i = 0; i < n; i++) {
        u->export_names[i] = rd_name(r);
        u->export_fns[i] = rd_uleb(r);
        if (r->bad || u->export_fns[i] >= u->nfns) {
            return bc_bad(ctx, "exports");
        }
    }
    return FILO_OK;
}

/* Checks a whole bundle — its checksum, its table, every member inside the
   file on its alignment — and picks the member named name, or when name is
   NULL the one at index. */
static int bundle_scan(filo_ctx *ctx, const uint8_t *data, size_t len, const char *name,
                       uint32_t index, uint32_t *count, filo_str *name_out, const uint8_t **unit,
                       size_t *unit_len) {
    clear_error(ctx);
    *unit = NULL;
    *unit_len = 0;
    if (len < BC_HEADER || data[0] != 0x7F || data[1] != 'F' || data[2] != 'B' || data[3] != 'C' ||
        data[4] != BC_KIND_BUNDLE) {
        return filo_fail(ctx, "bytecode: not a Filo bundle");
    }
    if (data[5] != BC_VERSION) {
        return filo_fail(ctx, "bytecode: a kind or version this runtime does not read");
    }
    uint32_t hsize = get_u16(data + 6);
    uint32_t n = get_u16(data + 16);
    if (n == 0 || n > BC_MEMBERS_MAX || hsize > len ||
        (size_t)BC_HEADER + ((size_t)n * BC_MEMBER) > hsize) {
        return bc_bad(ctx, "bundle header");
    }
    if (bc_checksum(data, len) != get_u32(data + 8)) {
        return filo_fail(ctx, "bytecode: the checksum does not match (a damaged bundle)");
    }
    size_t want = name != NULL ? strlen(name) : 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = data + BC_HEADER + ((size_t)i * BC_MEMBER);
        uint32_t off = get_u32(e);
        uint32_t mlen = get_u32(e + 4);
        uint32_t name_off = get_u32(e + 8);
        if (off < hsize || off > len || mlen > len - off || off % BC_ALIGN != 0 ||
            name_off < hsize || name_off >= len) {
            return bc_bad(ctx, "bundle table");
        }
        bc_rd r = {data + name_off, data + len, false};
        filo_str s = rd_name(&r);
        if (r.bad || s.len == 0) {
            return bc_bad(ctx, "bundle table");
        }
        bool pick = i == index;
        if (name != NULL) {
            pick = false;
            if (s.len == want) {
                pick = memcmp(s.ptr, name, want) == 0;
            }
        }
        if (*unit == NULL && pick) {
            *unit = data + off;
            *unit_len = mlen;
            if (name_out != NULL) {
                *name_out = s;
            }
        }
    }
    if (count != NULL) {
        *count = n;
    }
    if (*unit == NULL) {
        return name != NULL ? filo_fail2(ctx, "bytecode: no bundle member named ", name)
                            : filo_fail(ctx, "bytecode: no bundle member at that index");
    }
    return FILO_OK;
}

int filo_bundle_find(filo_ctx *ctx, const uint8_t *data, size_t len, const char *name,
                     const uint8_t **unit, size_t *unit_len) {
    return bundle_scan(ctx, data, len, name, 0, NULL, NULL, unit, unit_len);
}

int filo_bundle_at(filo_ctx *ctx, const uint8_t *data, size_t len, uint32_t index, uint32_t *count,
                   filo_str *name, const uint8_t **unit, size_t *unit_len) {
    return bundle_scan(ctx, data, len, NULL, index, count, name, unit, unit_len);
}

/* The header and the section table of a unit, checked: where each section
   is, or what is wrong with them. */
typedef enum {
    BC_TABLE_OK,
    BC_TABLE_NOT_UNIT,
    BC_TABLE_VERSION,
    BC_TABLE_HEADER,
    BC_TABLE_CHECKSUM,
    BC_TABLE_SECTIONS,
} bc_table_result;

static bc_table_result bc_table(const uint8_t *data, size_t len, bc_rd *sec, bool *have) {
    if (len < BC_HEADER || data[0] != 0x7F || data[1] != 'F' || data[2] != 'B' || data[3] != 'C') {
        return BC_TABLE_NOT_UNIT;
    }
    if (data[4] != BC_KIND_UNIT || data[5] != BC_VERSION) {
        return BC_TABLE_VERSION;
    }
    uint32_t hsize = get_u16(data + 6);
    uint32_t nsec = get_u16(data + 16);
    if (hsize < BC_HEADER || hsize > len || nsec > 64 ||
        (size_t)BC_HEADER + ((size_t)nsec * BC_SECTION) > hsize) {
        return BC_TABLE_HEADER;
    }
    if (bc_checksum(data, len) != get_u32(data + 8)) {
        return BC_TABLE_CHECKSUM;
    }
    memset(have, 0, sizeof(bool) * (BC_SECTIONS + 1));
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *e = data + BC_HEADER + ((size_t)i * BC_SECTION);
        uint32_t kind = get_u16(e);
        uint32_t off = get_u32(e + 4);
        uint32_t n = get_u32(e + 8);
        if (off < hsize || off > len || n > len - off) {
            return BC_TABLE_SECTIONS;
        }
        if (kind < 1 || kind > BC_SECTIONS) {
            continue; /* a kind from a later version: skipped */
        }
        if (have[kind]) {
            return BC_TABLE_SECTIONS;
        }
        have[kind] = true;
        sec[kind].p = data + off;
        sec[kind].end = data + off + n;
        sec[kind].bad = false;
    }
    if (!have[BC_SEC_CODE] || !have[BC_SEC_FUNCTIONS] || !have[BC_SEC_EXPORTS]) {
        return BC_TABLE_SECTIONS;
    }
    return BC_TABLE_OK;
}

bool filo_bc_declares(const uint8_t *data, size_t len, const char *entry) {
    bc_rd sec[BC_SECTIONS + 1];
    bool have[BC_SECTIONS + 1];
    if (bc_table(data, len, sec, have) != BC_TABLE_OK) {
        return false;
    }
    bc_rd *r = &sec[BC_SEC_EXPORTS];
    uint32_t n = rd_uleb(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        filo_str name = rd_name(r);
        (void)rd_uleb(r);
        if (!r->bad && name.ptr != NULL && cname_eq(entry, name.ptr, name.len)) {
            return true;
        }
    }
    return false;
}

static int bc_load(filo_ctx *ctx, const uint8_t *data, size_t len, bool lazy,
                   const filo_unit **out) {
    clear_error(ctx);
    *out = NULL;
    bc_rd sec[BC_SECTIONS + 1];
    bool have[BC_SECTIONS + 1];
    switch (bc_table(data, len, sec, have)) {
    case BC_TABLE_OK:
        break;
    case BC_TABLE_NOT_UNIT:
        return filo_fail(ctx, "bytecode: not a Filo unit");
    case BC_TABLE_VERSION:
        return filo_fail(ctx, "bytecode: a kind or version this runtime does not read");
    case BC_TABLE_HEADER:
        return bc_bad(ctx, "header");
    case BC_TABLE_CHECKSUM:
        return filo_fail(ctx, "bytecode: the checksum does not match (a damaged unit)");
    case BC_TABLE_SECTIONS:
        return bc_bad(ctx, "section table");
    }
    filo_unit *u = palloc(ctx, sizeof(filo_unit));
    if (u == NULL) {
        return FILO_ERR;
    }
    memset(u, 0, sizeof(*u));
    u->code = sec[BC_SEC_CODE].p;
    u->code_len = (uint32_t)(sec[BC_SEC_CODE].end - sec[BC_SEC_CODE].p);
    if (have[BC_SEC_DEBUG]) {
        u->debug = sec[BC_SEC_DEBUG].p;
        u->debug_len = (uint32_t)(sec[BC_SEC_DEBUG].end - sec[BC_SEC_DEBUG].p);
    }
    bc_missing m;
    m.len = 0;
    m.n = 0;
    m.cut = false;
    m.text[0] = '\0';
    if ((have[BC_SEC_IMPORTS] && bc_load_imports(ctx, &sec[BC_SEC_IMPORTS], u, &m) != FILO_OK) ||
        (have[BC_SEC_GLOBALS] && bc_load_globals(ctx, &sec[BC_SEC_GLOBALS], u, &m) != FILO_OK) ||
        (have[BC_SEC_EXTERNS] &&
         bc_load_externs(ctx, &sec[BC_SEC_EXTERNS], u, lazy, &m) != FILO_OK) ||
        (have[BC_SEC_CONSTANTS] && bc_load_consts(ctx, &sec[BC_SEC_CONSTANTS], u) != FILO_OK) ||
        bc_load_fns(ctx, &sec[BC_SEC_FUNCTIONS], u) != FILO_OK ||
        bc_load_exports(ctx, &sec[BC_SEC_EXPORTS], u) != FILO_OK) {
        return FILO_ERR;
    }
    if (m.n > 0) {
        return bc_refuse(ctx, &m);
    }
    *out = u;
    return FILO_OK;
}

int filo_bc_load(filo_ctx *ctx, const uint8_t *data, size_t len, const filo_unit **out) {
    return bc_load(ctx, data, len, false, out);
}

int filo_bc_load_lazy(filo_ctx *ctx, const uint8_t *data, size_t len, const filo_unit **out) {
    return bc_load(ctx, data, len, true, out);
}

/* ---- the machine ---- */

typedef struct bc_act bc_act;
struct bc_act {
    const bc_fn *fn;
    frame *f;
    uint32_t pc; /* in the code section */
    filo_value *stack;
    uint32_t sp;
    bc_act *caller;
    size_t mark; /* the run arena before the call, for its region */
    uint64_t escapes;
};

static bc_act *vm_activation(filo_ctx *ctx, const bc_fn *fn, frame *parent, const filo_value *args,
                             uint32_t n) {
    bc_act *a = ralloc(ctx, sizeof(bc_act));
    frame *f = ralloc(ctx, sizeof(frame));
    if (a == NULL || f == NULL) {
        return NULL;
    }
    f->slots = NULL;
    if (fn->nslots > 0) {
        f->slots = ralloc(ctx, sizeof(filo_value) * fn->nslots);
        if (f->slots == NULL) {
            return NULL;
        }
        memset(f->slots, 0, sizeof(filo_value) * fn->nslots);
    }
    if (n > fn->nslots) {
        (void)filo_fail(ctx, "bytecode: more arguments than the frame holds");
        return NULL;
    }
    if (n > 0 && f->slots != NULL) {
        memcpy(f->slots, args, sizeof(filo_value) * n);
    }
    f->n = fn->nslots;
    f->parent = parent;
    memset(a, 0, sizeof(*a));
    /* at least one value, so the stack is never NULL: every access is
       checked against maxstack first */
    a->stack = ralloc(ctx, sizeof(filo_value) * (fn->maxstack > 0 ? fn->maxstack : 1U));
    if (a->stack == NULL) {
        return NULL;
    }
    a->fn = fn;
    a->f = f;
    a->pc = fn->off;
    return a;
}

static int vm_push(filo_ctx *ctx, bc_act *a, filo_value v) {
    if (a->sp >= a->fn->maxstack) {
        return filo_fail(ctx, "bytecode: operand stack overflow");
    }
    a->stack[a->sp] = v;
    a->sp++;
    return FILO_OK;
}

static int vm_need(filo_ctx *ctx, const bc_act *a, uint32_t n) {
    if (a->sp < n) {
        return filo_fail(ctx, "bytecode: operand stack underflow");
    }
    return FILO_OK;
}

static bool vm_uleb(const uint8_t *code, uint32_t end, uint32_t *pc, uint32_t *out) {
    bc_rd r = {code + *pc, code + end, false};
    *out = rd_uleb(&r);
    *pc = (uint32_t)(r.p - code);
    if (r.bad) {
        return false;
    }
    return true;
}

static frame *vm_frame_up(const bc_act *a, uint32_t depth) {
    frame *f = a->f;
    for (uint32_t i = 0; i < depth && f != NULL; i++) {
        f = f->parent;
    }
    return f;
}

static int vm_trap(filo_ctx *ctx, const filo_value *msg) {
    if (msg->kind != FILO_STRING) {
        return filo_fail(ctx, "bytecode: a trap without a message");
    }
    char text[FILO_ERROR_MAX] = {0};
    size_t n = msg->u.str.len < sizeof(text) - 1 ? msg->u.str.len : sizeof(text) - 1;
    if (n > 0) {
        memcpy(text, msg->u.str.ptr, n);
    }
    text[n] = '\0';
    return filo_fail(ctx, text);
}

static int vm_jump(filo_ctx *ctx, bc_act *a, uint32_t cond, uint32_t end) {
    if (a->pc + 2U > end) {
        return filo_fail(ctx, "bytecode: truncated jump");
    }
    const uint8_t *code = a->fn->unit->code;
    int32_t off = (int16_t)(uint16_t)(code[a->pc] | ((uint32_t)code[a->pc + 1] << 8U));
    a->pc += 2U;
    bool take = cond == BC_J_ALWAYS;
    if (cond != BC_J_ALWAYS) {
        if (cond > BC_J_CHECK) {
            return filo_fail(ctx, "bytecode: unknown jump condition");
        }
        if (vm_need(ctx, a, 1) != FILO_OK) {
            return FILO_ERR;
        }
        const filo_value *v = &a->stack[a->sp - 1];
        if (v->kind != FILO_BOOL) {
            return fail_expected(ctx, "bool", v);
        }
        bool b = v->u.b;
        if (cond == BC_J_FALSE) {
            a->sp--;
        }
        if (cond == BC_J_FALSE || cond == BC_J_AND) {
            take = true; /* these jump on false */
            if (b) {
                take = false;
            }
        }
        if (cond == BC_J_OR) {
            take = b;
        }
        if (!take && (cond == BC_J_AND || cond == BC_J_OR)) {
            a->sp--;
        }
    }
    if (!take) {
        return FILO_OK;
    }
    int64_t target = (int64_t)a->pc + off;
    if (target < (int64_t)a->fn->off || target > (int64_t)end) {
        return filo_fail(ctx, "bytecode: a jump out of its function");
    }
    a->pc = (uint32_t)target;
    return FILO_OK;
}

/* A call to a function value with argc arguments on top of the stack, and
   drop values to take off when it returns (the arguments, and the function
   itself when it was on the stack too): bytecode gets a new activation and
   the machine moves into it; anything else runs through filo_call. */
static int vm_invoke(filo_ctx *ctx, bc_act **ap, filo_value fnv, uint32_t argc, uint32_t drop) {
    bc_act *a = *ap;
    if (fnv.kind != FILO_FUNC) {
        char msg[64];
        size_t k = cstr_copy(msg, sizeof(msg), "attempt to call non-function (got ");
        k += cstr_copy(msg + k, sizeof(msg) - k, filo_kind_name(fnv.kind));
        (void)cstr_copy(msg + k, sizeof(msg) - k, ")");
        return filo_fail(ctx, msg);
    }
    const filo_func *fn = fnv.u.fn;
    const filo_value *args = &a->stack[a->sp - argc];
    if (fn->bc == NULL) {
        filo_value r;
        memset(&r, 0, sizeof(r));
        if (filo_call(ctx, &fnv, args, argc, &r) != FILO_OK) {
            return FILO_ERR;
        }
        a->sp -= drop;
        return vm_push(ctx, a, r);
    }
    if (fn->bc->nparams != argc) {
        return fail_arity(ctx, fn->bc->nparams, argc);
    }
    if (enter_call(ctx) != FILO_OK) {
        return FILO_ERR;
    }
    size_t mark = ctx->run.used;
    uint64_t escapes = ctx->escapes;
    bc_act *n = vm_activation(ctx, fn->bc, fn->frame, args, argc);
    if (n == NULL) {
        ctx->recursion--;
        return FILO_ERR;
    }
    a->sp -= drop;
    n->caller = a;
    n->mark = mark;
    n->escapes = escapes;
    *ap = n;
    return FILO_OK;
}

static int vm_callb(filo_ctx *ctx, bc_act **ap, uint32_t argc, uint32_t end) {
    bc_act *a = *ap;
    const filo_unit *u = a->fn->unit;
    uint32_t idx = 0;
    if (!vm_uleb(u->code, end, &a->pc, &idx) || idx >= u->nimports) {
        return filo_fail(ctx, "bytecode: a builtin outside the imports");
    }
    if (vm_need(ctx, a, argc) != FILO_OK) {
        return FILO_ERR;
    }
    const bc_imported *imp = &u->imports[idx];
    if (imp->builtin == NULL) {
        if (!ctx->defined[imp->sym]) {
            return filo_fail2(ctx, "undefined global: ", ctx->symbols[imp->sym]);
        }
        return vm_invoke(ctx, ap, ctx->globals[imp->sym], argc, argc);
    }
    filo_value r;
    memset(&r, 0, sizeof(r));
    if (call_entry(ctx, imp->builtin, &a->stack[a->sp - argc], argc, &r) != FILO_OK) {
        return FILO_ERR;
    }
    a->sp -= argc;
    return vm_push(ctx, a, r);
}

/* A call to a function value below its arguments on the stack. */
static int vm_call(filo_ctx *ctx, bc_act **ap, uint32_t argc) {
    const bc_act *a = *ap;
    if (vm_need(ctx, a, argc + 1) != FILO_OK) {
        return FILO_ERR;
    }
    return vm_invoke(ctx, ap, a->stack[a->sp - argc - 1], argc, argc + 1);
}

/* A builtin as a value: the import resolved to one, or to the global that
   holds the function in Filo this VM has for that name. */
static int vm_push_b(filo_ctx *ctx, bc_act *a, uint32_t idx) {
    const filo_unit *u = a->fn->unit;
    if (idx >= u->nimports) {
        return filo_fail(ctx, "bytecode: a builtin outside the imports");
    }
    const bc_imported *imp = &u->imports[idx];
    filo_value v;
    if (imp->builtin == NULL) {
        if (!ctx->defined[imp->sym]) {
            return filo_fail2(ctx, "undefined global: ", ctx->symbols[imp->sym]);
        }
        return vm_push(ctx, a, ctx->globals[imp->sym]);
    }
    if (builtin_value(ctx, imp->builtin, &v) != FILO_OK) {
        return FILO_ERR;
    }
    return vm_push(ctx, a, v);
}

static int vm_closure(filo_ctx *ctx, bc_act *a, uint32_t idx) {
    const filo_unit *u = a->fn->unit;
    if (idx >= u->nfns) {
        return filo_fail(ctx, "bytecode: a function outside the unit");
    }
    filo_func *fn = ralloc(ctx, sizeof(filo_func));
    if (fn == NULL) {
        return FILO_ERR;
    }
    memset(fn, 0, sizeof(*fn));
    fn->nparams = u->fns[idx].nparams;
    fn->frame = a->f;
    fn->bc = &u->fns[idx];
    ctx->escapes++; /* it captures the frame, by reference */
    filo_value v;
    memset(&v, 0, sizeof(v));
    v.kind = FILO_FUNC;
    v.u.fn = fn;
    return vm_push(ctx, a, v);
}

static int vm_unpack(filo_ctx *ctx, bc_act *a, uint32_t n) {
    if (vm_need(ctx, a, 1) != FILO_OK) {
        return FILO_ERR;
    }
    a->sp--;
    filo_value t = a->stack[a->sp];
    if (t.kind != FILO_TUPLE) {
        return filo_fail(ctx, "letv expects tuple expression");
    }
    if (t.u.seq.len != n) {
        return filo_fail(ctx, "letv arity mismatch");
    }
    for (uint32_t i = 0; i < n; i++) {
        if (vm_push(ctx, a, seq_at(&t.u.seq, i)) != FILO_OK) {
            return FILO_ERR;
        }
    }
    return FILO_OK;
}

/* A slot of the current frame or of one further out, checked: a unit is
   untrusted input. */
static filo_value *vm_slot(filo_ctx *ctx, const bc_act *a, uint32_t depth, uint32_t slot) {
    frame *f = vm_frame_up(a, depth);
    if (f == NULL || slot >= f->n) {
        (void)filo_fail(ctx, "bytecode: a slot outside its frame");
        return NULL;
    }
    return &f->slots[slot];
}

/* Runs until base returns. */
/* Where pc came from in the source: the last entry of the unit's debug
   table at or before it. The table is untrusted like the rest of a unit, so
   a malformed one only stops the search. */
static bool bc_position(const filo_unit *u, uint32_t pc, uint32_t *line, uint32_t *col) {
    if (u->debug == NULL) {
        return false;
    }
    bc_rd r = {u->debug, u->debug + u->debug_len, false};
    uint32_t at = 0;
    int64_t l = 0;
    uint32_t cl = 0;
    bool found = false;
    while (r.p < r.end) {
        uint32_t d = rd_uleb(&r);
        uint32_t z = rd_uleb(&r);
        uint32_t cc = rd_uleb(&r);
        if (r.bad || d > UINT32_MAX - at) {
            break;
        }
        at += d;
        if (at > pc) {
            break;
        }
        l += (z & 1U) != 0 ? -(int64_t)((z + 1U) / 2U) : (int64_t)(z / 2U);
        if (l < 1 || l > (int64_t)UINT32_MAX) {
            break;
        }
        cl = cc;
        found = true;
    }
    if (found) {
        *line = (uint32_t)l;
        *col = cl;
    }
    return found;
}

/* The instruction that failed is the one just decoded in a: its position
   is the error's, unless one further in (a builtin calling back) already
   said where. */
static void vm_where(filo_ctx *ctx, const bc_act *a) {
    if (ctx->signal != SIG_NONE || ctx->error_line != 0) {
        return;
    }
    uint32_t pc = a->pc > a->fn->off ? a->pc - 1U : a->fn->off;
    uint32_t line = 0;
    uint32_t col = 0;
    if (bc_position(a->fn->unit, pc, &line, &col)) {
        ctx->error_line = line;
        ctx->error_col = col;
    }
}

/* *cur follows the activation that is running, written when a call or a
   return changes it and never per instruction: the caller reads it only
   when the loop fails, to say where. */
static int vm_loop(filo_ctx *ctx, const bc_act *base, bc_act **cur, bool pausable,
                   filo_value *out) {
    bc_act *a = *cur;
    for (;;) {
        if (ctx->host.should_stop != NULL && ctx->host.should_stop(ctx->host.user)) {
            return filo_fail(ctx, "execution cancelled");
        }
        ctx->steps++;
        /* one comparison per instruction for both: past vm_stop it is the
           step limit, or the pause, which only the run's own loop takes (a
           loop under a builtin cannot stop; the one above it will) */
        if (ctx->steps > ctx->vm_stop) {
            if (ctx->limits.step_limit > 0 && ctx->steps > ctx->limits.step_limit) {
                return filo_fail(ctx, "step limit exceeded");
            }
            if (pausable) {
                ctx->steps--; /* the instruction did not run */
                return FILO_PAUSED;
            }
        }
        const filo_unit *u = a->fn->unit;
        uint32_t end = a->fn->off + a->fn->len;
        if (a->pc >= end) {
            return filo_fail(ctx, "bytecode: ran past the end of a function");
        }
        if (ctx->host.trace != NULL) {
            filo_trace t = {a->pc, a->stack, a->sp, ctx->recursion};
            ctx->host.trace(ctx->host.user, &t);
        }
        uint32_t b = u->code[a->pc];
        a->pc++;
        uint32_t op = b >> 3U;
        uint32_t x = b & 7U;
        if (x == 7U && op != BC_JMP && !vm_uleb(u->code, end, &a->pc, &x)) {
            return filo_fail(ctx, "bytecode: a truncated operand");
        }
        int rc = FILO_OK;
        switch (op) {
        case BC_PUSH_K:
            if (x >= u->nconsts) {
                return filo_fail(ctx, "bytecode: a constant outside the unit");
            }
            rc = vm_push(ctx, a, u->consts[x]);
            break;
        case BC_PUSH_G: {
            if (x >= u->nglobals) {
                return filo_fail(ctx, "bytecode: a global outside the unit");
            }
            uint32_t id = u->globals[x];
            if (!ctx->defined[id]) {
                return filo_fail2(ctx, "undefined global: ", ctx->symbols[id]);
            }
            rc = vm_push(ctx, a, ctx->globals[id]);
            break;
        }
        case BC_STORE_G:
            if (x >= u->nglobals) {
                return filo_fail(ctx, "bytecode: a global outside the unit");
            }
            if (vm_need(ctx, a, 1) != FILO_OK) {
                return FILO_ERR;
            }
            if (set_global_id(ctx, u->globals[x], a->stack[a->sp - 1]) != FILO_OK) {
                return FILO_ERR;
            }
            break;
        case BC_PUSH_L:
        case BC_STORE_L:
        case BC_PUSH_UP:
        case BC_STORE_UP: {
            uint32_t depth = 0;
            uint32_t slot = x;
            if (op == BC_PUSH_UP || op == BC_STORE_UP) {
                depth = x;
                if (!vm_uleb(u->code, end, &a->pc, &slot)) {
                    return filo_fail(ctx, "bytecode: a truncated operand");
                }
            }
            filo_value *s = vm_slot(ctx, a, depth, slot);
            if (s == NULL) {
                return FILO_ERR;
            }
            if (op == BC_PUSH_L || op == BC_PUSH_UP) {
                rc = vm_push(ctx, a, *s);
                break;
            }
            if (vm_need(ctx, a, 1) != FILO_OK) {
                return FILO_ERR;
            }
            if (depth > 0) {
                ctx->escapes++; /* a frame further out now holds it */
            }
            *s = a->stack[a->sp - 1];
            break;
        }
        case BC_POP:
            rc = vm_need(ctx, a, x);
            if (rc == FILO_OK) {
                a->sp -= x;
            }
            break;
        case BC_JMP:
            rc = vm_jump(ctx, a, x, end);
            break;
        case BC_CALL:
            rc = vm_call(ctx, &a, x);
            *cur = a;
            break;
        case BC_CALLB:
            rc = vm_callb(ctx, &a, x, end);
            *cur = a;
            break;
        case BC_RET: {
            if (vm_need(ctx, a, 1) != FILO_OK) {
                return FILO_ERR;
            }
            filo_value v = a->stack[a->sp - 1];
            if (x == 1U) {
                ctx->signal = SIG_EXIT; /* ends the run from wherever it is */
                ctx->signaled = v;
                return FILO_ERR;
            }
            if (x != 0U) {
                return filo_fail(ctx, "bytecode: unknown return");
            }
            if (a == base) {
                *out = v;
                return FILO_OK;
            }
            bc_act *caller = a->caller;
            ctx->recursion--;
            if (ctx->escapes == a->escapes) {
                region_release(ctx, a->mark, &v);
            }
            a = caller;
            *cur = a;
            rc = vm_push(ctx, a, v);
            break;
        }
        case BC_CLOSURE:
            rc = vm_closure(ctx, a, x);
            break;
        case BC_TUPLE: {
            rc = vm_need(ctx, a, x);
            if (rc != FILO_OK) {
                break;
            }
            filo_value t;
            rc = filo_tuple(ctx, &a->stack[a->sp - x], x, &t);
            if (rc == FILO_OK) {
                a->sp -= x;
                rc = vm_push(ctx, a, t);
            }
            break;
        }
        case BC_UNPACK:
            rc = vm_unpack(ctx, a, x);
            break;
        case BC_TRAP:
            if (x >= u->nconsts) {
                return filo_fail(ctx, "bytecode: a constant outside the unit");
            }
            return vm_trap(ctx, &u->consts[x]);
        case BC_PUSH_B:
            rc = vm_push_b(ctx, a, x);
            break;
        default:
            return filo_fail(ctx, "bytecode: unknown instruction");
        }
        if (rc != FILO_OK) {
            return FILO_ERR;
        }
    }
}

/* Runs from *cur, an activation of base's run; *cur is where it stopped. */
static int vm_exec_from(filo_ctx *ctx, const bc_act *base, bc_act **cur, bool pausable,
                        filo_value *out) {
    if (ctx->depth >= FILO_EVAL_DEPTH_MAX) {
        return filo_fail(ctx, "evaluation too deep");
    }
    ctx->depth++;
    int rc = vm_loop(ctx, base, cur, pausable, out);
    if (rc == FILO_ERR) {
        vm_where(ctx, *cur);
    }
    ctx->depth--;
    return rc;
}

static int vm_exec(filo_ctx *ctx, bc_act *base, filo_value *out) {
    bc_act *cur = base;
    return vm_exec_from(ctx, base, &cur, false, out);
}

/* A bytecode closure called by the IR or by a builtin through filo_call:
   the recursion depth is already counted, and released here as run_func
   releases it. */
static int vm_run_func(filo_ctx *ctx, const filo_func *fn, const filo_value *slots, uint32_t n,
                       filo_value *out) {
    bc_act *a = vm_activation(ctx, fn->bc, fn->frame, slots, n);
    int rc = FILO_ERR;
    if (a != NULL) {
        rc = vm_exec(ctx, a, out);
    }
    ctx->recursion--;
    return rc;
}

/* ------------------------------------------------------------- rendering */

/* A sink that either counts or writes, so a value is rendered twice: once to
   size the run-arena buffer, once into it. */
typedef struct {
    char *dst;
    size_t cap;
    size_t pos;
} sink;

static void sink_put(sink *s, const char *bytes, size_t n) {
    if (s->dst != NULL && s->pos < s->cap) {
        size_t room = s->cap - s->pos;
        memcpy(s->dst + s->pos, bytes, n < room ? n : room);
    }
    s->pos += n;
}

static void sink_puts(sink *s, const char *str) {
    sink_put(s, str, strlen(str));
}

/* Quotes like Go's %q: printable bytes verbatim, the usual escapes named,
   other control bytes as \x.. */
static void sink_quoted(sink *s, filo_str str) {
    static const char hex[] = "0123456789abcdef";
    sink_puts(s, "\"");
    for (uint32_t i = 0; i < str.len; i++) {
        uint8_t c = str.ptr[i];
        char esc[5] = {'\\', 0, 0, 0, 0};
        switch (c) {
        case '"':
            esc[1] = '"';
            sink_put(s, esc, 2);
            break;
        case '\\':
            esc[1] = '\\';
            sink_put(s, esc, 2);
            break;
        case '\n':
            esc[1] = 'n';
            sink_put(s, esc, 2);
            break;
        case '\t':
            esc[1] = 't';
            sink_put(s, esc, 2);
            break;
        case '\r':
            esc[1] = 'r';
            sink_put(s, esc, 2);
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                esc[1] = 'x';
                esc[2] = hex[(unsigned)c >> 4U];
                esc[3] = hex[(unsigned)c & 0xFU];
                sink_put(s, esc, 4);
            } else {
                sink_put(s, (const char *)&str.ptr[i], 1);
            }
            break;
        }
    }
    sink_puts(s, "\"");
}

/* A list or a tuple may hold the same value more than once, so a value
   made in a few steps, (fold (fn (a x) (tuple a a)) 0 (range 60)), is small
   in memory and 2^60 parts to walk. Whatever walks a value stops at
   WALK_PARTS_MAX parts and WALK_LEVELS_MAX levels, the Go engine's
   ceilings, checked in its order, so both fail on the same value: writing
   it checks first (walkable), comparing it counts as it goes (equal_walk).
   The levels also bound the C stack the walk takes. */
enum {
    WALK_PARTS_MAX = 1U << 22U,
    WALK_LEVELS_MAX = FILO_EVAL_DEPTH_MAX,
};

static int walk_check(filo_ctx *ctx, const filo_value *v, uint32_t level, uint32_t *parts) {
    (*parts)++;
    if (*parts > WALK_PARTS_MAX) {
        return filo_fail(ctx, "value too large: more than 4194304 parts");
    }
    if (v->kind != FILO_LIST && v->kind != FILO_TUPLE) {
        return FILO_OK;
    }
    if (level > WALK_LEVELS_MAX) {
        return filo_fail(ctx, "value too deep: more than 512 levels");
    }
    for (uint32_t i = 0; i < v->u.seq.len; i++) {
        filo_value item = seq_at(&v->u.seq, i);
        if (walk_check(ctx, &item, level + 1U, parts) != FILO_OK) {
            return FILO_ERR;
        }
    }
    return FILO_OK;
}

static int walkable(filo_ctx *ctx, const filo_value *v) {
    uint32_t parts = 0;
    return walk_check(ctx, v, 1, &parts);
}

static int render(filo_ctx *ctx, const filo_value *v, sink *s, bool quote) {
    switch (v->kind) {
    case FILO_NUMBER: {
        char buf[48];
        if (ctx->host.num_to_str == NULL) {
            return filo_fail(ctx, "number formatting unavailable on this host");
        }
        size_t n = ctx->host.num_to_str(ctx->host.user, v->u.num, buf, sizeof(buf));
        if (n == 0) {
            return filo_fail(ctx, "number formatting failed");
        }
        sink_put(s, buf, n);
        return FILO_OK;
    }
    case FILO_BOOL:
        sink_puts(s, v->u.b ? "#t" : "#f");
        return FILO_OK;
    case FILO_STRING:
        if (quote) {
            sink_quoted(s, v->u.str);
        } else {
            sink_put(s, (const char *)v->u.str.ptr, v->u.str.len);
        }
        return FILO_OK;
    case FILO_LIST:
    case FILO_TUPLE:
        sink_puts(s, v->kind == FILO_LIST ? "(list" : "(tuple");
        for (uint32_t i = 0; i < v->u.seq.len; i++) {
            sink_puts(s, " ");
            filo_value item = seq_at(&v->u.seq, i);
            if (render(ctx, &item, s, true) != FILO_OK) {
                return FILO_ERR;
            }
        }
        sink_puts(s, ")");
        return FILO_OK;
    case FILO_FUNC:
        return filo_fail(ctx, "string: cannot convert a function");
    default:
        return filo_fail(ctx, "cannot render value");
    }
}

int filo_value_text(filo_ctx *ctx, const filo_value *v, char *dst, size_t cap, size_t *len) {
    sink s = {dst, cap, 0};
    if (walkable(ctx, v) != FILO_OK || render(ctx, v, &s, false) != FILO_OK) {
        return FILO_ERR;
    }
    *len = s.pos;
    if (dst != NULL && s.pos < cap) {
        dst[s.pos] = '\0';
    }
    return FILO_OK;
}

int filo_value_repr(filo_ctx *ctx, const filo_value *v, char *dst, size_t cap, size_t *len) {
    sink s = {dst, cap, 0};
    if (walkable(ctx, v) != FILO_OK || render(ctx, v, &s, true) != FILO_OK) {
        return FILO_ERR;
    }
    *len = s.pos;
    if (dst != NULL && s.pos < cap) {
        dst[s.pos] = '\0';
    }
    return FILO_OK;
}

/* Renders into a fresh run-arena string value — what (string v) yields. */
static int render_to_value(filo_ctx *ctx, const filo_value *v, filo_value *out) {
    sink count = {NULL, 0, 0};
    if (walkable(ctx, v) != FILO_OK || render(ctx, v, &count, false) != FILO_OK) {
        return FILO_ERR;
    }
    char *buf = NULL;
    if (count.pos > 0) {
        buf = ralloc(ctx, count.pos);
        if (buf == NULL) {
            return FILO_ERR;
        }
        sink w = {buf, count.pos, 0};
        (void)render(ctx, v, &w, false);
    }
    *out = filo_string((const uint8_t *)buf, (uint32_t)count.pos);
    return FILO_OK;
}

/* ------------------------------------------------------------ public API */

static void register_core(filo_ctx *ctx);

int filo_init(filo_ctx *ctx, const filo_host *host, void *persistent, size_t persistent_cap,
              void *run, size_t run_cap) {
    memset(ctx, 0, sizeof(*ctx));
    if (host != NULL) {
        ctx->host = *host;
    }
    ctx->persistent.base = persistent;
    ctx->persistent.cap = persistent_cap;
    ctx->run.base = run;
    ctx->run.cap = run_cap;
    ctx->limits.step_limit = FILO_STEP_LIMIT_DEFAULT;
    ctx->limits.recursion_limit = FILO_RECURSION_LIMIT_DEFAULT;
    register_core(ctx);
    return FILO_OK;
}

int filo_register_builtin(filo_ctx *ctx, const char *name, filo_builtin fn) {
    if (name == NULL || name[0] == '\0' || fn == NULL) {
        return filo_fail(ctx, "builtin needs a name and a function");
    }
    if (builtin_by_name(ctx, (const uint8_t *)name, (uint32_t)strlen(name)) != NULL) {
        return filo_fail2(ctx, "builtin already registered: ", name);
    }
    if (ctx->nbuiltins >= FILO_BUILTINS_MAX) {
        return filo_fail(ctx, "too many builtins");
    }
    ctx->builtins[ctx->nbuiltins].name = name;
    ctx->builtins[ctx->nbuiltins].fn = fn;
    ctx->builtins[ctx->nbuiltins].value = NULL;
    ctx->nbuiltins++;
    return FILO_OK;
}

#ifndef FILO_VM_ONLY
int filo_compile(filo_ctx *ctx, const uint8_t *src, size_t len, filo_prog *out) {
    /* the parse tree and the lowering scopes are run-arena temporaries */
    cancel_paused(ctx);
    arena_reset(&ctx->run);
    clear_error(ctx);
    node *tree = parse_all(ctx, src, len);
    if (tree == NULL) {
        return FILO_ERR;
    }
    tree = fold(ctx, tree);
    lowerer lw = {ctx, NULL, 0, 0};
    if (scope_enter(&lw) == NULL) {
        return FILO_ERR;
    }
    const filo_instr *root = lower(&lw, tree);
    if (root == NULL) {
        return FILO_ERR;
    }
    out->root = root;
    return FILO_OK;
}
#endif

#ifndef FILO_VM_ONLY
/* ---- the stages, shown ---- */

static const char *const op_names[] = {
    "const", "local", "global", "dynamic", "builtin", "empty", "invalid", "if",
    "cond",  "do",    "and",    "or",      "let",     "letv",  "set",     "fn",
    "def",   "tuple", "exit",   "return",  "callb",   "call",
};

typedef struct {
    filo_ctx *ctx;
    filo_line out;
    void *user;
} shower;

/* "line:col" and the text, indented by depth. */
static void show_line(shower *s, uint32_t line, uint32_t col, uint32_t depth, const char *text) {
    char buf[FILO_ERROR_MAX + 64];
    char at[24];
    size_t n = u32_text(at, sizeof(at), line);
    n += cstr_copy(at + n, sizeof(at) - n, ":");
    (void)u32_text(at + n, sizeof(at) - n, col);
    size_t k = cstr_copy(buf, sizeof(buf), at);
    while (k < 8 && k + 1 < sizeof(buf)) {
        buf[k] = ' ';
        k++;
    }
    for (uint32_t i = 0; i < depth * 2U && k + 1 < sizeof(buf); i++) {
        buf[k] = ' ';
        k++;
    }
    (void)cstr_copy(buf + k, sizeof(buf) - k, text);
    s->out(s->user, buf);
}

/* A value as source, cut to what a line can hold. */
static void show_value_text(shower *s, const filo_value *v, char *dst, size_t cap) {
    size_t n = 0;
    if (filo_value_repr(s->ctx, v, dst, cap, &n) != FILO_OK) {
        (void)cstr_copy(dst, cap, "?");
        return;
    }
    dst[n < cap ? n : cap - 1] = '\0';
}

static void show_tree(shower *s, const node *n, uint32_t depth) {
    char text[FILO_ERROR_MAX];
    char val[160];
    filo_value v = {0};
    switch (n->kind) {
    case N_LIST: {
        size_t k = cstr_copy(text, sizeof(text), "list of ");
        (void)u32_text(text + k, sizeof(text) - k, n->nelems);
        show_line(s, n->line, n->col, depth, text);
        for (uint32_t i = 0; i < n->nelems; i++) {
            show_tree(s, n->elems[i], depth + 1);
        }
        return;
    }
    case N_SYMBOL: {
        size_t k = cstr_copy(text, sizeof(text), "symbol ");
        size_t take = n->text.len < sizeof(text) - k - 1 ? n->text.len : sizeof(text) - k - 1;
        memcpy(text + k, n->text.ptr, take);
        text[k + take] = '\0';
        show_line(s, n->line, n->col, depth, text);
        return;
    }
    case N_NUMBER:
        v = filo_num(n->num);
        break;
    case N_BOOL:
        v = filo_bool(n->b);
        break;
    default:
        v = filo_string(n->text.ptr, n->text.len);
        break;
    }
    show_value_text(s, &v, val, sizeof(val));
    size_t k = cstr_copy(text, sizeof(text),
                         n->kind == N_NUMBER ? "number "
                         : n->kind == N_BOOL ? "bool "
                                             : "string ");
    (void)cstr_copy(text + k, sizeof(text) - k, val);
    show_line(s, n->line, n->col, depth, text);
}

static void show_ir(shower *s, const filo_instr *in, uint32_t depth) {
    char text[FILO_ERROR_MAX];
    size_t k = cstr_copy(text, sizeof(text), in->op < 22 ? op_names[in->op] : "?");
    if (in->op == OP_CONST) {
        char val[160];
        show_value_text(s, &in->val, val, sizeof(val));
        k += cstr_copy(text + k, sizeof(text) - k, " ");
        k += cstr_copy(text + k, sizeof(text) - k, val);
    } else if (in->name != NULL) {
        k += cstr_copy(text + k, sizeof(text) - k, " ");
        k += cstr_copy(text + k, sizeof(text) - k, in->name);
    }
    if (in->op == OP_LOCAL) {
        k += cstr_copy(text + k, sizeof(text) - k, "  (frame ");
        k += u32_text(text + k, sizeof(text) - k, in->a);
        k += cstr_copy(text + k, sizeof(text) - k, " out, slot ");
        k += u32_text(text + k, sizeof(text) - k, in->b);
        k += cstr_copy(text + k, sizeof(text) - k, ")");
    }
    if (in->nnames > 0) {
        k += cstr_copy(text + k, sizeof(text) - k, "  names");
        for (uint32_t i = 0; i < in->nnames; i++) {
            k += cstr_copy(text + k, sizeof(text) - k, " ");
            k += cstr_copy(text + k, sizeof(text) - k, in->names[i]);
        }
    }
    if (in->msg != NULL) {
        k += cstr_copy(text + k, sizeof(text) - k, "  error: ");
        (void)cstr_copy(text + k, sizeof(text) - k, in->msg);
    }
    show_line(s, in->line, in->col, depth, text);
    for (uint32_t i = 0; i < in->nargs; i++) {
        show_ir(s, in->args[i], depth + 1);
    }
    for (uint32_t i = 0; i < in->nclauses; i++) {
        const clause *cl = &in->clauses[i];
        show_line(s, in->line, in->col, depth + 1, cl->is_else ? "clause else" : "clause");
        if (cl->test != NULL) {
            show_ir(s, cl->test, depth + 2);
        }
        for (uint32_t j = 0; j < cl->nbody; j++) {
            show_ir(s, cl->body[j], depth + 2);
        }
    }
}

int filo_show(filo_ctx *ctx, const uint8_t *src, size_t len, const char *stage, filo_line out,
              void *user) {
    bool tree = strcmp(stage, "tree") == 0;
    bool folded = strcmp(stage, "folded") == 0;
    bool ir = strcmp(stage, "ir") == 0;
    if (!tree && !folded && !ir) {
        clear_error(ctx);
        return filo_fail2(ctx, "no stage named ", stage);
    }
    cancel_paused(ctx);
    arena_reset(&ctx->run);
    clear_error(ctx);
    shower s = {ctx, out, user};
    node *n = parse_all(ctx, src, len);
    if (n == NULL) {
        return FILO_ERR;
    }
    if (tree) {
        show_tree(&s, n, 0);
        return FILO_OK;
    }
    n = fold(ctx, n);
    if (folded) {
        show_tree(&s, n, 0);
        return FILO_OK;
    }
    lowerer lw = {ctx, NULL, 0, 0};
    if (scope_enter(&lw) == NULL) {
        return FILO_ERR;
    }
    const filo_instr *root = lower(&lw, n);
    if (root == NULL) {
        return FILO_ERR;
    }
    show_ir(&s, root, 0);
    return FILO_OK;
}
#endif

/* What every run does first and last, whatever runs in between: the IR or
   a unit's bytecode. */
static void run_begin(filo_ctx *ctx, const filo_limits *limits, filo_limits *saved) {
    cancel_paused(ctx);
    arena_reset(&ctx->run);
    clear_error(ctx);
    ctx->steps = 0;
    ctx->recursion = 0;
    ctx->depth = 0;
    ctx->frame = NULL;
    ctx->signal = SIG_NONE;
    *saved = ctx->limits;
    if (limits != NULL) {
        ctx->limits = *limits;
        if (ctx->limits.step_limit == 0) {
            ctx->limits.step_limit = FILO_STEP_LIMIT_DEFAULT;
        }
        if (ctx->limits.recursion_limit == 0) {
            ctx->limits.recursion_limit = FILO_RECURSION_LIMIT_DEFAULT;
        }
    }
    ctx->vm_stop = ctx->limits.step_limit > 0 ? ctx->limits.step_limit : UINT32_MAX;
}

static int run_end(filo_ctx *ctx, int rc, filo_value v, const filo_limits *saved,
                   filo_value *result) {
    ctx->limits = *saved;
    if (rc != FILO_OK && ctx->signal != SIG_NONE) {
        /* exit ends the run with its value; a top-level return acts alike */
        v = ctx->signaled;
        ctx->signal = SIG_NONE;
        clear_error(ctx);
        rc = FILO_OK;
    }
    if (rc != FILO_OK) {
        rollback_globals(ctx); /* a failed run leaves no trace, as in Go */
        return FILO_ERR;
    }
    if (commit_globals(ctx) != FILO_OK) {
        return FILO_ERR;
    }
    if (result != NULL) {
        if (materialise(ctx, &v) != FILO_OK) { /* it crosses the boundary */
            return FILO_ERR;
        }
        *result = v;
    }
    return FILO_OK;
}

#ifndef FILO_VM_ONLY
int filo_run(filo_ctx *ctx, const filo_prog *prog, const filo_limits *limits, filo_value *result) {
    filo_limits saved;
    run_begin(ctx, limits, &saved);
    filo_value v;
    memset(&v, 0, sizeof(v));
    int rc = eval(ctx, prog->root, &v);
    return run_end(ctx, rc, v, &saved, result);
}
#endif

static const bc_fn *bc_export(const filo_unit *unit, const char *entry) {
    const bc_fn *fn = NULL;
    size_t n = strlen(entry);
    for (uint32_t i = 0; i < unit->nexports; i++) {
        const filo_str *name = &unit->export_names[i];
        if (name->len == n && (n == 0 || memcmp(name->ptr, entry, n) == 0)) {
            fn = &unit->fns[unit->export_fns[i]];
        }
    }
    return fn;
}

bool filo_bc_has(const filo_unit *unit, const char *entry) {
    return bc_export(unit, entry) != NULL;
}

int filo_bc_run(filo_ctx *ctx, const filo_unit *unit, const char *entry, const filo_limits *limits,
                filo_value *result) {
    const bc_fn *fn = bc_export(unit, entry);
    if (fn == NULL) {
        clear_error(ctx);
        return filo_fail2(ctx, "bytecode: no entry point named ", entry);
    }
    filo_limits saved;
    run_begin(ctx, limits, &saved);
    filo_value v;
    memset(&v, 0, sizeof(v));
    int rc = FILO_ERR;
    if (fn->nparams != 0) {
        rc = filo_fail(ctx, "bytecode: an entry point takes no arguments");
    } else {
        bc_act *a = vm_activation(ctx, fn, NULL, NULL, 0);
        if (a != NULL) {
            rc = vm_exec(ctx, a, &v);
        }
    }
    return run_end(ctx, rc, v, &saved, result);
}

/* Runs the paused or new run from start for at most budget instructions. */
static int bc_go_on(filo_ctx *ctx, bc_act *base, bc_act *start, uint32_t budget,
                    filo_value *result) {
    uint32_t stop = ctx->limits.step_limit > 0 ? ctx->limits.step_limit : UINT32_MAX;
    if (budget > 0 && budget < stop - ctx->steps) {
        stop = ctx->steps + budget;
    }
    ctx->vm_stop = stop;
    filo_value v;
    memset(&v, 0, sizeof(v));
    bc_act *cur = start;
    int rc = vm_exec_from(ctx, base, &cur, true, &v);
    if (rc == FILO_PAUSED) {
        ctx->paused = cur;
        ctx->paused_base = base;
        return FILO_PAUSED;
    }
    filo_limits saved = ctx->paused_limits;
    return run_end(ctx, rc, v, &saved, result);
}

int filo_bc_start(filo_ctx *ctx, const filo_unit *unit, const char *entry,
                  const filo_limits *limits, uint32_t budget, filo_value *result) {
    const bc_fn *fn = bc_export(unit, entry);
    if (fn == NULL) {
        cancel_paused(ctx);
        clear_error(ctx);
        return filo_fail2(ctx, "bytecode: no entry point named ", entry);
    }
    run_begin(ctx, limits, &ctx->paused_limits);
    if (fn->nparams != 0) {
        filo_limits saved = ctx->paused_limits;
        filo_value v = {0};
        return run_end(ctx, filo_fail(ctx, "bytecode: an entry point takes no arguments"), v,
                       &saved, result);
    }
    bc_act *a = vm_activation(ctx, fn, NULL, NULL, 0);
    if (a == NULL) {
        filo_limits saved = ctx->paused_limits;
        filo_value v = {0};
        return run_end(ctx, FILO_ERR, v, &saved, result);
    }
    return bc_go_on(ctx, a, a, budget, result);
}

int filo_bc_resume(filo_ctx *ctx, uint32_t budget, filo_value *result) {
    clear_error(ctx);
    if (ctx->paused == NULL) {
        return filo_fail(ctx, "bytecode: no run is paused");
    }
    bc_act *start = ctx->paused;
    ctx->paused = NULL;
    return bc_go_on(ctx, ctx->paused_base, start, budget, result);
}

const char *filo_error(const filo_ctx *ctx) {
    return ctx->error;
}

bool filo_error_at(const filo_ctx *ctx, uint32_t *line, uint32_t *col) {
    if (ctx->error_line == 0) {
        return false;
    }
    *line = ctx->error_line;
    *col = ctx->error_col;
    return true;
}

/* --------------------------------------------------------------- builtins */

static int as_num(filo_ctx *ctx, const filo_value *v, double *out) {
    if (v->kind != FILO_NUMBER) {
        return fail_expected(ctx, "number", v);
    }
    *out = v->u.num;
    return FILO_OK;
}

static int as_list(filo_ctx *ctx, const filo_value *v, filo_seq *out) {
    if (v->kind != FILO_LIST) {
        return fail_expected(ctx, "list", v);
    }
    *out = v->u.seq;
    return FILO_OK;
}

static int b_add(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    double sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        double x = 0;
        if (as_num(ctx, &args[i], &x) != FILO_OK) {
            return FILO_ERR;
        }
        sum += x;
    }
    *out = filo_num(sum);
    return FILO_OK;
}

static int b_sub(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n == 0) {
        return filo_fail(ctx, "- expects at least 1 argument");
    }
    double r = 0;
    if (as_num(ctx, &args[0], &r) != FILO_OK) {
        return FILO_ERR;
    }
    if (n == 1) {
        *out = filo_num(-r);
        return FILO_OK;
    }
    for (uint32_t i = 1; i < n; i++) {
        double x = 0;
        if (as_num(ctx, &args[i], &x) != FILO_OK) {
            return FILO_ERR;
        }
        r -= x;
    }
    *out = filo_num(r);
    return FILO_OK;
}

static int b_mul(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    double r = 1;
    for (uint32_t i = 0; i < n; i++) {
        double x = 0;
        if (as_num(ctx, &args[i], &x) != FILO_OK) {
            return FILO_ERR;
        }
        r *= x;
    }
    *out = filo_num(r);
    return FILO_OK;
}

static int b_div(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n == 0) {
        return filo_fail(ctx, "/ expects at least 1 argument");
    }
    double r = 0;
    if (as_num(ctx, &args[0], &r) != FILO_OK) {
        return FILO_ERR;
    }
    if (n == 1) {
        /* reciprocal, the counterpart of (- x) being negation */
        if (r == 0) {
            return filo_fail(ctx, "division by zero");
        }
        *out = filo_num(1 / r);
        return FILO_OK;
    }
    for (uint32_t i = 1; i < n; i++) {
        double x = 0;
        if (as_num(ctx, &args[i], &x) != FILO_OK) {
            return FILO_ERR;
        }
        if (x == 0) {
            return filo_fail(ctx, "division by zero");
        }
        r /= x;
    }
    *out = filo_num(r);
    return FILO_OK;
}

/* fmod without libm, and exact. The remainder of two doubles is always
   representable, so there is one right answer and any exact method gives the
   one Go's math.Mod gives: this one is long division on the significands as
   integers, a bit at a time. The shortcut it replaced, a - trunc(a/b)*b
   through an int64, was undefined past a quotient of 2^63 and inexact past
   2^53. Special cases as Go: an infinite or NaN dividend or a NaN divisor
   gives NaN, an infinite divisor gives the dividend back. */
static double fmod_exact(double a, double b) {
    const uint64_t sign_bit = 1ULL << 63U;
    const uint64_t implicit = 1ULL << 52U;
    const uint64_t inf = 0x7FFULL << 52U;
    uint64_t ua = 0;
    uint64_t ub = 0;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    uint64_t sign = ua & sign_bit;
    uint64_t ma = ua & ~sign_bit; /* magnitudes: for these, bit order is value order */
    uint64_t mb = ub & ~sign_bit;
    if (ma >= inf || mb > inf || mb == 0) {
        uint64_t nan_bits = 0x7FF8000000000000ULL;
        double nan = 0;
        memcpy(&nan, &nan_bits, sizeof(nan));
        return nan;
    }
    if (ma < mb) {
        return a; /* an infinite divisor lands here too */
    }
    uint64_t r = sign;
    if (ma != mb) {
        int ea = (int)(ma >> 52U);
        int eb = (int)(mb >> 52U);
        uint64_t fa = ma & (implicit - 1);
        uint64_t fb = mb & (implicit - 1);
        /* the leading one at bit 52, subnormals included */
        if (ea == 0) {
            ea = 1;
            while (fa < implicit) {
                fa <<= 1U;
                ea--;
            }
        } else {
            fa |= implicit;
        }
        if (eb == 0) {
            eb = 1;
            while (fb < implicit) {
                fb <<= 1U;
                eb--;
            }
        } else {
            fb |= implicit;
        }
        while (ea > eb) {
            if (fa >= fb) {
                fa -= fb;
            }
            fa <<= 1U; /* fa < 2*fb < 2^54 throughout */
            ea--;
        }
        if (fa >= fb) {
            fa -= fb;
        }
        if (fa != 0) {
            while (fa < implicit) {
                fa <<= 1U;
                ea--;
            }
            if (ea > 0) {
                r |= ((uint64_t)ea << 52U) | (fa & (implicit - 1));
            } else {
                r |= fa >> (unsigned)(1 - ea); /* subnormal: the bits shifted out are zero */
            }
        }
    }
    double out = 0;
    memcpy(&out, &r, sizeof(out));
    return out;
}

static int b_mod(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 2) {
        return filo_fail(ctx, "% expects 2 arguments");
    }
    double a = 0;
    double b = 0;
    if (as_num(ctx, &args[0], &a) != FILO_OK || as_num(ctx, &args[1], &b) != FILO_OK) {
        return FILO_ERR;
    }
    if (b == 0) {
        return filo_fail(ctx, "modulo by zero");
    }
    /* floored, as in Lua: the result takes the divisor's sign */
    double r = fmod_exact(a, b);
    if (r != 0 && (r < 0) != (b < 0)) {
        r += b;
    }
    *out = filo_num(r);
    return FILO_OK;
}

/* pow with an integral exponent: the same double on every machine and in
   the Go engine, which computes it step for step the same way (powInt in
   pow.go). The C library's pow differs between machines and Go's is not
   correctly rounded; a program is the same bytes everywhere only if its
   folded constants are, and the same result only if pow is.

   The mantissa is raised in double-double (a pair of doubles, about 106
   bits) by squaring, with the exponent kept apart in an integer, so no step
   leaves the range of the doubles; the power of two is put back once, at
   the end, with one rounding. Every product goes through rmul, which rounds
   it where it is written: a compiler may fuse a multiply and an add, which
   rounds once where these steps round twice. */
static double rmul(double a, double b) {
    volatile double p = a * b;
    return p;
}

static double bits_double(uint64_t bits) {
    double x = 0;
    memcpy(&x, &bits, sizeof(x));
    return x;
}

static uint64_t double_bits(double x) {
    uint64_t bits = 0;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

/* 2^k, for k from -1022 to 1023 */
static double pow2(int32_t k) {
    return bits_double((uint64_t)(k + 1023) << 52U);
}

/* x = f·2^e with f in [0.5, 1), for a finite x other than 0 */
static double frexp_bits(double x, int32_t *e) {
    uint64_t bits = double_bits(x);
    int32_t exp = (int32_t)((bits >> 52U) & 0x7FFU);
    int32_t adjust = 0;
    if (exp == 0) { /* subnormal: made normal first, exactly */
        x = rmul(x, pow2(54));
        bits = double_bits(x);
        exp = (int32_t)((bits >> 52U) & 0x7FFU);
        adjust = -54;
    }
    *e = exp - 1022 + adjust;
    return bits_double((bits & ~((uint64_t)0x7FFU << 52U)) | ((uint64_t)1022U << 52U));
}

/* a*b rounded, and what the rounding lost, exactly: by an fma where the
   machine has one, by Dekker's splitting where not, the same two doubles
   either way. The operands are mantissas, near 1: nothing overflows or
   underflows. */
#ifdef __FP_FAST_FMA
static double two_prod(double a, double b, double *e) {
    double p = rmul(a, b);
    *e = __builtin_fma(a, b, -p);
    return p;
}
#else
/* a as two halves of 26 bits, whose products are exact */
static void dd_split(double a, double *h, double *l) {
    double c = rmul(134217729.0, a); /* 2^27 + 1 */
    *h = c - (c - a);
    *l = a - *h;
}

static double two_prod(double a, double b, double *e) {
    double p = rmul(a, b);
    double ah = 0;
    double al = 0;
    double bh = 0;
    double bl = 0;
    dd_split(a, &ah, &al);
    dd_split(b, &bh, &bl);
    *e = (((rmul(ah, bh) - p) + rmul(ah, bl)) + rmul(al, bh)) + rmul(al, bl);
    return p;
}
#endif

/* a+b rounded, and what the rounding lost */
static double two_sum(double a, double b, double *e) {
    double s = a + b;
    double bb = s - a;
    *e = (a - (s - bb)) + (b - bb);
    return s;
}

static double dd_mul(double xh, double xl, double yh, double yl, double *l) {
    double e = 0;
    double p = two_prod(xh, yh, &e);
    e = e + (rmul(xh, yl) + rmul(xl, yh));
    return two_sum(p, e, l);
}

/* 1/(h+l): the quotient of the high part, corrected by its remainder */
static double dd_recip(double h, double l, double *out_l) {
    double q = 1 / h;
    double pe = 0;
    double p = two_prod(q, h, &pe);
    double rem = ((1 - p) - pe) - rmul(q, l);
    return two_sum(q, rem / h, out_l);
}

/* h into [0.5, 1), l with it, and *e by as much. h is a product or a
   quotient of mantissas, never subnormal: frexp_bits without that case.
   Scaling l by a power of two is exact, fused or not. */
static double dd_norm(double h, double *l, int32_t *e) {
    uint64_t bits = double_bits(h);
    int32_t k = (int32_t)((bits >> 52U) & 0x7FFU) - 1022;
    *l = *l * pow2(-k);
    *e += k;
    return bits_double((bits & ~((uint64_t)0x7FFU << 52U)) | ((uint64_t)1022U << 52U));
}

/* s·2^e for s in [0.5, 1], rounded once: a power of two within the normal
   doubles scales exactly, and only the last step can round */
static double scale2(double s, int32_t e) {
    if (e > 2046) {
        return bits_double(0x7FF0000000000000ULL);
    }
    if (e > 1023) {
        return rmul(rmul(s, pow2(1023)), pow2(e - 1023));
    }
    if (e >= -1022) {
        return rmul(s, pow2(e));
    }
    if (e >= -2022) {
        return rmul(rmul(s, pow2(-1000)), pow2(e + 1000));
    }
    return 0;
}

/* ±0 or ±Inf raised to an integral power, as IEEE 754's pow */
static double pow_edge(double a, bool neg, bool odd) {
    bool zero = false;
    if (a == 0) {
        zero = true;
    }
    if (neg) { /* 0 to a negative power is infinite, and infinity's is 0 */
        if (zero) {
            zero = false;
        } else {
            zero = true;
        }
    }
    double r = zero ? 0 : bits_double(0x7FF0000000000000ULL);
    if (odd && (double_bits(a) >> 63U) != 0) {
        return -r;
    }
    return r;
}

/* b is a whole number (every double past 2^52 is) */
static bool is_whole(double b) {
    if (b - b != 0) { /* NaN or ±Inf */
        return false;
    }
    double m = b < 0 ? -b : b;
    if (m >= 4503599627370496.0 || b == (double)(int64_t)b) {
        return true;
    }
    return false;
}

static double pow_int(double a, double b) {
    if (b == 0) {
        return 1;
    }
    if (a != a) {
        return a;
    }
    bool neg = b < 0;
    double n = neg ? -b : b;
    bool odd = false; /* past 2^53 every double is even */
    if (n < 9007199254740992.0 && ((uint64_t)n & 1U) != 0) {
        odd = true;
    }
    if (n > 4611686018427387904.0) {
        n = 4611686018427387904.0; /* 2^62: as large and even, the result is 0, 1 or +Inf all the
                                      same */
    }
    if (a == 0 || a - a != 0) {
        return pow_edge(a, neg, odd);
    }
    int32_t k = 0;
    double m = frexp_bits(a < 0 ? -a : a, &k);
    double rh = 1;
    double rl = 0;
    int32_t re = 0;
    double bh = m;
    double bl = 0;
    int32_t be = k;
    for (uint64_t e = (uint64_t)n;;) {
        if ((e & 1U) != 0) {
            rh = dd_mul(rh, rl, bh, bl, &rl);
            re += be;
            rh = dd_norm(rh, &rl, &re);
        }
        e >>= 1U;
        if (e == 0) {
            break;
        }
        if (be > 4096 || be < -4096) {
            /* every factor is a power of |a|, all above 1 or all below: one
               this far out decides the result, past any double */
            re = be;
            break;
        }
        bh = dd_mul(bh, bl, bh, bl, &bl);
        be *= 2;
        bh = dd_norm(bh, &bl, &be);
    }
    if (neg) {
        rh = dd_recip(rh, rl, &rl);
        re = -re;
        rh = dd_norm(rh, &rl, &re);
    }
    double r = scale2(rh + rl, re);
    if (a < 0 && odd) {
        return -r;
    }
    return r;
}

/* pow with a fractional exponent is the host's: without a C library it is
   NaN (the libc host installs pow, which differs from Go's in the last
   bits there, as the corpus's host-pow cases allow) */
static double core_pow(double a, double b) {
    (void)a;
    (void)b;
    return __builtin_nan("");
}

static double (*pow_hook)(double, double) = core_pow;

static int b_pow(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 2) {
        return filo_fail(ctx, "pow expects 2 arguments");
    }
    double a = 0;
    double b = 0;
    if (as_num(ctx, &args[0], &a) != FILO_OK || as_num(ctx, &args[1], &b) != FILO_OK) {
        return FILO_ERR;
    }
    *out = filo_num(is_whole(b) ? pow_int(a, b) : pow_hook(a, b)); /* the same double everywhere */
    return FILO_OK;
}

void filo_set_pow(double (*fn)(double, double)) {
    pow_hook = fn != NULL ? fn : core_pow;
}

/* filo_equal, walking both values together and stopping at the walk
   ceilings on the parts it compares: a comparison settled early costs what
   it walked, whatever the size of the rest. The Go engine (equalWalk)
   counts the same parts in the same order. */
static int equal_walk(filo_ctx *ctx, const filo_value *a, const filo_value *b, uint32_t level,
                      uint32_t *parts, bool *eq) {
    *eq = false;
    (*parts)++;
    if (*parts > WALK_PARTS_MAX) {
        return filo_fail(ctx, "value too large: more than 4194304 parts");
    }
    if (a->kind != b->kind) {
        return FILO_OK;
    }
    if (a->kind != FILO_LIST && a->kind != FILO_TUPLE) {
        *eq = filo_equal(a, b);
        return FILO_OK;
    }
    if (a->u.seq.len != b->u.seq.len) {
        return FILO_OK;
    }
    if (level > WALK_LEVELS_MAX) {
        return filo_fail(ctx, "value too deep: more than 512 levels");
    }
    for (uint32_t i = 0; i < a->u.seq.len; i++) {
        filo_value av = seq_at(&a->u.seq, i);
        filo_value bv = seq_at(&b->u.seq, i);
        if (equal_walk(ctx, &av, &bv, level + 1U, parts, eq) != FILO_OK) {
            return FILO_ERR;
        }
        if (!*eq) {
            return FILO_OK;
        }
    }
    *eq = true;
    return FILO_OK;
}

static int b_eq(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n < 2) {
        return filo_fail(ctx, "= expects at least 2 arguments");
    }
    for (uint32_t i = 1; i < n; i++) {
        if (args[i].kind != args[0].kind) {
            return filo_fail(ctx, "expected values of the same kind");
        }
    }
    for (uint32_t i = 1; i < n; i++) {
        uint32_t parts = 0;
        bool eq = false;
        if (equal_walk(ctx, &args[0], &args[i], 1, &parts, &eq) != FILO_OK) {
            return FILO_ERR;
        }
        if (!eq) {
            *out = filo_bool(false);
            return FILO_OK;
        }
    }
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_ne(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (b_eq(ctx, args, n, out) != FILO_OK) {
        return FILO_ERR;
    }
    bool equal = out->u.b;
    out->u.b = true;
    if (equal) {
        out->u.b = false;
    }
    return FILO_OK;
}

typedef enum { CMP_LT, CMP_LE, CMP_GT, CMP_GE } cmp_op;

static int compare_chain(filo_ctx *ctx, const filo_value *args, uint32_t n, cmp_op op,
                         const char *arity_msg, filo_value *out) {
    if (n < 2) {
        return filo_fail(ctx, arity_msg);
    }
    for (uint32_t i = 1; i < n; i++) {
        double l = 0;
        double r = 0;
        if (as_num(ctx, &args[i - 1], &l) != FILO_OK || as_num(ctx, &args[i], &r) != FILO_OK) {
            return FILO_ERR;
        }
        bool ok = false;
        switch (op) {
        case CMP_LT:
            ok = l < r;
            break;
        case CMP_LE:
            ok = l <= r;
            break;
        case CMP_GT:
            ok = l > r;
            break;
        default:
            ok = l >= r;
            break;
        }
        if (!ok) {
            *out = filo_bool(false);
            return FILO_OK;
        }
    }
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_lt(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return compare_chain(ctx, a, n, CMP_LT, "< expects at least 2 arguments", out);
}

static int b_le(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return compare_chain(ctx, a, n, CMP_LE, "<= expects at least 2 arguments", out);
}

static int b_gt(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return compare_chain(ctx, a, n, CMP_GT, "> expects at least 2 arguments", out);
}

static int b_ge(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return compare_chain(ctx, a, n, CMP_GE, ">= expects at least 2 arguments", out);
}

static int b_not(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "not expects 1 argument");
    }
    bool b = false;
    if (as_bool(ctx, &args[0], &b) != FILO_OK) {
        return FILO_ERR;
    }
    bool negated = true;
    if (b) {
        negated = false;
    }
    *out = filo_bool(negated);
    return FILO_OK;
}

static int b_string(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "string expects 1 argument");
    }
    return render_to_value(ctx, &args[0], out);
}

static bool is_space(uint8_t c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
        return true;
    }
    return false;
}

static int b_number(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "number expects 1 argument");
    }
    const filo_value *v = &args[0];
    if (v->kind == FILO_NUMBER) {
        *out = *v;
        return FILO_OK;
    }
    if (v->kind != FILO_STRING) {
        return filo_fail2(ctx, "number expects a number or a numeric string, got ",
                          filo_kind_name(v->kind));
    }
    const uint8_t *s = v->u.str.ptr;
    uint32_t len = v->u.str.len;
    while (len > 0 && is_space(s[0])) {
        s++;
        len--;
    }
    while (len > 0 && is_space(s[len - 1])) {
        len--;
    }
    double x = 0;
    if (ctx->host.str_to_num == NULL || !ctx->host.str_to_num(ctx->host.user, s, len, &x)) {
        return filo_fail(ctx, "number: cannot parse");
    }
    *out = filo_num(x);
    return FILO_OK;
}

static int b_type_of(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "type-of expects 1 argument");
    }
    *out = filo_cstring(filo_kind_name(args[0].kind));
    return FILO_OK;
}

static int b_is_empty(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "is-empty expects 1 argument");
    }
    const filo_value *v = &args[0];
    bool empty = false;
    if (v->kind == FILO_STRING) {
        empty = v->u.str.len == 0;
    }
    if (v->kind == FILO_LIST || v->kind == FILO_TUPLE) {
        empty = v->u.seq.len == 0;
    }
    *out = filo_bool(empty);
    return FILO_OK;
}

static int b_is_nil(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "is-nil expects 1 argument");
    }
    bool nil = false;
    if (args[0].kind == FILO_LIST && args[0].u.seq.len == 0) {
        nil = true;
    }
    *out = filo_bool(nil);
    return FILO_OK;
}

static int b_list(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    return filo_list(ctx, args, n, out);
}

static int b_length(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "length expects 1 argument");
    }
    if (args[0].kind != FILO_LIST && args[0].kind != FILO_TUPLE) {
        return filo_fail(ctx, "length expects list or tuple");
    }
    *out = filo_num((double)args[0].u.seq.len);
    return FILO_OK;
}

static int b_head(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "head expects 1 argument");
    }
    filo_seq l;
    if (as_list(ctx, &args[0], &l) != FILO_OK) {
        return FILO_ERR;
    }
    if (l.len == 0) {
        return filo_fail(ctx, "head of empty list");
    }
    *out = seq_at(&l, 0);
    return FILO_OK;
}

static int b_tail(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "tail expects 1 argument");
    }
    filo_seq l;
    if (as_list(ctx, &args[0], &l) != FILO_OK) {
        return FILO_ERR;
    }
    if (l.len == 0) {
        return filo_fail(ctx, "tail of empty list");
    }
    const filo_value *items = seq_items(ctx, &l);
    if (items == NULL) {
        return FILO_ERR;
    }
    return filo_list(ctx, items + 1, l.len - 1, out);
}

/* Integral and exactly representable (|x| <= 2^53); NaN and infinities fail,
   as does anything a 64-bit cast could not round-trip. */
static bool is_integral(double x) {
    if (!(x >= -9007199254740992.0 && x <= 9007199254740992.0)) {
        return false;
    }
    return (double)(int64_t)x == x;
}

static int b_nth(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 2) {
        return filo_fail(ctx, "nth expects 2 arguments");
    }
    filo_seq l;
    double idx = 0;
    if (as_list(ctx, &args[0], &l) != FILO_OK || as_num(ctx, &args[1], &idx) != FILO_OK) {
        return FILO_ERR;
    }
    if (!is_integral(idx)) {
        return filo_fail(ctx, "nth expects an integer index");
    }
    if (idx < 0 || idx >= (double)l.len) {
        return filo_fail(ctx, "index out of range");
    }
    *out = seq_at(&l, (uint32_t)idx);
    return FILO_OK;
}

static int b_list_append(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 2) {
        return filo_fail(ctx, "list-append expects 2 arguments (list, value)");
    }
    filo_seq l;
    if (as_list(ctx, &args[0], &l) != FILO_OK) {
        return prefix_error(ctx, "list-append: first argument must be list: ");
    }
    if (make_seq(ctx, FILO_LIST, NULL, l.len + 1, out) != FILO_OK) {
        return FILO_ERR;
    }
    if (l.len > 0) {
        for (uint32_t i = 0; i < l.len; i++) {
            out->u.seq.items[i] = seq_at(&l, i);
        }
    }
    out->u.seq.items[l.len] = args[1];
    return materialise(ctx, &out->u.seq.items[l.len]);
}

static int b_list_concat(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n < 2) {
        return filo_fail(ctx, "list-concat expects at least 2 arguments");
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (args[i].kind != FILO_LIST) {
            char msg[64];
            size_t k = cstr_copy(msg, sizeof(msg), "list-concat: argument ");
            k += u32_text(msg + k, sizeof(msg) - k, i);
            (void)cstr_copy(msg + k, sizeof(msg) - k, " is not a list");
            return filo_fail(ctx, msg);
        }
        total += args[i].u.seq.len;
    }
    if (make_seq(ctx, FILO_LIST, NULL, total, out) != FILO_OK) {
        return FILO_ERR;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (args[i].u.seq.len > 0) {
            const filo_value *from = seq_items(ctx, &args[i].u.seq);
            if (from == NULL) {
                return FILO_ERR;
            }
            memcpy(out->u.seq.items + at, from, sizeof(filo_value) * args[i].u.seq.len);
            at += args[i].u.seq.len;
        }
    }
    return FILO_OK;
}

static int b_map(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 2) {
        return filo_fail(ctx, "map expects function and list");
    }
    if (args[0].kind != FILO_FUNC) {
        return filo_fail(ctx, "map expects function as first argument");
    }
    filo_seq l;
    if (as_list(ctx, &args[1], &l) != FILO_OK) {
        return FILO_ERR;
    }
    if (make_seq(ctx, FILO_LIST, NULL, l.len, out) != FILO_OK) {
        return FILO_ERR;
    }
    for (uint32_t i = 0; i < l.len; i++) {
        filo_value item = seq_at(&l, i);
        if (filo_call(ctx, &args[0], &item, 1, &out->u.seq.items[i]) != FILO_OK) {
            return FILO_ERR;
        }
        if (materialise(ctx, &out->u.seq.items[i]) != FILO_OK) {
            return FILO_ERR;
        }
    }
    return FILO_OK;
}

static int b_fold(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 3) {
        return filo_fail(ctx, "fold expects function, initial value, and list");
    }
    if (args[0].kind != FILO_FUNC) {
        return filo_fail(ctx, "fold expects function as first argument");
    }
    filo_seq l;
    if (as_list(ctx, &args[2], &l) != FILO_OK) {
        return FILO_ERR;
    }
    filo_value acc = args[1];
    for (uint32_t i = 0; i < l.len; i++) {
        filo_value pair[2] = {acc, seq_at(&l, i)};
        if (filo_call(ctx, &args[0], pair, 2, &acc) != FILO_OK) {
            return FILO_ERR;
        }
    }
    *out = acc;
    return FILO_OK;
}

static int b_filter(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 2) {
        return filo_fail(ctx, "filter expects function and list");
    }
    if (args[0].kind != FILO_FUNC) {
        return filo_fail(ctx, "filter expects function as first argument");
    }
    filo_seq l;
    if (as_list(ctx, &args[1], &l) != FILO_OK) {
        return FILO_ERR;
    }
    if (make_seq(ctx, FILO_LIST, NULL, l.len, out) != FILO_OK) {
        return FILO_ERR;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < l.len; i++) {
        filo_value v = {0};
        filo_value item = seq_at(&l, i);
        if (filo_call(ctx, &args[0], &item, 1, &v) != FILO_OK) {
            return FILO_ERR;
        }
        bool keep = false;
        if (as_bool(ctx, &v, &keep) != FILO_OK) {
            return prefix_error(ctx, "filter predicate must return a bool: ");
        }
        if (keep) {
            out->u.seq.items[kept] = seq_at(&l, i);
            kept++;
        }
    }
    out->u.seq.len = kept;
    return FILO_OK;
}

static int b_reverse(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "reverse expects 1 argument");
    }
    filo_seq l;
    if (as_list(ctx, &args[0], &l) != FILO_OK) {
        return FILO_ERR;
    }
    if (make_seq(ctx, FILO_LIST, NULL, l.len, out) != FILO_OK) {
        return FILO_ERR;
    }
    for (uint32_t i = 0; i < l.len; i++) {
        out->u.seq.items[l.len - 1 - i] = seq_at(&l, i);
    }
    return FILO_OK;
}

static int b_range(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    if (n != 1 && n != 2) {
        return filo_fail(ctx, "range expects 1 or 2 arguments");
    }
    double start = 0;
    double end = 0;
    if (n == 1) {
        if (as_num(ctx, &args[0], &end) != FILO_OK) {
            return FILO_ERR;
        }
    } else if (as_num(ctx, &args[0], &start) != FILO_OK || as_num(ctx, &args[1], &end) != FILO_OK) {
        return FILO_ERR;
    }
    if (!is_integral(start) || !is_integral(end)) {
        return filo_fail(ctx, "range expects integer bounds");
    }
    int64_t lo = (int64_t)start;
    int64_t hi = (int64_t)end;
    if (hi <= lo) {
        *out = empty_list();
        return FILO_OK;
    }
    if (hi - lo > (int64_t)FILO_RANGE_MAX) {
        return filo_fail(ctx, "range too large");
    }
    uint32_t count = (uint32_t)(hi - lo);
    if (lo == 0) {
        memset(out, 0, sizeof(*out));
        out->kind = FILO_LIST;
        out->u.seq.items = NULL; /* the integers 0..count-1, holding no memory */
        out->u.seq.len = count;
        return FILO_OK;
    }
    if (make_seq(ctx, FILO_LIST, NULL, count, out) != FILO_OK) {
        return FILO_ERR;
    }
    for (uint32_t i = 0; i < count; i++) {
        out->u.seq.items[i] = filo_num((double)(lo + (int64_t)i));
    }
    return FILO_OK;
}

static int b_error(filo_ctx *ctx, const filo_value *args, uint32_t n, filo_value *out) {
    (void)out;
    if (n != 1) {
        return filo_fail(ctx, "error expects 1 argument (a message string)");
    }
    if (args[0].kind != FILO_STRING) {
        (void)fail_expected(ctx, "string", &args[0]);
        return prefix_error(ctx, "error expects a string message: ");
    }
    /* the message is script text: bounded copy, no format directives */
    size_t len = args[0].u.str.len;
    if (len > sizeof(ctx->error) - 1) {
        len = sizeof(ctx->error) - 1;
    }
    memcpy(ctx->error, args[0].u.str.ptr, len);
    ctx->error[len] = '\0';
    return FILO_ERR;
}

static void register_core(filo_ctx *ctx) {
    static const struct {
        const char *name;
        filo_builtin fn;
    } core[] = {
        {"+", b_add},
        {"-", b_sub},
        {"*", b_mul},
        {"/", b_div},
        {"%", b_mod},
        {"pow", b_pow},
        {"=", b_eq},
        {"!=", b_ne},
        {"<", b_lt},
        {"<=", b_le},
        {">", b_gt},
        {">=", b_ge},
        {"not", b_not},
        {"string", b_string},
        {"number", b_number},
        {"type-of", b_type_of},
        {"is-empty", b_is_empty},
        {"is-nil", b_is_nil},
        {"list", b_list},
        {"length", b_length},
        {"head", b_head},
        {"tail", b_tail},
        {"nth", b_nth},
        {"list-append", b_list_append},
        {"list-concat", b_list_concat},
        {"map", b_map},
        {"fold", b_fold},
        {"filter", b_filter},
        {"reverse", b_reverse},
        {"range", b_range},
        {"error", b_error},
    };
    for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); i++) {
        (void)filo_register_builtin(ctx, core[i].name, core[i].fn);
    }
}

/* ---------------------------------------------------------- for builtins */

int filo_arg_num(filo_ctx *ctx, const filo_value *v, double *out) {
    return as_num(ctx, v, out);
}

int filo_arg_str(filo_ctx *ctx, const filo_value *v, filo_str *out) {
    if (v->kind != FILO_STRING) {
        return fail_expected(ctx, "string", v);
    }
    *out = v->u.str;
    return FILO_OK;
}

int filo_arg_list(filo_ctx *ctx, const filo_value *v, filo_seq *out) {
    int rc = as_list(ctx, v, out);
    if (rc != FILO_OK) {
        return rc;
    }
    if (out->items == NULL && out->len > 0) {
        filo_value *items = seq_items(ctx, out); /* the host gets a real vector */
        if (items == NULL) {
            return FILO_ERR;
        }
        out->items = items;
    }
    return FILO_OK;
}

void *filo_alloc(filo_ctx *ctx, size_t n) {
    return ralloc(ctx, n);
}
