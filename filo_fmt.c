/* filo_fmt.h's formatter, step for step the Go one (format.go: tokenize,
   spans, layout): the same tokens, the same widths counted in runes as Go's
   utf8 counts them, the same decisions. */
#include "filo_fmt.h"

#include <stdbool.h>
#include <string.h>

enum { TOK_OPEN, TOK_CLOSE, TOK_ATOM, TOK_COMMENT };

typedef struct {
    uint8_t type;
    bool trailing;   /* a comment on the same line as the code before it */
    bool newline;    /* an atom with a newline inside: a string over lines */
    uint32_t blanks; /* blank lines before it */
    const char *text;
    uint32_t len;
    uint32_t runes;
    int32_t end;     /* an open's matching close, -1 when there is none */
    int32_t compact; /* an open's group on one line, in runes; -1 when it cannot be */
} token;

typedef struct {
    const char *head; /* the symbol right after the paren, NULL until seen */
    uint32_t head_len;
    uint32_t children; /* laid out so far, the head not counted */
    bool broken;       /* a child took a line of its own: the rest follow */
    uint32_t align;    /* the column the children of a broken frame start on */
    int32_t inline_n;  /* children the head line keeps; -1 for an ordinary call */
    bool clause;       /* a clause of cond: the head line keeps the test */
} frame;

typedef struct {
    token *toks;
    uint32_t ntoks;
    frame *stack;
    uint32_t depth;
    char *out;
    size_t n;
    size_t cap;
    bool full;
    uint32_t indent;
    uint32_t width;
    uint32_t col;
    bool at_line_start;
    bool fresh; /* a line was just started: nothing goes before the token */
} layout;

/* Runes in s as Go's utf8.RuneCountInString counts them: a valid sequence
   is one, and so is each byte of an invalid one. */
static uint32_t runes(const char *str, size_t len) {
    const uint8_t *s = (const uint8_t *)str;
    uint32_t count = 0;
    for (size_t i = 0; i < len; count++) {
        uint8_t c = s[i];
        size_t need = 0;
        uint8_t lo = 0x80;
        uint8_t hi = 0xBF;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            lo = c == 0xE0 ? 0xA0 : 0x80;
            hi = c == 0xED ? 0x9F : 0xBF;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            lo = c == 0xF0 ? 0x90 : 0x80;
            hi = c == 0xF4 ? 0x8F : 0xBF;
        } else {
            i++;
            continue;
        }
        bool ok = i + need < len;
        if (ok && (s[i + 1] < lo || s[i + 1] > hi)) {
            ok = false;
        }
        for (size_t k = 2; ok && k <= need; k++) {
            if (s[i + k] < 0x80 || s[i + k] > 0xBF) {
                ok = false;
            }
        }
        i += ok ? need + 1 : 1;
    }
    return count;
}

/* ---- tokens ---- */

/* Whether something other than blanks precedes position i on its line. */
static bool code_before(const char *src, size_t i) {
    size_t j = i;
    while (j > 0 && (src[j - 1] == ' ' || src[j - 1] == '\t' || src[j - 1] == '\r')) {
        j--;
    }
    if (j == 0) {
        return false;
    }
    return src[j - 1] != '\n';
}

/* The tokens of src into toks (cap of them), or with toks NULL only their
   count; how many, or -1 when they do not fit. */
