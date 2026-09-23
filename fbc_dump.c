#include "fbc_dump.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HEADER = 20,
    SECTION = 12,
};

enum {
    OP_PUSH_K = 0,
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
    OP_COUNT,
};

static const char *const mnemonic[OP_COUNT] = {
    "PUSH_K", "PUSH_G", "STORE_G", "PUSH_L", "STORE_L", "PUSH_UP", "STORE_UP", "POP",
    "JMP",    "CALL",   "CALLB",   "RET",    "CLOSURE", "TUPLE",   "UNPACK",   "TRAP",
};

static const char *const condition[] = {"always", "if false", "and", "or", "check"};

/* ---- reading ---- */

typedef struct {
    const uint8_t *p;
    size_t at;
    size_t end;
    bool bad;
} reader;

static uint32_t rd_byte(reader *r) {
    if (r->bad || r->at >= r->end) {
        r->bad = true;
        return 0;
    }
    uint32_t b = r->p[r->at];
    r->at++;
    return b;
}

static uint32_t rd_uleb(reader *r) {
    uint32_t v = 0;
    for (uint32_t shift = 0; shift < 35U; shift += 7U) {
        uint32_t b = rd_byte(r);
        if (shift == 28U && b > 0x0FU) {
            r->bad = true; /* more than 32 bits */
        }
        v |= (b & 0x7FU) << shift;
        if ((b & 0x80U) == 0) {
            return v;
        }
    }
    r->bad = true;
    return 0;
}

static fbc_span rd_name(reader *r) {
    fbc_span s = {0, 0};
    s.len = rd_uleb(r);
    s.off = (uint32_t)r->at;
    if (r->bad || s.len > r->end - r->at) {
        r->bad = true;
        s.len = 0;
        return s;
    }
    r->at += s.len;
    return s;
}

static uint32_t get_u16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U);
}

static uint32_t get_u32(const uint8_t *p) {
    return get_u16(p) | (get_u16(p + 2) << 16U);
}

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

static bool fail(char *why, size_t cap, const char *msg) {
    (void)snprintf(why, cap, "%s", msg);
    return false;
}

static bool read_names(reader *r, fbc_span *names, uint32_t *n) {
    *n = rd_uleb(r);
    if (*n > FBC_NAMES_MAX) {
        return false;
    }
    for (uint32_t i = 0; i < *n; i++) {
        names[i] = rd_name(r);
    }
    if (r->bad) {
        return false;
    }
    return true;
}

static bool read_consts(reader *r, fbc_unit *u) {
    u->nconsts = rd_uleb(r);
    if (u->nconsts > FBC_CONSTS_MAX) {
        return false;
    }
    for (uint32_t i = 0; i < u->nconsts && !r->bad; i++) {
        u->consts[i] = (uint32_t)r->at;
        uint32_t tag = rd_byte(r);
        if (tag == 1) {
            for (int k = 0; k < 8; k++) {
                (void)rd_byte(r);
            }
        } else if (tag == 2) {
            (void)rd_name(r);
        } else if (tag < 3 || tag > 5) {
            return false;
        }
    }
    if (r->bad) {
        return false;
    }
    return true;
}

static bool read_fns(reader *r, fbc_unit *u) {
    u->nfns = rd_uleb(r);
    if (u->nfns > FBC_FNS_MAX) {
        return false;
    }
    for (uint32_t i = 0; i < u->nfns; i++) {
        fbc_fn *f = &u->fns[i];
        f->off = rd_uleb(r);
        f->len = rd_uleb(r);
        f->params = rd_uleb(r);
        f->slots = rd_uleb(r);
        f->stack = rd_uleb(r);
    }
    if (r->bad) {
        return false;
    }
    return true;
}

static bool read_exports(reader *r, fbc_unit *u) {
    u->nexports = rd_uleb(r);
    if (u->nexports > FBC_EXPORTS_MAX) {
        return false;
    }
    for (uint32_t i = 0; i < u->nexports; i++) {
        u->export_names[i] = rd_name(r);
        u->export_fns[i] = rd_uleb(r);
    }
    if (r->bad) {
        return false;
    }
    return true;
}

