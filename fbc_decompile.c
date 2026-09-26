/* fbc_decompile.h's decompiler, step for step the Go engine's (package fbc,
   decompile.go): the same shapes read the same way, so the two write the
   same text, and make govm holds them to it on every unit of the corpus. */
#include "fbc_decompile.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    OP_PUSH_K,
    OP_PUSH_G,
    OP_STORE_G,
    OP_PUSH_L,
    OP_STORE_L,
    OP_PUSH_UP,
    OP_STORE_UP,
    OP_POP,
    OP_JMP,
    OP_CALL,
    OP_CALLB,
    OP_RET,
    OP_CLOSURE,
    OP_TUPLE,
    OP_UNPACK,
    OP_TRAP,
    OP_PUSH_B,
    OP_COUNT,
};

enum { DEPTH_MAX = 1024 }; /* regions inside regions, closures inside closures */

/* A form: an atom when kids is NULL, else a list of forms. The flags say
   what the shapes around it need to know. */
typedef struct dnode dnode;
struct dnode {
    const char *atom;
    size_t alen;
    dnode **kids;
    uint32_t nkids;
    bool lit;      /* a constant the folder takes as a literal */
    bool empty;    /* the empty-list constant: (cond), what an if without else gives */
    bool isdo;     /* a (do ...): its forms spread into the body around it */
    bool isif;     /* (if test then [else]) */
    uint8_t chain; /* 2 and, 3 or: the operands of the jumps to out */
    uint32_t out;
    bool check; /* the last operand of an and or an or (JMP 4) */
};

enum { STMT, BIND, UNPACK };

/* What a sequence ran before the value it leads to. */
typedef struct {
    uint8_t kind;
    dnode *v;      /* the statement, the (name value) binding, or (tuple names) */
    uint32_t slot; /* a binding's, an unpack's lowest */
} dstep;

typedef struct {
    dstep *p;
    uint32_t n;
} dsteps;

typedef struct {
    dnode *n;
    dsteps pre;
} dentry;

typedef struct {
    const fbc_unit *u;
    uint8_t *mem;
    size_t cap;
    size_t used;
    fbc_folds folds;
    void *fold_user;
    char *why;
    size_t whycap;
    bool failed;
    int32_t parent[FBC_FNS_MAX];
    char **names[FBC_FNS_MAX];
    uint32_t depth;
} dc;

typedef struct {
    uint32_t fn;
    bool *declared; /* a slot bound by a let so far */
    uint32_t nslots;
} fnstate;

static bool fail(dc *d, const char *msg) {
    if (!d->failed && d->why != NULL && d->whycap > 0) {
        (void)snprintf(d->why, d->whycap, "%s", msg);
    }
    d->failed = true;
    return false;
}

/* Whether the work goes on: nothing has failed yet. */
static bool alive(const dc *d) {
    if (d->failed) {
        return false;
    }
    return true;
}

static void *take(dc *d, size_t n) {
    size_t at = (d->used + 7U) & ~(size_t)7U;
    if (d->failed || at > d->cap || n > d->cap - at) {
        (void)fail(d, "decompiling needs more memory than it was given");
        return NULL;
    }
    d->used = at + n;
    memset(d->mem + at, 0, n);
    return d->mem + at;
}

static dnode *atom(dc *d, const char *s, size_t n) {
    dnode *a = take(d, sizeof(dnode));
    if (a != NULL) {
        a->atom = s;
        a->alen = n;
    }
    return a;
}

static dnode *atomz(dc *d, const char *s) {
    return atom(d, s, strlen(s));
}

/* A copy of s in the arena, as an atom. */
static dnode *atom_copy(dc *d, const char *s, size_t n) {
    char *c = take(d, n + 1);
    if (c == NULL) {
        return NULL;
    }
    memcpy(c, s, n);
    return atom(d, c, n);
}

/* (head kids...), n of them. */
static dnode *list(dc *d, const char *head, dnode **kids, uint32_t n) {
    dnode *l = take(d, sizeof(dnode));
    dnode **k = (dnode **)take(d, sizeof(dnode *) * (n + 1U));
    if (l == NULL || k == NULL) {
        return NULL;
    }
    k[0] = atomz(d, head);
    if (n > 0) {
        memcpy((void *)(k + 1), (const void *)kids, sizeof(dnode *) * n);
    }
    l->kids = k;
    l->nkids = n + 1U;
    return l;
}

static dnode *list1(dc *d, const char *head, dnode *a) {
    return list(d, head, &a, 1);
}

static dnode *list2(dc *d, const char *head, dnode *a, dnode *b) {
    dnode *k[2] = {a, b};
    return list(d, head, k, 2);
}

/* A list of kids as they are, no head. */
static dnode *bare(dc *d, dnode **kids, uint32_t n) {
    dnode *l = take(d, sizeof(dnode));
    dnode **k = (dnode **)take(d, sizeof(dnode *) * (n > 0 ? n : 1U));
    if (l == NULL || k == NULL) {
        return NULL;
    }
    if (n > 0) {
        memcpy((void *)k, (const void *)kids, sizeof(dnode *) * n);
    }
    l->kids = k;
    l->nkids = n;
    return l;
}

/* ---- the text ---- */

typedef struct {
    dc *d;
    char *p;
    size_t n;
    size_t cap;
} text;