static int64_t tokenize(const char *src, size_t len, token *toks, size_t cap) {
    size_t n = 0;
    uint32_t blanks = 0;
    for (size_t i = 0; i < len;) {
        char c = src[i];
        if (c == '\n') {
            size_t line_start = i;
            i++;
            while (i < len && (src[i] == ' ' || src[i] == '\t')) {
                i++;
            }
            if (i < len && src[i] == '\n') {
                blanks++;
                continue;
            }
            i = line_start + 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            continue;
        }
        if (toks != NULL && n == cap) {
            return -1;
        }
        token scratch;
        token *t = toks != NULL ? &toks[n] : &scratch;
        memset(t, 0, sizeof(*t));
        t->blanks = blanks;
        t->end = -1;
        t->compact = -1;
        size_t start = i;
        if (c == ';') {
            while (i < len && src[i] != '\n') {
                i++;
            }
            t->type = TOK_COMMENT;
            t->trailing = code_before(src, start);
        } else if (c == '(' || c == ')') {
            i++;
            t->type = c == '(' ? TOK_OPEN : TOK_CLOSE;
        } else if (c == '"') {
            i++;
            while (i < len) {
                if (src[i] == '\\' && i + 1 < len) {
                    i += 2;
                    continue;
                }
                if (src[i] == '"') {
                    i++;
                    break;
                }
                i++;
            }
            t->type = TOK_ATOM;
        } else {
            while (i < len) {
                char a = src[i];
                if (a == ' ' || a == '\t' || a == '\n' || a == '\r' || a == '(' || a == ')' ||
                    a == ';') {
                    break;
                }
                i++;
            }
            t->type = TOK_ATOM;
        }
        blanks = 0;
        n++;
        if (toks == NULL) {
            continue;
        }
        t->text = src + start;
        t->len = (uint32_t)(i - start);
        t->runes = runes(t->text, t->len);
        for (uint32_t k = 0; t->type == TOK_ATOM && k < t->len; k++) {
            if (t->text[k] == '\n') {
                t->newline = true;
            }
        }
    }
    return (int64_t)n;
}

/* Each open's matching close, and the width of its group on one line: -1
   when a comment, a blank line or a string over lines inside forbids it. */
static bool spans(token *toks, uint32_t n, uint32_t *stack) {
    uint32_t depth = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (toks[i].type == TOK_OPEN) {
            stack[depth++] = i;
        } else if (toks[i].type == TOK_CLOSE && depth > 0) {
            uint32_t open = stack[--depth];
            toks[open].end = (int32_t)i;
            int32_t w = 0;
            bool prev_open = false;
            for (uint32_t k = open; k <= i && w >= 0; k++) {
                const token *t = &toks[k];
                if (t->blanks > 0 && k > open) {
                    w = -1;
                    break;
                }
                switch (t->type) {
                case TOK_COMMENT:
                    w = -1;
                    break;
                case TOK_OPEN:
                    w += (k > open && !prev_open ? 1 : 0) + 1;
                    prev_open = true;
                    break;
                case TOK_CLOSE:
                    w++;
                    prev_open = false;
                    break;
                default:
                    if (t->newline) {
                        w = -1;
                        break;
                    }
                    w += (prev_open ? 0 : 1) + (int32_t)t->runes;
                    prev_open = false;
                    break;
                }
            }
            toks[open].compact = w;
        }
    }
    return true;
}

/* ---- the layout ---- */

static void emit(layout *l, const char *s, size_t n) {
    if (l->n + n > l->cap) {
        l->full = true;
        return;
    }
    memcpy(l->out + l->n, s, n);
    l->n += n;
}

static void write_text(layout *l, const char *s, size_t n) {
    emit(l, s, n);
    const char *nl = NULL;
    for (size_t i = n; i > 0; i--) {
        if (s[i - 1] == '\n') {
            nl = s + i - 1;
            break;
        }
    }
    if (nl != NULL) {
        l->col = runes(nl + 1, (size_t)(s + n - (nl + 1)));
    } else {
        l->col += runes(s, n);
    }
    l->at_line_start = false;
    l->fresh = false;
}

static bool ends_with_open(const layout *l) {
    if (l->n == 0) {
        return false;
    }
    return l->out[l->n - 1] == '(';
}

static bool need_space(const layout *l) {
    if (l->at_line_start || l->fresh) {
        return false;
    }
    if (ends_with_open(l)) {
        return false;
    }
    return true;
}

static void newline(layout *l, uint32_t indent) {
    if (!l->at_line_start) {
        emit(l, "\n", 1);
    }
    for (uint32_t i = 0; i < indent; i++) {
        emit(l, " ", 1);
    }
    l->col = indent;
    l->at_line_start = false;
    l->fresh = true;
}

static frame *parent(layout *l) {
    return l->depth > 0 ? &l->stack[l->depth - 1] : NULL;
}

static void blank_line(layout *l) {
    if (!l->at_line_start) {
        emit(l, "\n", 1);
    }
    emit(l, "\n", 1);
    l->col = 0;
    l->at_line_start = true;
    frame *p = parent(l);
    if (p != NULL) {
        p->broken = true;
    }
}