static bool read_section(fbc_unit *u, uint32_t kind, uint32_t off, uint32_t len) {
    reader r = {u->data, off, (size_t)off + len, false};
    switch (kind) {
    case 1:
        return read_names(&r, u->imports, &u->nimports);
    case 2:
        return read_names(&r, u->globals, &u->nglobals);
    case 3:
        return read_consts(&r, u);
    case 4:
        return read_fns(&r, u);
    case 5:
        u->code.off = off;
        u->code.len = len;
        return true;
    case 6:
        return read_exports(&r, u);
    default:
        return true; /* a kind this reader does not know is skipped, as the spec says */
    }
}

static uint32_t checksum_of(const uint8_t *data, size_t len) {
    static const uint8_t zero[4] = {0, 0, 0, 0};
    uint32_t h = fnv1a(2166136261U, data, 8);
    h = fnv1a(h, zero, 4);
    return fnv1a(h, data + 12, len - 12);
}

uint32_t fbc_kind(const uint8_t *data, size_t len) {
    if (len < HEADER || memcmp(data,
                               "\x7f"
                               "FBC",
                               4) != 0) {
        return 0;
    }
    if (data[4] == 1 || data[4] == 2) {
        return data[4];
    }
    return 0;
}

bool fbc_read_bundle(fbc_bundle *b, const uint8_t *data, size_t len, char *why, size_t cap) {
    memset(b, 0, sizeof(*b));
    b->data = data;
    b->len = len;
    if (fbc_kind(data, len) != 2) {
        return fail(why, cap, "not a Filo bundle");
    }
    b->version = data[5];
    uint32_t hsize = get_u16(data + 6);
    b->checksum = get_u32(data + 8);
    b->checksum_ok = checksum_of(data, len) == b->checksum;
    b->widest_stack = get_u16(data + 12);
    b->widest_frame = get_u16(data + 14);
    b->n = get_u16(data + 16);
    if (b->n == 0 || b->n > FBC_MEMBERS_MAX || hsize > len || HEADER + (b->n * 12U) > hsize) {
        return fail(why, cap, "the bundle's header does not fit the file");
    }
    for (uint32_t i = 0; i < b->n; i++) {
        const uint8_t *e = data + HEADER + ((size_t)i * 12U);
        fbc_member *m = &b->members[i];
        m->unit.off = get_u32(e);
        m->unit.len = get_u32(e + 4);
        uint32_t name_at = get_u32(e + 8);
        if (m->unit.off < hsize || m->unit.off > len || m->unit.len > len - m->unit.off ||
            m->unit.off % 8U != 0 || name_at < hsize || name_at >= len) {
            return fail(why, cap, "a member lies outside the bundle");
        }
        reader r = {data, name_at, len, false};
        m->name = rd_name(&r);
        if (r.bad || m->name.len == 0) {
            return fail(why, cap, "a member's name does not read");
        }
    }
    return true;
}

bool fbc_read(fbc_unit *u, const uint8_t *data, size_t len, char *why, size_t cap) {
    memset(u, 0, sizeof(*u));
    u->data = data;
    u->len = len;
    if (len < HEADER || data[0] != 0x7F || data[1] != 'F' || data[2] != 'B' || data[3] != 'C') {
        return fail(why, cap, "not a Filo unit (no \\x7fFBC at the start)");
    }
    if (data[4] != 1) {
        return fail(why, cap, "not a unit (kind is not 1)");
    }
    u->version = data[5];
    uint32_t hsize = get_u16(data + 6);
    u->checksum = get_u32(data + 8);
    u->widest_stack = get_u16(data + 12);
    u->widest_frame = get_u16(data + 14);
    uint32_t nsec = get_u16(data + 16);
    if (hsize < HEADER || hsize > len || nsec > (hsize - HEADER) / SECTION) {
        return fail(why, cap, "the header does not fit the file");
    }
    u->checksum_ok = checksum_of(data, len) == u->checksum;
    bool seen[8] = {false};
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *e = data + HEADER + ((size_t)i * SECTION);
        uint32_t kind = get_u16(e);
        uint32_t off = get_u32(e + 4);
        uint32_t slen = get_u32(e + 8);
        if (off > len || slen > len - off) {
            return fail(why, cap, "a section lies outside the file");
        }
        if (kind < 8) {
            if (seen[kind]) {
                return fail(why, cap, "a section appears twice");
            }
            seen[kind] = true;
        }
        if (!read_section(u, kind, off, slen)) {
            return fail(why, cap, "a section does not read as the spec says");
        }
    }
    for (uint32_t i = 0; i < u->nfns; i++) {
        if (u->fns[i].off > u->code.len || u->fns[i].len > u->code.len - u->fns[i].off) {
            return fail(why, cap, "a function lies outside the code section");
        }
    }
    for (uint32_t i = 0; i < u->nexports; i++) {
        if (u->export_fns[i] >= u->nfns) {
            return fail(why, cap, "an export names no function");
        }
    }
    return true;
}