static void put(text *t, const char *s, size_t n) {
    if (t->n + n > t->cap) {
        size_t cap = (t->cap + n) * 2U;
        char *p = take(t->d, cap);
        if (p == NULL) {
            return;
        }
        if (t->n > 0) {
            memcpy(p, t->p, t->n);
        }
        t->p = p;
        t->cap = cap;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
}

static void write_node(text *t, const dnode *n);

static void write_list(text *t, const char *head, dnode *const *kids, uint32_t n) {
    put(t, "(", 1);
    put(t, head, strlen(head));
    for (uint32_t i = 0; i < n; i++) {
        put(t, " ", 1);
        write_node(t, kids[i]);
    }
    put(t, ")", 1);
}

/* A clause of a cond: the test (else when NULL), then the forms of what it
   runs. */
static void write_clause(text *t, const dnode *test, const dnode *run) {
    put(t, " (", 2);
    if (test != NULL) {
        write_node(t, test);
    } else {
        put(t, "else", 4);
    }
    if (run->isdo) {
        for (uint32_t i = 1; i < run->nkids; i++) {
            put(t, " ", 1);
            write_node(t, run->kids[i]);
        }
    } else {
        put(t, " ", 1);
        write_node(t, run);
    }
    put(t, ")", 1);
}

/* An if as if, or as cond: a chain reads better as one, and a literal test
   must be a cond, which the folder leaves alone where it would fold the if.
   Both are the same bytes. */
static void write_if(text *t, const dnode *n) {
    const dnode *test = n->kids[0];
    bool nested = false;
    if (n->nkids == 3) {
        nested = n->kids[2]->isif;
    }
    if (!test->lit && !nested) {
        write_list(t, "if", n->kids, n->nkids);
        return;
    }
    put(t, "(cond", 5);
    for (const dnode *at = n;;) {
        write_clause(t, at->kids[0], at->kids[1]);
        if (at->nkids < 3) {
            break;
        }
        const dnode *next = at->kids[2];
        if (!next->isif) {
            write_clause(t, NULL, next);
            break;
        }
        at = next;
    }
    put(t, ")", 1);
}

static void write_node(text *t, const dnode *n) {
    if (n->chain != 0 || n->check) {
        write_list(t, n->chain == 3 ? "or" : "and", n->kids, n->nkids);
        return;
    }
    if (n->isif) {
        write_if(t, n);
        return;
    }
    if (n->kids == NULL) {
        put(t, n->atom, n->alen);
        return;
    }
    put(t, "(", 1);
    for (uint32_t i = 0; i < n->nkids; i++) {
        if (i > 0) {
            put(t, " ", 1);
        }
        write_node(t, n->kids[i]);
    }
    put(t, ")", 1);
}

/* ---- sequences ---- */

static dsteps concat(dc *d, dsteps a, const dstep *b, uint32_t nb) {
    if (nb == 0) {
        return a;
    }
    dsteps c = {take(d, sizeof(dstep) * (a.n + nb)), a.n + nb};
    if (c.p == NULL) {
        return a;
    }
    if (a.n > 0) {
        memcpy(c.p, a.p, sizeof(dstep) * a.n);
    }
    memcpy(c.p + a.n, b, sizeof(dstep) * nb);
    return c;
}

typedef struct {
    dnode **p;
    uint32_t n;
    uint32_t cap;
} nodes;

static void add(dc *d, nodes *v, dnode *n) {
    if (v->n == v->cap) {
        uint32_t cap = v->cap == 0 ? 8U : v->cap * 2U;
        dnode **p = (dnode **)take(d, sizeof(dnode *) * cap);
        if (p == NULL) {
            return;
        }
        if (v->n > 0) {
            memcpy((void *)p, (const void *)v->p, sizeof(dnode *) * v->n);
        }
        v->p = p;
        v->cap = cap;
    }
    v->p[v->n++] = n;
}

/* The steps and then n as the forms of a body: statements as they are,
   bindings on consecutive slots as one let, a letv, each around what
   follows. Built from the end, the forms held reversed. */
static nodes seq_forms(dc *d, dsteps steps, dnode *n) {
    nodes rev = {0};
    if (n == NULL) {
        (void)fail(d, "a form that did not read");
        return rev;
    }
    if (n->isdo) {
        for (uint32_t i = n->nkids; i > 1; i--) {
            add(d, &rev, n->kids[i - 1]);
        }
    } else {
        add(d, &rev, n);
    }
    uint32_t *group = take(d, sizeof(uint32_t) * (steps.n + 1U)); /* a let's end, at its start */
    if (group == NULL) {
        return rev;
    }
    for (uint32_t i = 0; i < steps.n;) {
        uint32_t j = i + 1U;
        if (steps.p[i].kind == BIND) {
            while (j < steps.n && steps.p[j].kind == BIND &&
                   steps.p[j].slot == steps.p[i].slot + (j - i)) {
                j++;
            }
        }
        group[i] = j;
        i = j;
    }
    uint32_t *starts = take(d, sizeof(uint32_t) * (steps.n + 1U));
    uint32_t ngroups = 0;
    for (uint32_t i = 0; i < steps.n && starts != NULL; i = group[i]) {
        starts[ngroups++] = i;
    }
    for (uint32_t g = ngroups; g > 0 && !d->failed; g--) {
        uint32_t i = starts[g - 1U];
        const dstep *s = &steps.p[i];
        if (s->kind == STMT) {
            add(d, &rev, s->v);
            continue;
        }
        /* a let or a letv around everything after it */
        nodes kids = {0};
        if (s->kind == UNPACK) {
            add(d, &kids, s->v->kids[1]); /* the names */
            add(d, &kids, s->v->kids[0]); /* the tuple */
        } else {
            nodes binds = {0};
            for (uint32_t k = i; k < group[i]; k++) {
                add(d, &binds, steps.p[k].v);
            }
            add(d, &kids, bare(d, binds.p, binds.n));
        }
        for (uint32_t k = rev.n; k > 0; k--) {
            add(d, &kids, rev.p[k - 1U]);
        }
        rev.n = 0;
        add(d, &rev, list(d, s->kind == UNPACK ? "letv" : "let", kids.p, kids.n));
    }
    nodes out = {0};
    for (uint32_t k = rev.n; k > 0; k--) {
        add(d, &out, rev.p[k - 1U]);
    }
    return out;
}

/* e as one form: its steps around it. */
static dnode *wrap(dc *d, dentry e) {
    if (e.pre.n == 0) {
        return e.n;
    }
    nodes f = seq_forms(d, e.pre, e.n);
    if (f.n == 1) {
        return f.p[0];
    }
    dnode *l = list(d, "do", f.p, f.n);
    if (l != NULL) {
        l->isdo = true;
    }
    return l;
}

/* ---- names ---- */

static bool same(const uint8_t *a, size_t an, const char *b, size_t bn) {
    if (an != bn) {
        return false;
    }
    return memcmp(a, b, an) == 0;
}

static const char special_forms[] =
    " def fn let letv if cond else do set and or return exit values tuple list ";

/* Whether name is taken: a global, an import, a special form. */
static bool taken(const dc *d, const char *name, size_t n) {
    const fbc_unit *u = d->u;
    for (uint32_t i = 0; i < u->nimports; i++) {
        if (same(u->data + u->imports[i].off, u->imports[i].len, name, n)) {
            return true;
        }
    }
    for (uint32_t i = 0; i < u->nglobals; i++) {
        if (same(u->data + u->globals[i].off, u->globals[i].len, name, n)) {
            return true;
        }
    }
    for (const char *p = special_forms; *p != '\0';) {
        const char *e = strchr(p + 1, ' ');
        if (e == NULL) {
            break;
        }
        if ((size_t)(e - (p + 1)) == n && memcmp(p + 1, name, n) == 0) {
            return true;
        }
        p = e;
    }
    return false;
}

/* A local's name: slot s of function fn, a parameter or a binding. */
static const char *name_of(dc *d, uint32_t fn, uint32_t s) {
    const fbc_fn *f = &d->u->fns[fn];
    if (s >= f->slots) {
        (void)fail(d, "a slot past the frame");
        return "?";
    }
    if (d->names[fn] == NULL) {
        d->names[fn] = (char **)take(d, sizeof(char *) * (f->slots > 0 ? f->slots : 1U));
        if (d->names[fn] == NULL) {
            return "?";
        }
    }
    if (d->names[fn][s] != NULL) {
        return d->names[fn][s];
    }
    uint32_t level = 0;
    for (int32_t p = d->parent[fn]; p >= 0; p = d->parent[p]) {
        level++;
    }
    static const char params[] = "xyzuvw";
    static const char binds[] = "abcdefghijkmnopqrst";
    char n[48];
    size_t len = 0;
    if (s < f->params) {
        len = s < 6 ? (size_t)snprintf(n, sizeof(n), "%c", params[s])
                    : (size_t)snprintf(n, sizeof(n), "p%u_", s);
        if (level > 1) {
            len += (size_t)snprintf(n + len, sizeof(n) - len, "%u", level);
        }
    } else {
        uint32_t k = s - f->params;
        len = k < 19 ? (size_t)snprintf(n, sizeof(n), "%c", binds[k])
                     : (size_t)snprintf(n, sizeof(n), "l%u_", k);
        if (level > 0) {
            len += (size_t)snprintf(n + len, sizeof(n) - len, "%u", level);
        }
    }
    while (taken(d, n, len) && len + 2 < sizeof(n)) {
        n[len++] = '_';
        n[len] = '\0';
    }
    char *c = take(d, len + 1);
    if (c == NULL) {
        return "?";
    }
    memcpy(c, n, len + 1);
    d->names[fn][s] = c;
    return c;
}

static int32_t ancestor(const dc *d, uint32_t fn, uint32_t depth) {
    int32_t f = (int32_t)fn;
    for (; depth > 0 && f >= 0; depth--) {
        f = d->parent[f];
    }
    return f;
}

/* ---- instructions ---- */

typedef struct {
    uint32_t op;
    uint32_t x; /* the first operand, a jump's condition */
    uint32_t y; /* the second: a slot, an import, a jump's target */
    uint32_t len;
} insn;

static bool uleb(const dc *d, uint32_t *at, uint32_t end, uint32_t *v) {
    uint32_t r = 0;
    for (uint32_t shift = 0; shift < 35; shift += 7) {
        if (*at >= end) {
            return false;
        }
        uint8_t b = d->u->data[d->u->code.off + *at];
        (*at)++;
        r |= (uint32_t)(b & 0x7FU) << shift;
        if (b < 0x80U) {
            *v = r;
            return true;
        }
    }
    return false;
}

/* The instruction at pc of the code section; false when it cannot be read. */
static bool decode(const dc *d, uint32_t pc, insn *in) {
    uint32_t end = d->u->code.len;
    if (pc >= end) {
        return false;
    }
    uint8_t b = d->u->data[d->u->code.off + pc];
    uint32_t at = pc + 1;
    in->op = b >> 3U;
    in->x = b & 7U;
    in->y = 0;
    if (in->op >= OP_COUNT) {
        return false;
    }
    if (in->op == OP_JMP) {
        if (at + 2 > end) {
            return false;
        }
        const uint8_t *p = d->u->data + d->u->code.off + at;
        int16_t off = (int16_t)(uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8U));
        at += 2;
        in->y = (uint32_t)((int32_t)at + off);
        in->len = at - pc;
        return true;
    }
    if (in->x == 7 && !uleb(d, &at, end, &in->x)) {
        return false;
    }
    if ((in->op == OP_PUSH_UP || in->op == OP_STORE_UP || in->op == OP_CALLB) &&
        !uleb(d, &at, end, &in->y)) {
        return false;
    }
    in->len = at - pc;
    return true;
}