static uint32_t child_indent(layout *l) {
    const frame *p = parent(l);
    return p == NULL ? 0 : p->align;
}

/* Whether something width runes wide goes on the current line; -1 is a
   thing that cannot go on one line at all. */
static bool fits_here(const layout *l, int64_t width) {
    if (width < 0) {
        return false;
    }
    int64_t avail = (int64_t)l->width - (int64_t)l->col;
    if (need_space(l)) {
        avail--;
    }
    return width <= avail;
}

static bool stays(const layout *l, const frame *p, int64_t width) {
    if (p->inline_n >= 0) {
        return (int64_t)p->children < (int64_t)p->inline_n;
    }
    if (p->broken) {
        return false;
    }
    return fits_here(l, width);
}

static void place(layout *l, frame *p, int64_t width) {
    if (p == NULL) {
        if (!l->at_line_start) {
            newline(l, 0); /* top-level forms never share a line */
        }
        return;
    }
    if (!stays(l, p, width)) {
        newline(l, child_indent(l));
        p->broken = true;
    }
}

static void comment(layout *l, const token *t) {
    if (!t->trailing || l->at_line_start) {
        newline(l, child_indent(l));
    } else {
        write_text(l, " ", 1);
    }
    write_text(l, t->text, t->len);
    emit(l, "\n", 1);
    l->col = 0;
    l->at_line_start = true;
    frame *p = parent(l);
    if (p != NULL) {
        p->broken = true;
    }
}

static uint32_t closers_after(const layout *l, uint32_t i) {
    uint32_t n = 0;
    for (uint32_t j = i + 1; j < l->ntoks && l->toks[j].type == TOK_CLOSE && l->toks[j].blanks == 0;
         j++) {
        n++;
    }
    return n;
}

static bool is_head(const frame *f, const char *name) {
    size_t n = strlen(name);
    if (f->head == NULL || f->head_len != n) {
        return false;
    }
    return memcmp(f->head, name, n) == 0;
}

static int32_t head_inline(const char *s, uint32_t n) {
    const frame f = {s, n, 0, false, 0, 0, false};
    if (is_head(&f, "fn") || is_head(&f, "let") || is_head(&f, "def") || is_head(&f, "if")) {
        return 1;
    }
    if (is_head(&f, "letv")) {
        return 2;
    }
    if (is_head(&f, "cond") || is_head(&f, "do") || is_head(&f, "list")) {
        return 0;
    }
    return -1;
}

/* The group from token i on one line, as compact writes it. */
static void write_compact(layout *l, uint32_t open, uint32_t close) {
    bool prev_open = false;
    for (uint32_t k = open; k <= close; k++) {
        const token *t = &l->toks[k];
        if (t->type == TOK_OPEN) {
            if (k > open && !prev_open) {
                write_text(l, " ", 1);
            }
            write_text(l, "(", 1);
            prev_open = true;
        } else if (t->type == TOK_CLOSE) {
            write_text(l, ")", 1);
            prev_open = false;
        } else {
            if (!prev_open) {
                write_text(l, " ", 1);
            }
            write_text(l, t->text, t->len);
            prev_open = false;
        }
    }
}

/* Lays out the group opened at token i; the first token after what it
   took: the whole group when it fit, only the paren when it breaks. */