/* ---- text ---- */

static size_t put(char *dst, size_t cap, size_t at, const char *fmt, const char *text) {
    if (at >= cap) {
        return at;
    }
    int n = snprintf(dst + at, cap - at, fmt, text);
    if (n < 0) {
        return at;
    }
    return at + (size_t)n > cap ? cap : at + (size_t)n;
}

/* A name from the unit, cut short when it would not fit a line. */
static void name_text(const fbc_unit *u, fbc_span s, char *dst, size_t cap) {
    size_t n = s.len < cap - 1 ? s.len : cap - 1;
    memcpy(dst, u->data + s.off, n);
    dst[n] = '\0';
}

/* How Filo writes a number, which is how Go's strconv does with 'g' and the
   shortest precision: the fewest digits that read back as the same double,
   plain from 1e-4 up to 1e6 and in scientific form outside. */
void fbc_number(double x, char *dst, size_t cap) {
    if (x != x) {
        (void)snprintf(dst, cap, "NaN");
        return;
    }
    if (x - x != x - x) {
        (void)snprintf(dst, cap, "%s", x > 0 ? "+Inf" : "-Inf");
        return;
    }
    char sci[40];
    int prec = 1;
    for (; prec < 17; prec++) {
        (void)snprintf(sci, sizeof(sci), "%.*e", prec - 1, x);
        if (strtod(sci, NULL) == x) {
            break;
        }
    }
    (void)snprintf(sci, sizeof(sci), "%.*e", prec - 1, x);
    const char *e = strchr(sci, 'e');
    int exp = e != NULL ? (int)strtol(e + 1, NULL, 10) : 0;
    if (exp < -4 || exp >= 6) {
        (void)snprintf(dst, cap, "%s", sci);
        return;
    }
    int decimals = prec - 1 - exp;
    (void)snprintf(dst, cap, "%.*f", decimals > 0 ? decimals : 0, x);
}

static void const_text(const fbc_unit *u, uint32_t i, char *dst, size_t cap) {
    const uint8_t *p = u->data + u->consts[i];
    switch (p[0]) {
    case 1: {
        uint64_t bits = 0;
        for (int k = 7; k >= 0; k--) {
            bits = (bits << 8U) | p[1 + k];
        }
        double x = 0;
        memcpy(&x, &bits, sizeof(x));
        fbc_number(x, dst, cap);
        return;
    }
    case 2: {
        reader r = {u->data, u->consts[i] + 1U, u->len, false};
        fbc_span s = rd_name(&r);
        size_t at = put(dst, cap, 0, "%s", "\"");
        for (uint32_t k = 0; k < s.len && at + 8 < cap; k++) {
            uint8_t c = u->data[s.off + k];
            char esc[8];
            if (c == '"' || c == '\\') {
                (void)snprintf(esc, sizeof(esc), "\\%c", c);
            } else if (c == '\n') {
                (void)snprintf(esc, sizeof(esc), "\\n");
            } else if (c < 0x20 || c == 0x7F) {
                (void)snprintf(esc, sizeof(esc), "\\x%02x", c);
            } else {
                esc[0] = (char)c;
                esc[1] = '\0';
            }
            at = put(dst, cap, at, "%s", esc);
            if (k == 40 && s.len > 44) {
                at = put(dst, cap, at, "%s", "...");
                break;
            }
        }
        (void)put(dst, cap, at, "%s", "\"");
        return;
    }
    case 3:
        (void)snprintf(dst, cap, "#t");
        return;
    case 4:
        (void)snprintf(dst, cap, "#f");
        return;
    default:
        (void)snprintf(dst, cap, "(list)");
        return;
    }
}