/* ---- constants ---- */

static bool const_string(const dc *d, uint32_t k, const uint8_t **s, uint32_t *n) {
    const fbc_unit *u = d->u;
    if (k >= u->nconsts || u->data[u->consts[k]] != 2) {
        return false;
    }
    uint32_t at = u->consts[k] + 1;
    uint32_t len = 0;
    for (uint32_t shift = 0; shift < 35; shift += 7) {
        if (at >= u->len) {
            return false;
        }
        uint8_t b = u->data[at++];
        len |= (uint32_t)(b & 0x7FU) << shift;
        if (b < 0x80U) {
            break;
        }
    }
    if (len > u->len - at) {
        return false;
    }
    *s = u->data + at;
    *n = len;
    return true;
}

/* s as Filo reads it back: the escapes the reader knows, every other byte
   as it is. */
static dnode *literal(dc *d, const uint8_t *s, uint32_t n) {
    text t = {d, NULL, 0, 0};
    put(&t, "\"", 1);
    for (uint32_t i = 0; i < n; i++) {
        const char *esc = NULL;
        switch (s[i]) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\t':
            esc = "\\t";
            break;
        case '\r':
            esc = "\\r";
            break;
        case 0:
            esc = "\\0";
            break;
        case '\a':
            esc = "\\a";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\f':
            esc = "\\f";
            break;
        case '\v':
            esc = "\\v";
            break;
        default:
            put(&t, (const char *)s + i, 1);
            continue;
        }
        put(&t, esc, 2);
    }
    put(&t, "\"", 1);
    dnode *a = atom(d, t.p, t.n);
    if (a != NULL) {
        a->lit = true;
    }
    return a;
}