static uint32_t open_group(layout *l, uint32_t i, uint32_t cap_frames) {
    frame *p = parent(l);
    const token *g = &l->toks[i];
    int64_t width = -1;
    if (g->compact >= 0 && g->end >= 0) {
        width = g->compact + (int64_t)closers_after(l, (uint32_t)g->end);
    }
    bool head_slot = false;
    if (p != NULL && p->inline_n >= 0) {
        head_slot = (int64_t)p->children < (int64_t)p->inline_n;
    }
    place(l, p, width);
    if (fits_here(l, width)) {
        if (need_space(l)) {
            write_text(l, " ", 1);
        }
        write_compact(l, i, (uint32_t)g->end);
        if (p != NULL) {
            p->children++;
        }
        return (uint32_t)g->end + 1;
    }
    if (need_space(l)) {
        write_text(l, " ", 1);
    }
    /* the children of a broken form indent from its own paren, which is not
       where the depth says under a column aligned to a head line (the
       bindings of let) */
    frame f = {NULL, 0, 0, false, l->col + l->indent, -1, false};
    if (p != NULL && is_head(p, "cond")) {
        f.clause = true;
        f.inline_n = 1;
    }
    if (head_slot) {
        /* bindings and params open on the head line and stack their own
           children under the first one */
        f.align = l->col + 1;
        if (is_head(p, "let") || is_head(p, "letv")) {
            f.inline_n = 1;
        }
    }
    write_text(l, "(", 1);
    if (p != NULL) {
        p->children++;
    }
    if (l->depth == cap_frames) {
        l->full = true;
        return i + 1;
    }
    l->stack[l->depth++] = f;
    return i + 1;
}

static void close_group(layout *l) {
    write_text(l, ")", 1);
    if (l->depth > 0) {
        l->depth--;
    }
}

static void atom(layout *l, uint32_t i) {
    const token *t = &l->toks[i];
    frame *p = parent(l);
    if (p != NULL && p->head == NULL && p->children == 0 && ends_with_open(l)) {
        write_text(l, t->text, t->len);
        p->head = t->text;
        p->head_len = t->len;
        p->inline_n =
            p->clause ? 0 : head_inline(t->text, t->len); /* a clause's test is its head */
        return;
    }
    int64_t width = t->newline ? -1 : (int64_t)t->runes + (int64_t)closers_after(l, i);
    if (p != NULL) {
        place(l, p, width);
    } else if (l->at_line_start) {
        newline(l, 0);
    }
    if (need_space(l)) {
        write_text(l, " ", 1);
    }
    write_text(l, t->text, t->len);
    if (p != NULL) {
        p->children++;
    }
}

static void *take(uint8_t **at, size_t *left, size_t n) {
    size_t pad = (8U - ((uintptr_t)*at & 7U)) & 7U;
    if (pad > *left || n > *left - pad) {
        return NULL;
    }
    void *p = *at + pad;
    *at += pad + n;
    *left -= pad + n;
    return p;
}

const char *filo_fmt(const char *src, size_t len, uint32_t indent, uint32_t width, void *mem,
                     size_t cap, size_t *out_len) {
    uint8_t *at = mem;
    size_t left = cap;
    size_t ntok_cap = (size_t)tokenize(src, len, NULL, 0); /* counted first, then read */
    token *toks = take(&at, &left, sizeof(token) * (ntok_cap + 1));
    if (toks == NULL) {
        return NULL;
    }
    int64_t n = tokenize(src, len, toks, ntok_cap);
    uint32_t *stack = take(&at, &left, sizeof(uint32_t) * ((size_t)n + 1));
    frame *frames = take(&at, &left, sizeof(frame) * ((size_t)n + 1));
    if (n < 0 || stack == NULL || frames == NULL) {
        return NULL;
    }
    (void)spans(toks, (uint32_t)n, stack);
    layout l = {0};
    l.toks = toks;
    l.ntoks = (uint32_t)n;
    l.stack = frames;
    l.out = (char *)at;
    l.cap = left;
    l.indent = indent;
    l.width = width;
    l.at_line_start = true;
    for (uint32_t i = 0; i < l.ntoks && !l.full;) {
        const token *t = &toks[i];
        if (t->blanks > 0 && i > 0) {
            blank_line(&l);
        }
        switch (t->type) {
        case TOK_COMMENT:
            comment(&l, t);
            i++;
            break;
        case TOK_OPEN:
            i = open_group(&l, i, (uint32_t)n + 1);
            break;
        case TOK_CLOSE:
            close_group(&l);
            i++;
            break;
        default:
            atom(&l, i);
            i++;
            break;
        }
    }
    if (l.full) {
        return NULL;
    }
    while (l.n > 0 && (l.out[l.n - 1] == '\n' || l.out[l.n - 1] == ' ')) {
        l.n--;
    }
    if (l.n > 0) {
        if (l.n == l.cap) {
            return NULL;
        }
        l.out[l.n++] = '\n';
    }
    *out_len = l.n;
    return l.out;
}