uint32_t fbc_insn(const fbc_unit *u, uint32_t pc, char *dst, size_t cap) {
    if (pc >= u->code.len) {
        return 0;
    }
    reader r = {u->data, (size_t)u->code.off + pc, (size_t)u->code.off + u->code.len, false};
    uint32_t b = rd_byte(&r);
    uint32_t op = b >> 3U;
    uint32_t x = b & 7U;
    if (op >= OP_COUNT) {
        (void)snprintf(dst, cap, "(unknown 0x%02x)", b);
        return 1;
    }
    if (x == 7U && op != OP_JMP) {
        x = rd_uleb(&r);
    }
    char note[96] = "";
    char operands[48] = "";
    (void)snprintf(operands, sizeof(operands), "%u", x);
    switch (op) {
    case OP_PUSH_K:
    case OP_TRAP:
        if (x < u->nconsts) {
            const_text(u, x, note, sizeof(note));
        }
        break;
    case OP_PUSH_G:
    case OP_STORE_G:
        if (x < u->nglobals) {
            name_text(u, u->globals[x], note, sizeof(note));
        }
        break;
    case OP_PUSH_L:
    case OP_STORE_L:
        (void)snprintf(note, sizeof(note), "slot %u", x);
        break;
    case OP_PUSH_UP:
    case OP_STORE_UP: {
        uint32_t slot = rd_uleb(&r);
        (void)snprintf(operands, sizeof(operands), "%u %u", x, slot);
        (void)snprintf(note, sizeof(note), "slot %u, %u function%s out", slot, x,
                       x == 1 ? "" : "s");
        break;
    }
    case OP_JMP: {
        uint32_t lo = rd_byte(&r);
        uint32_t hi = rd_byte(&r);
        int32_t off = (int32_t)(int16_t)(uint16_t)(lo | (hi << 8U));
        int64_t to = (int64_t)(r.at - u->code.off) + off;
        (void)snprintf(operands, sizeof(operands), "%s", x < 5 ? condition[x] : "?");
        if (x != 4) {
            (void)snprintf(note, sizeof(note), "-> %04lld", (long long)to);
        }
        break;
    }
    case OP_CALLB: {
        uint32_t imp = rd_uleb(&r);
        (void)snprintf(operands, sizeof(operands), "%u %u", x, imp);
        if (imp < u->nimports) {
            name_text(u, u->imports[imp], note, sizeof(note));
        }
        break;
    }
    case OP_RET:
        (void)snprintf(note, sizeof(note), "%s", x == 0 ? "return" : "exit");
        break;
    case OP_CLOSURE:
        (void)snprintf(note, sizeof(note), "fn %u", x);
        break;
    default:
        break;
    }
    if (r.bad) {
        return 0;
    }
    if (note[0] != '\0') {
        (void)snprintf(dst, cap, "%-8s %-9s %s", mnemonic[op], operands, note);
    } else {
        (void)snprintf(dst, cap, "%-8s %s", mnemonic[op], operands);
    }
    return (uint32_t)(r.at - u->code.off - pc);
}

/* ---- the listing ---- */