/* Constant k as Filo writes it; the infinities and NaN have no literal, so
   they are the expressions the folder makes them from. */
static dnode *konst(dc *d, uint32_t k) {
    const fbc_unit *u = d->u;
    if (k >= u->nconsts) {
        (void)fail(d, "a constant past the table");
        return NULL;
    }
    uint32_t at = u->consts[k];
    dnode *a = NULL;
    switch (u->data[at]) {
    case 1: {
        uint64_t bits = 0;
        for (int i = 7; i >= 0; i--) {
            bits = (bits << 8U) | u->data[at + 1 + (uint32_t)i];
        }
        double v = 0;
        memcpy(&v, &bits, sizeof(v));
        if (v != v) {
            a = atomz(d, "(- 1e400 1e400)");
        } else if (v > 1.7976931348623157e308) {
            a = atomz(d, "1e400");
        } else if (v < -1.7976931348623157e308) {
            a = atomz(d, "-1e400");
        } else {
            char num[40];
            fbc_number(v, num, sizeof(num));
            a = atom_copy(d, num, strlen(num));
        }
        break;
    }
    case 2: {
        const uint8_t *s = NULL;
        uint32_t n = 0;
        if (!const_string(d, k, &s, &n)) {
            (void)fail(d, "a string constant that does not read");
            return NULL;
        }
        return literal(d, s, n);
    }
    case 3:
        a = atomz(d, "#t");
        break;
    case 4:
        a = atomz(d, "#f");
        break;
    default:
        a = atomz(d, "(cond)"); /* no clause, no value but () */
        if (a != NULL) {
            a->empty = true;
        }
        return a;
    }
    if (a != NULL) {
        a->lit = true;
    }
    return a;
}

/* ---- the stack ---- */

typedef struct {
    dentry *stack;
    uint32_t n;
    uint32_t cap;
    dsteps *pending; /* at each height, the steps waiting for the next value pushed there */
    uint32_t npending;
} reader;

static dsteps *pending_at(dc *d, reader *r, uint32_t h) {
    if (h >= r->npending) {
        uint32_t cap = (h * 2U) + 8U;
        dsteps *p = take(d, sizeof(dsteps) * cap);
        if (p == NULL) {
            return NULL;
        }
        if (r->npending > 0) {
            memcpy(p, r->pending, sizeof(dsteps) * r->npending);
        }
        r->pending = p;
        r->npending = cap;
    }
    return &r->pending[h];
}

static void push_entry(dc *d, reader *r, dentry e) {
    if (r->n == r->cap) {
        uint32_t cap = r->cap == 0 ? 16U : r->cap * 2U;
        dentry *s = take(d, sizeof(dentry) * cap);
        if (s == NULL) {
            return;
        }
        if (r->n > 0) {
            memcpy(s, r->stack, sizeof(dentry) * r->n);
        }
        r->stack = s;
        r->cap = cap;
    }
    r->stack[r->n++] = e;
}

static void push(dc *d, reader *r, dnode *n) {
    if (n == NULL) {
        return;
    }
    dsteps *p = pending_at(d, r, r->n);
    if (p == NULL) {
        return;
    }
    dentry e = {n, *p};
    *p = (dsteps){NULL, 0};
    push_entry(d, r, e);
}

static void add_step(dc *d, reader *r, dsteps pre, dstep s) {
    dsteps *p = pending_at(d, r, r->n);
    if (p == NULL) {
        return;
    }
    *p = concat(d, *p, pre.p, pre.n);
    *p = concat(d, *p, &s, 1);
}

/* The top k values off the stack: the forms, all but the first with what
   ran before them around them; the first's entry in first. */
static dnode **pop_args(dc *d, reader *r, uint32_t k, dentry *first) {
    if (r->n < k) {
        (void)fail(d, "an instruction takes more values than there are");
        return NULL;
    }
    dnode **ns = (dnode **)take(d, sizeof(dnode *) * (k > 0 ? k : 1U));
    if (ns == NULL) {
        return NULL;
    }
    dentry *es = r->stack + r->n - k;
    for (uint32_t i = 0; i < k; i++) {
        ns[i] = i == 0 ? es[i].n : wrap(d, es[i]);
    }
    if (k > 0) {
        *first = es[0];
    }
    r->n -= k;
    return ns;
}

/* Pushes the form made of the values popped: the steps before the first go
   out to it, unless keep says they stay in (a call the folder would fold). */
static void finish(dc *d, reader *r, dnode *made, dentry first, uint32_t k, bool keep) {
    if (made == NULL) {
        return;
    }
    if (k == 0) {
        push(d, r, made);
        return;
    }
    dsteps pre = first.pre;
    if (keep && pre.n > 0) {
        pre = (dsteps){NULL, 0};
    }
    push_entry(d, r, (dentry){made, pre});
}

/* Whether the folder folds (name ns...), all literals, to a constant. */
static bool folds(dc *d, const char *name, size_t nlen, dnode **ns, uint32_t k) {
    for (uint32_t i = 0; i < k; i++) {
        if (!ns[i]->lit) {
            return false;
        }
    }
    if (d->folds == NULL) {
        return false;
    }
    text t = {d, NULL, 0, 0};
    put(&t, "(", 1);
    put(&t, name, nlen);
    for (uint32_t i = 0; i < k; i++) {
        put(&t, " ", 1);
        write_node(&t, ns[i]);
    }
    put(&t, ")", 1);
    if (d->failed) {
        return false;
    }
    return d->folds(d->fold_user, t.p, t.n);
}

static const char *import_name(const dc *d, uint32_t i, size_t *n) {
    const fbc_span s = d->u->imports[i];
    *n = s.len;
    return (const char *)d->u->data + s.off;
}

/* ---- regions ---- */

static bool region(dc *d, fnstate *st, uint32_t from, uint32_t to, reader *r);
static nodes function(dc *d, uint32_t fn);

/* [from, to) as one value. */
static bool one(dc *d, fnstate *st, uint32_t from, uint32_t to, dentry *e) {
    reader r = {0};
    if (!region(d, st, from, to, &r)) {
        return false;
    }
    if (r.n != 1) {
        return fail(d, "a region that leaves other than one value");
    }
    *e = r.stack[0];
    return true;
}

/* A STORE_L: a let binding when it is the slot's first store and a POP
   follows, else a set. */
static bool store_local(dc *d, fnstate *st, reader *r, uint32_t x, uint32_t *next) {
    insn after = {0};
    bool pops = false;
    if (decode(d, *next, &after) && after.op == OP_POP) {
        pops = after.x == 1;
    }
    const char *name = name_of(d, st->fn, x);
    if (x >= st->nslots || st->declared[x] || x < d->u->fns[st->fn].params || !pops || r->n == 0) {
        dentry first = {0};
        dnode **ns = pop_args(d, r, 1, &first);
        if (ns == NULL) {
            return false;
        }
        finish(d, r, list2(d, "set", atomz(d, name), ns[0]), first, 1, false);
        return alive(d);
    }
    st->declared[x] = true;
    dentry e = r->stack[--r->n];
    /* a let takes its slots before its values run, so a let inside the
       value has higher ones: it stays inside */
    uint32_t keep = e.pre.n;
    for (uint32_t i = 0; i < e.pre.n; i++) {
        if ((e.pre.p[i].kind == BIND || e.pre.p[i].kind == UNPACK) && e.pre.p[i].slot > x) {
            keep = i;
            break;
        }
    }
    dnode *value = wrap(d, (dentry){e.n, {e.pre.p + keep, e.pre.n - keep}});
    dstep s = {BIND, list1(d, name, value), x}; /* (name value) */
    add_step(d, r, (dsteps){e.pre.p, keep}, s);
    *next += after.len;
    return alive(d);
}