static void say(fbc_out out, void *user, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void say(fbc_out out, void *user, const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    out(user, line);
}

static void list_names(const fbc_unit *u, const fbc_span *names, uint32_t n, const char *title,
                       const char *note, fbc_out out, void *user) {
    say(out, user, "%s (%u)%s", title, n, note);
    for (uint32_t i = 0; i < n; i++) {
        char name[128];
        name_text(u, names[i], name, sizeof(name));
        say(out, user, "  %4u  %s", i, name);
    }
}

static const char *export_of(const fbc_unit *u, uint32_t fn, char *dst, size_t cap) {
    dst[0] = '\0';
    for (uint32_t i = 0; i < u->nexports; i++) {
        if (u->export_fns[i] == fn) {
            name_text(u, u->export_names[i], dst, cap);
        }
    }
    return dst;
}

static void list_fn(const fbc_unit *u, uint32_t i, fbc_out out, void *user) {
    const fbc_fn *f = &u->fns[i];
    char name[128];
    export_of(u, i, name, sizeof(name));
    say(out, user, "");
    char label[160] = "";
    if (name[0] != '\0') {
        (void)snprintf(label, sizeof(label), " (entry \"%s\")", name);
    }
    say(out, user, "fn %u%s: params %u, slots %u, stack %u, %u bytes", i, label, f->params,
        f->slots, f->stack, f->len);
    uint32_t pc = f->off;
    uint32_t end = f->off + f->len;
    while (pc < end) {
        char text[160];
        uint32_t n = fbc_insn(u, pc, text, sizeof(text));
        if (n == 0) {
            say(out, user, "  %04u  (an instruction cut short)", pc);
            return;
        }
        char hex[20] = "";
        size_t at = 0;
        for (uint32_t k = 0; k < n && k < 5; k++) {
            at += (size_t)snprintf(hex + at, sizeof(hex) - at, "%02x ",
                                   u->data[u->code.off + pc + k]);
        }
        say(out, user, "  %04u  %-15s %s", pc, hex, text);
        pc += n;
    }
    if (pc > end) {
        say(out, user, "  (the last instruction runs past the function)");
    }
}

void fbc_dump(const fbc_unit *u, fbc_out out, void *user) {
    say(out, user, "unit: %zu bytes, format %u, checksum %08x %s", u->len, u->version, u->checksum,
        u->checksum_ok ? "ok" : "WRONG");
    say(out, user, "widest stack %u, widest frame %u", u->widest_stack, u->widest_frame);
    say(out, user, "");
    list_names(u, u->imports, u->nimports, "imports", ": what the loading context must provide",
               out, user);
    list_names(u, u->globals, u->nglobals, "globals", "", out, user);
    say(out, user, "constants (%u)", u->nconsts);
    for (uint32_t i = 0; i < u->nconsts; i++) {
        char text[128];
        const_text(u, i, text, sizeof(text));
        say(out, user, "  %4u  %s", i, text);
    }
    say(out, user, "exports (%u)", u->nexports);
    for (uint32_t i = 0; i < u->nexports; i++) {
        char name[128];
        name_text(u, u->export_names[i], name, sizeof(name));
        say(out, user, "  %s -> fn %u", name, u->export_fns[i]);
    }
    for (uint32_t i = 0; i < u->nfns; i++) {
        list_fn(u, i, out, user);
    }
}

void fbc_dump_bundle(const fbc_bundle *b, fbc_out out, void *user) {
    say(out, user, "bundle: %zu bytes, format %u, checksum %08x %s", b->len, b->version,
        b->checksum, b->checksum_ok ? "ok" : "WRONG");
    say(out, user, "widest stack %u, widest frame %u", b->widest_stack, b->widest_frame);
    say(out, user, "members (%u)", b->n);
    for (uint32_t i = 0; i < b->n; i++) {
        const fbc_member *m = &b->members[i];
        say(out, user, "  %-16.*s %6u bytes at %u", (int)m->name.len,
            (const char *)b->data + m->name.off, m->unit.len, m->unit.off);
    }
    for (uint32_t i = 0; i < b->n; i++) {
        const fbc_member *m = &b->members[i];
        static fbc_unit u;
        char why[128];
        say(out, user, "");
        say(out, user, "== %.*s", (int)m->name.len, (const char *)b->data + m->name.off);
        if (!fbc_read(&u, b->data + m->unit.off, m->unit.len, why, sizeof(why))) {
            say(out, user, "(not a unit: %s)", why);
            continue;
        }
        fbc_dump(&u, out, user);
    }
}