/* A letv: the tuple, UNPACK n, and each name stored from the last. */
static bool unpack(dc *d, fnstate *st, reader *r, uint32_t n, uint32_t *at) {
    if (r->n == 0) {
        return fail(d, "UNPACK of nothing");
    }
    dentry e = r->stack[--r->n];
    dnode **names = (dnode **)take(d, sizeof(dnode *) * (n > 0 ? n : 1U));
    if (names == NULL) {
        return false;
    }
    uint32_t lowest = 0; /* (letv () ...) takes no slot: nothing to keep in order */
    for (uint32_t i = n; i > 0; i--) {
        insn store = {0};
        insn pop = {0};
        if (!decode(d, *at, &store) || store.op != OP_STORE_L ||
            !decode(d, *at + store.len, &pop) || pop.op != OP_POP || pop.x != 1 ||
            store.x >= st->nslots) {
            return fail(d, "UNPACK not followed by its stores");
        }
        st->declared[store.x] = true;
        names[i - 1U] = atomz(d, name_of(d, st->fn, store.x));
        lowest = store.x;
        *at += store.len + pop.len;
    }
    dnode *k[2] = {e.n, bare(d, names, n)};
    add_step(d, r, e.pre, (dstep){UNPACK, bare(d, k, 2), lowest});
    return alive(d);
}

/* The form a jump starts: an if (or a cond clause) at JMP 1, an and or an
   or at JMP 2 or 3, the check of the last operand at JMP 4. */
static bool jump(dc *d, fnstate *st, reader *r, const insn *in, uint32_t pc, uint32_t *next) {
    uint32_t target = in->y;
    if (target < pc + in->len || target > d->u->fns[st->fn].off + d->u->fns[st->fn].len) {
        return fail(d, "a jump out of its function or backward");
    }
    dentry first = {0};
    if (in->x == 1) {
        insn back = {0};
        if (target < 3 || !decode(d, target - 3, &back) || back.op != OP_JMP || back.x != 0) {
            return fail(d, "an if with no jump over its else");
        }
        uint32_t end = back.y;
        if (end < target || end > d->u->fns[st->fn].off + d->u->fns[st->fn].len) {
            return fail(d, "an if whose end is out of its function");
        }
        dentry then = {0};
        dentry els = {0};
        if (!one(d, st, *next, target - 3, &then) || !one(d, st, target, end, &els)) {
            return false;
        }
        dnode **ns = pop_args(d, r, 1, &first);
        if (ns == NULL) {
            return false;
        }
        dnode *t = wrap(d, then);
        dnode *e = wrap(d, els);
        dnode *k[3] = {ns[0], t, e};
        dnode *n = bare(d, k, e != NULL && e->empty && els.pre.n == 0 ? 2U : 3U);
        if (n != NULL) {
            n->isif = true;
        }
        finish(d, r, n, first, 1, false);
        *next = end;
        return alive(d);
    }
    if (in->x == 2 || in->x == 3) {
        dentry rest = {0};
        if (!one(d, st, *next, target, &rest) || rest.n == NULL) {
            return fail(d, "an and/or whose rest does not read");
        }
        if (!((rest.n->chain == in->x && rest.n->out == target) || rest.n->check)) {
            return fail(d, "an and/or whose rest is not one");
        }
        dnode **more = rest.n->kids;
        uint32_t nmore = rest.n->nkids;
        if (rest.pre.n > 0) { /* what ran before the rest's first operand is its own */
            dnode **m = (dnode **)take(d, sizeof(dnode *) * nmore);
            if (m == NULL) {
                return false;
            }
            memcpy((void *)m, (const void *)more, sizeof(dnode *) * nmore);
            m[0] = wrap(d, (dentry){more[0], rest.pre});
            more = m;
        }
        dnode **ns = pop_args(d, r, 1, &first);
        if (ns == NULL) {
            return false;
        }
        nodes kids = {0};
        add(d, &kids, ns[0]);
        for (uint32_t i = 0; i < nmore; i++) {
            add(d, &kids, more[i]);
        }
        dnode *n = bare(d, kids.p, kids.n);
        if (n != NULL) {
            n->chain = (uint8_t)in->x;
            n->out = target;
        }
        finish(d, r, n, first, 1, false);
        *next = target;
        return alive(d);
    }
    if (in->x == 4) {
        dnode **ns = pop_args(d, r, 1, &first);
        if (ns == NULL) {
            return false;
        }
        dnode *n = bare(d, ns, 1);
        if (n != NULL) {
            n->check = true;
        }
        finish(d, r, n, first, 1, false);
        return alive(d);
    }
    return fail(d, "a jump where no form starts");
}

/* The forms that compile to nothing but a TRAP with their message. */
static const char *const traps[][2] = {
    {"empty list expression", "()"},
    {"do expects at least 1 expression", "(do)"},
    {"if expects 2 or 3 arguments (condition then [else])", "(if)"},
    {"in let: let expects bindings and body", "(let)"},
    {"in let: let expects binding list", "(let x 1)"},
    {"in letv: letv expects bindings and body", "(letv)"},
    {"in letv: letv expects name list", "(letv x 1)"},
    {"in fn: fn expects parameters and body", "(fn)"},
    {"in fn: fn expects parameter list", "(fn x 1)"},
    {"in def: def expects name and expression", "(def)"},
    {"def name must be symbol", "(def 1 2)"},
    {"set expects name and expression", "(set)"},
    {"exit expects 0 or 1 argument", "(exit 1 2)"},
    {"return expects 0 or 1 argument", "(return 1 2)"},
    {"cond clause must be a list of a test and a body", "(cond 1)"},
};

/* A TRAP: the malformed form it stands for. (set 1 v) evaluates v and
   drops it first, so v is the statement waiting there. */
static bool trap(dc *d, reader *r, uint32_t k) {
    const uint8_t *s = NULL;
    uint32_t n = 0;
    if (!const_string(d, k, &s, &n)) {
        return fail(d, "TRAP of a constant that is not a string");
    }
    if (same(s, n, "set name must be symbol", 23)) {
        dsteps *p = pending_at(d, r, r->n);
        if (p == NULL || p->n == 0 || p->p[p->n - 1U].kind != STMT) {
            return fail(d, "(set 1 v) without its v");
        }
        dnode *v = p->p[p->n - 1U].v;
        p->n--;
        push(d, r, list2(d, "set", atomz(d, "1"), v));
        return alive(d);
    }
    for (size_t i = 0; i < sizeof(traps) / sizeof(traps[0]); i++) {
        if (same(s, n, traps[i][0], strlen(traps[i][0]))) {
            push(d, r, atomz(d, traps[i][1]));
            return alive(d);
        }
    }
    return fail(d, "a TRAP whose message no form is known to make");
}

static bool is_import(const dc *d, const dnode *n) {
    if (n->kids != NULL || n->lit) {
        return false;
    }
    for (uint32_t i = 0; i < d->u->nimports; i++) {
        size_t len = 0;
        const char *name = import_name(d, i, &len);
        if (len == n->alen && memcmp(name, n->atom, len) == 0) {
            return true;
        }
    }
    return false;
}

/* The value an instruction that pushes one thing pushes, or NULL. */
static dnode *leaf(dc *d, const fnstate *st, const insn *in) {
    const fbc_unit *u = d->u;
    switch (in->op) {
    case OP_PUSH_K:
        return konst(d, in->x);
    case OP_PUSH_G:
        if (in->x >= u->nglobals) {
            (void)fail(d, "a global past the table");
            return NULL;
        }
        return atom(d, (const char *)u->data + u->globals[in->x].off, u->globals[in->x].len);
    case OP_PUSH_L:
        return atomz(d, name_of(d, st->fn, in->x));
    case OP_PUSH_UP: {
        int32_t a = ancestor(d, st->fn, in->x);
        if (a < 0) {
            (void)fail(d, "a local of a function out of reach");
            return NULL;
        }
        return atomz(d, name_of(d, (uint32_t)a, in->y));
    }
    case OP_PUSH_B: {
        if (in->x >= u->nimports) {
            (void)fail(d, "an import past the table");
            return NULL;
        }
        size_t n = 0;
        const char *name = import_name(d, in->x, &n);
        return atom(d, name, n);
    }
    default:
        return NULL;
    }
}

/* A CALLB, a CALL or a TUPLE: the values it takes, in a form. The steps
   before the first stay inside it when moving them out would leave the
   folder a call it folds, which the original could not have been. */
static bool combine_call(dc *d, reader *r, const insn *in) {
    dentry first = {0};
    size_t n = 0;
    const char *name = "tuple";
    uint32_t k = in->x;
    if (in->op == OP_CALLB) {
        if (in->y >= d->u->nimports) {
            return fail(d, "an import past the table");
        }
        name = import_name(d, in->y, &n);
    } else if (in->op == OP_CALL) {
        k++;
    } else {
        n = 5;
    }
    dnode **ns = pop_args(d, r, k, &first);
    if (ns == NULL) {
        return false;
    }
    bool keep = false;
    if (in->op == OP_CALLB && k > 0 && first.pre.n > 0) {
        keep = folds(d, name, n, ns, k);
    }
    if (keep) {
        ns[0] = wrap(d, first);
    }
    nodes kids = {0};
    if (in->op == OP_CALL) {
        if (is_import(d, ns[0])) {
            ns[0] = list1(d, "do", ns[0]); /* a builtin by name would be a CALLB */
        }
    } else {
        add(d, &kids, atom(d, name, n));
    }
    for (uint32_t i = 0; i < k; i++) {
        add(d, &kids, ns[i]);
    }
    finish(d, r, bare(d, kids.p, kids.n), first, k, keep);
    return alive(d);
}

static bool one_value(dc *d, reader *r, dentry *first) {
    dnode **ns = pop_args(d, r, 1, first);
    return ns != NULL;
}

/* Reads the instruction at pc; where the next one starts in *next. */
static bool step(dc *d, fnstate *st, reader *r, const insn *in, uint32_t pc, uint32_t *next) {
    *next = pc + in->len;
    dnode *n = leaf(d, st, in);
    if (n != NULL || d->failed) {
        push(d, r, n);
        return alive(d);
    }
    dentry first = {0};
    switch (in->op) {
    case OP_CLOSURE: {
        if (in->x <= st->fn || in->x >= d->u->nfns) {
            return fail(d, "a closure of a function that is not a later one");
        }
        nodes f = function(d, in->x);
        if (d->failed || f.n != 1) {
            return fail(d, "a closure that does not read");
        }
        push(d, r, f.p[0]);
        return alive(d);
    }
    case OP_TRAP:
        return trap(d, r, in->x);
    case OP_STORE_G: {
        if (in->x >= d->u->nglobals) {
            return fail(d, "a global past the table");
        }
        if (!one_value(d, r, &first)) {
            return false;
        }
        fbc_span g = d->u->globals[in->x];
        dnode *made = list2(d, d->parent[st->fn] < 0 ? "def" : "set",
                            atom(d, (const char *)d->u->data + g.off, g.len), first.n);
        finish(d, r, made, first, 1, false);
        return alive(d);
    }
    case OP_STORE_L:
        return store_local(d, st, r, in->x, next);
    case OP_STORE_UP: {
        int32_t a = ancestor(d, st->fn, in->x);
        if (a < 0) {
            return fail(d, "a local of a function out of reach");
        }
        const char *name = name_of(d, (uint32_t)a, in->y);
        if (!one_value(d, r, &first)) {
            return false;
        }
        finish(d, r, list2(d, "set", atomz(d, name), first.n), first, 1, false);
        return alive(d);
    }
    case OP_POP:
        if (r->n < in->x) {
            return fail(d, "POP of more values than there are");
        }
        for (uint32_t i = 0; i < in->x; i++) {
            dentry e = r->stack[--r->n];
            add_step(d, r, e.pre, (dstep){STMT, e.n, 0});
        }
        return alive(d);
    case OP_CALLB:
    case OP_CALL:
    case OP_TUPLE:
        return combine_call(d, r, in);
    case OP_RET: {
        if (!one_value(d, r, &first)) {
            return false;
        }
        const char *kw = in->x == 1 ? "exit" : "return";
        dnode *made = first.n->empty ? list(d, kw, NULL, 0) : list1(d, kw, first.n);
        finish(d, r, made, first, 1, false);
        return alive(d);
    }
    case OP_UNPACK:
        return unpack(d, st, r, in->x, next);
    case OP_JMP:
        return jump(d, st, r, in, pc, next);
    default:
        return fail(d, "an instruction where no form starts");
    }
}

/* Reads [from, to), leaving what it pushed in r. */
static bool region(dc *d, fnstate *st, uint32_t from, uint32_t to, reader *r) {
    if (++d->depth > DEPTH_MAX) {
        return fail(d, "forms nested deeper than this decompiler reads");
    }
    for (uint32_t pc = from; pc < to && !d->failed;) {
        insn in = {0};
        if (!decode(d, pc, &in)) {
            return fail(d, "an instruction that does not read");
        }
        uint32_t next = pc;
        if (!step(d, st, r, &in, pc, &next)) {
            return false;
        }
        pc = next;
    }
    d->depth--;
    return alive(d);
}

/* fn as forms: an entry's top level, or a closure's (fn ...). */
static nodes function(dc *d, uint32_t fn) {
    nodes none = {0};
    const fbc_fn *f = &d->u->fns[fn];
    insn last = {0};
    if (f->len == 0 || !decode(d, f->off + f->len - 1, &last) || last.op != OP_RET || last.x != 0 ||
        last.len != 1) {
        (void)fail(d, "a function that does not end in RET 0");
        return none;
    }
    fnstate st = {fn, take(d, sizeof(bool) * (f->slots > 0 ? f->slots : 1U)), f->slots};
    if (st.declared == NULL) {
        return none;
    }
    dentry e = {0};
    if (!one(d, &st, f->off, f->off + f->len - 1, &e)) {
        return none;
    }
    nodes forms = seq_forms(d, e.pre, e.n);
    if (d->parent[fn] < 0) {
        return forms;
    }
    nodes params = {0};
    for (uint32_t s = 0; s < f->params; s++) {
        add(d, &params, atomz(d, name_of(d, fn, s)));
    }
    nodes kids = {0};
    add(d, &kids, bare(d, params.p, params.n));
    for (uint32_t i = 0; i < forms.n; i++) {
        add(d, &kids, forms.p[i]);
    }
    nodes out = {0};
    add(d, &out, list(d, "fn", kids.p, kids.n));
    return out;
}

bool fbc_decompile(const fbc_unit *u, void *mem, size_t cap, fbc_folds folder, void *fold_user,
                   fbc_source out, void *out_user, char *why, size_t whycap) {
    static dc d; /* the size of the tables it keeps: not a stack's */
    memset(&d, 0, sizeof(d));
    d.u = u;
    d.mem = mem;
    d.cap = cap;
    d.folds = folder;
    d.fold_user = fold_user;
    d.why = why;
    d.whycap = whycap;
    for (uint32_t i = 0; i < u->nexports; i++) {
        if (u->export_fns[i] != i) {
            return fail(&d, "an entry point is not the function of its place: not this compiler's");
        }
    }
    for (uint32_t i = 0; i < FBC_FNS_MAX; i++) {
        d.parent[i] = -1;
    }
    for (uint32_t fn = 0; fn < u->nfns; fn++) {
        const fbc_fn *f = &u->fns[fn];
        for (uint32_t pc = f->off; pc < f->off + f->len;) {
            insn in = {0};
            if (!decode(&d, pc, &in)) {
                return fail(&d, "an instruction that does not read");
            }
            if (in.op == OP_CLOSURE && in.x > fn && in.x < u->nfns) {
                d.parent[in.x] = (int32_t)fn;
            }
            pc += in.len;
        }
    }
    for (uint32_t i = 0; i < u->nexports; i++) {
        size_t mark = d.used;
        nodes forms = function(&d, i);
        if (d.failed) {
            return false;
        }
        text t = {&d, NULL, 0, 0};
        for (uint32_t k = 0; k < forms.n; k++) {
            write_node(&t, forms.p[k]);
            put(&t, "\n", 1);
        }
        if (d.failed) {
            return false;
        }
        fbc_span s = u->export_names[i];
        out(out_user, (const char *)u->data + s.off, s.len, t.p, t.n);
        d.used = mark;
        for (uint32_t k = 0; k < FBC_FNS_MAX; k++) {
            d.names[k] = NULL; /* they lived in what was just given back */
        }
    }
    return true;
}
