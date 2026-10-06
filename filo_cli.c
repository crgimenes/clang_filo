/* filo: the language at a terminal, and the whole road from source to the
   machine in view. A program runs from source (the IR, as the Go engine runs
   it) or from a unit (the bytecode); build writes a unit, dump lists one,
   and --trace shows the machine at work, one instruction at a time.

   The listing and the trace read units with fbc_dump.c, which knows the
   format only from docs/bytecode.md: what they show is the file as written,
   not what the runtime made of it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fbc_decompile.h"
#include "fbc_dump.h"
#include "filo.h"
#include "filo_fmt.h"
#include "filo_libc.h"
#include "filo_math.h"
#include "filo_strings.h"

enum {
    MEM_PERSISTENT = 4U << 20U,
    MEM_RUN = 8U << 20U,
    FILE_MAX = 1U << 20U,
    FILES_MAX = 64,
};

static uint8_t persistent_mem[MEM_PERSISTENT];
static uint8_t run_mem[MEM_RUN];
static uint8_t unit_mem[FILE_MAX];
static uint8_t bundle_mem[FILE_MAX];
static char sources[FILES_MAX][FILE_MAX / FILES_MAX];
static filo_ctx ctx;
static fbc_unit listing;

/* Each command's help: filo CMD -h writes its own, filo -h all of them. */
typedef struct {
    const char *name;
    const char *synopsis;
    const char *text;
    const char *examples;
} command;

static const command commands[] = {
    {
        "run",
        "filo run [--vm | --trace | --both] FILE [MEMBER] [ENTRY...]",
        "run runs a program and writes its value: a source (FILE is told apart from\n"
        "bytecode by the magic) on the tree the compiler lowers; --vm compiles it to\n"
        "bytecode in memory first; --trace runs it as bytecode, writing each\n"
        "instruction with the top of its operand stack; --both runs it both ways, a\n"
        "line each, with their steps. For a unit, the ENTRY points run in order and\n"
        "share their globals (default: main, or the first); for a bundle, MEMBER is\n"
        "the unit (default: main, or the first).",
        "  filo run --both fib.filo\n  filo run prog.fbc main fail",
    },
    {
        "build",
        "filo build [--strip] [-vm PROFILE] -o OUT FILE...",
        "build compiles programs into one unit of bytecode (docs/bytecode.md), each\n"
        "an entry named by its file (lib/hello.filo is the entry \"hello\"): the same\n"
        "bytes the Go engine's filo build writes. --strip leaves out the debug section\n"
        "(the lines and columns errors say). -vm compiles against the VM the profile\n"
        "lists, as check reads it: a call to one of its functions is a call to a\n"
        "builtin (an import), as where that VM compiles it, not to a global.",
        "  filo build -o prog.fbc main.filo fail.filo\n  filo build -vm app.vm -o app.fbc "
        "draw.filo",
    },
    {
        "bundle",
        "filo bundle -o OUT UNIT...",
        "bundle puts units into one bundle, each a member named by its file.",
        "  filo bundle -o demo.fbb prog.fbc upper.fbc",
    },
    {
        "dump",
        "filo dump FILE",
        "dump lists Filo bytecode: a unit (.fbc) or a bundle (.fbb), as\n"
        "docs/bytecode.md describes it -- the header, the names it imports and the\n"
        "globals it uses (the extern ones marked), its constants and entry points,\n"
        "and every function, each instruction with its bytes and the line:column it\n"
        "came from. A source is compiled first, as build compiles it.",
        "  filo dump edt.fbb | less",
    },
    {
        "show",
        "filo show tree|folded|ir FILE",
        "show writes one stage of what the compiler makes of a source, each line\n"
        "with the line:column it came from: the tree as read, the tree once\n"
        "constants folded, or the IR, frames and slots named.",
        "  filo show ir fib.filo",
    },
    {
        "check",
        "filo check [-vm PROFILE] [FILE]",
        "check says of each unit (a bundle's members, each) whether a VM gives what\n"
        "it asks for: the functions it imports and the extern globals it reads. The\n"
        "VM is this command's (the core, math and strings), or the one PROFILE lists:\n"
        "the names it gives, one a line -- a function, or \"global NAME\" for a value\n"
        "it sets -- and \"#\" for a comment (rocchetto's build writes its own). A line a unit: \"NAME  "
        "runs: ...\" or \"NAME  lacks N: a, b\"; the exit\n"
        "status is 1 when one lacks something. FILE is read from standard input when\n"
        "absent or \"-\".",
        "  filo check -vm bin.vm mine.fbb",
    },
    {
        "decompile",
        "filo decompile [-o DIR] FILE [MEMBER]",
        "decompile writes a unit's entry points back as Filo, a top-level form a\n"
        "line, laid out as filofmt does: each under a \"; NAME.filo\" line, or with -o as\n"
        "DIR/NAME.filo, the paths written in the unit's order, which filo build takes\n"
        "to make the same unit again, byte for byte but for the debug section. The\n"
        "bytes do not keep comments, layout, or the names of parameters and let\n"
        "bindings (x y z, a b c here, a digit for a nested function's); forms that\n"
        "compile alike come back as one (cond for if chains, a constant for what was\n"
        "folded into it). The Go engine's filo decompile writes the same forms. For a\n"
        "bundle, MEMBER is the unit (default: main, or the first).",
        "  filo decompile prog.fbc\n  filo build -o again.fbc $(filo decompile -o src prog.fbc)",
    },
    {
        "fmt",
        "filo fmt [-w] [FILE...]",
        "fmt lays Filo source out as the Go repository's filofmt does, byte for\n"
        "byte: on standard output, or with -w back into each file. FILE is read from\n"
        "standard input when there is none.",
        "  filo fmt -w examples/*.filo",
    },
    {
        "size",
        "filo size [FILE]",
        "size says where the bytes go: each unit's header and sections, in the\n"
        "order the file has them. FILE is read from standard input when absent or \"-\".",
        "  filo size screens.fbb",
    },
};

enum { NCOMMANDS = sizeof(commands) / sizeof(commands[0]) };

/* The help of the command name, or of them all when name is NULL. */
static void help(FILE *f, const char *name) {
    const char *lead = "usage: ";
    for (int i = 0; i < NCOMMANDS; i++) {
        if (name == NULL || strcmp(commands[i].name, name) == 0) {
            (void)fprintf(f, "%s%s\n", lead, commands[i].synopsis);
            lead = "       ";
        }
    }
    for (int i = 0; i < NCOMMANDS; i++) {
        if (name == NULL || strcmp(commands[i].name, name) == 0) {
            (void)fprintf(f, "\n%s\n", commands[i].text);
        }
    }
    (void)fputs("\nExamples:\n", f);
    for (int i = 0; i < NCOMMANDS; i++) {
        if (name == NULL || strcmp(commands[i].name, name) == 0) {
            (void)fprintf(f, "%s\n", commands[i].examples);
        }
    }
}

/* A diagnostic, on stderr, where it does not mix with the value. */
static int complain(const char *text) {
    (void)fputs("filo: ", stderr);
    (void)fputs(text, stderr);
    (void)fputs("\n", stderr);
    return 1;
}

static int complain2(const char *a, const char *b) {
    char line[512];
    (void)snprintf(line, sizeof(line), "%s: %s", a, b);
    return complain(line);
}

/* An error of a run, as compilers say them: where:line:col: message, when
   the runtime knows the place. A parse error already says it. */
static int complain_at(const char *where) {
    uint32_t line = 0;
    uint32_t col = 0;
    const char *msg = filo_error(&ctx);
    if (strncmp(msg, "parse error", 11) == 0 || !filo_error_at(&ctx, &line, &col)) {
        return complain2(where, msg);
    }
    char place[300];
    (void)snprintf(place, sizeof(place), "%s:%u:%u", where, line, col);
    return complain2(place, msg);
}

static void start(void) {
    filo_init(&ctx, &filo_libc_host, persistent_mem, sizeof(persistent_mem), run_mem,
              sizeof(run_mem));
    (void)filo_math_register(&ctx, &filo_libc_math);
    (void)filo_strings_register(&ctx, &filo_libc_strings);
}

static size_t read_file(const char *path, void *dst, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        (void)complain2("cannot open", path);
        return 0;
    }
    size_t n = fread(dst, 1, cap, f);
    fclose(f);
    if (n == cap) {
        (void)complain2(path, "too large");
        return 0;
    }
    if (n == 0) {
        (void)complain2(path, "is empty");
    }
    return n;
}

static bool is_bytecode(const uint8_t *p, size_t n) {
    return fbc_kind(p, n) != 0;
}

/* "lib/hello.filo" is the entry "hello", and "lib/hello.fbc" the member "hello" */
static void entry_name(const char *path, const char *ext, char *dst, size_t cap) {
    const char *base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    size_t n = strlen(base);
    size_t e = strlen(ext);
    if (n > e && strcmp(base + n - e, ext) == 0) {
        n -= e;
    }
    (void)snprintf(dst, cap, "%.*s", (int)n, base);
}

/* Compiles the files, in order, into one unit in unit_mem. */
static size_t build(char **paths, int n) {
    static filo_prog progs[FILES_MAX];
    static filo_bc_entry entries[FILES_MAX];
    static char names[FILES_MAX][256];
    if (n < 1 || n > FILES_MAX) {
        (void)complain("build takes 1 to 64 files");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        size_t len = read_file(paths[i], sources[i], sizeof(sources[i]));
        if (len == 0) {
            return 0;
        }
        if (filo_compile(&ctx, (const uint8_t *)sources[i], len, &progs[i]) != FILO_OK) {
            (void)complain_at(paths[i]);
            return 0;
        }
        entry_name(paths[i], ".filo", names[i], sizeof(names[i]));
        entries[i].name = names[i];
        entries[i].prog = &progs[i];
    }
    size_t len = 0;
    if (filo_bc_build(&ctx, entries, (uint32_t)n, unit_mem, sizeof(unit_mem), &len) != FILO_OK) {
        (void)complain(filo_error(&ctx));
        return 0;
    }
    return len;
}

static void print_line(void *user, const char *line) {
    (void)user;
    puts(line);
}

/* Spaces that bring text to width columns: a constant may hold UTF-8, and a
   byte count would push the stack column out of line. */
static int pad(const char *text, int width) {
    int cols = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (((unsigned char)*p & 0xC0U) != 0x80U) {
            cols++;
        }
    }
    return cols < width ? width - cols : 0;
}

/* Each instruction as the listing shows it, indented by how many calls deep
   the run is, and the top of the operand stack on the right. */
static void show_step(void *user, const filo_trace *t) {
    (void)user;
    char insn[160];
    if (fbc_insn(&listing, t->pc, insn, sizeof(insn)) == 0) {
        (void)snprintf(insn, sizeof(insn), "(unreadable)");
    }
    char stack[160] = "";
    size_t at = 0;
    uint32_t from = t->depth > 4 ? t->depth - 4 : 0;
    if (from > 0) {
        at += (size_t)snprintf(stack, sizeof(stack), "... ");
    }
    for (uint32_t i = from; i < t->depth && at < sizeof(stack) - 24; i++) {
        char v[64];
        size_t n = 0;
        if (t->stack[i].kind == FILO_FUNC) {
            n = (size_t)snprintf(v, sizeof(v), "fn"); /* a function has no source form */
        } else if (filo_value_repr(&ctx, &t->stack[i], v, sizeof(v), &n) != FILO_OK) {
            n = (size_t)snprintf(v, sizeof(v), "?");
        }
        at += (size_t)snprintf(stack + at, sizeof(stack) - at, "%s%.*s%s", i > from ? " " : "",
                               n > 18 ? 16 : (int)n, v, n > 18 ? ".." : "");
    }
    printf("%*s%04u  %s%*s |%s%s\n", (int)(t->calls * 2U), "", t->pc, insn, pad(insn, 34), "",
           stack[0] != '\0' ? " " : "", stack);
}

/* The value whole, however long: measured first, then written. */
static int show_value(const filo_value *v) {
    size_t n = 0;
    if (filo_value_text(&ctx, v, NULL, 0, &n) != FILO_OK) {
        return complain(filo_error(&ctx));
    }
    char *text = malloc(n + 1);
    if (text == NULL) {
        return complain("out of memory");
    }
    (void)filo_value_text(&ctx, v, text, n + 1, &n);
    (void)fwrite(text, 1, n, stdout);
    (void)fputc('\n', stdout);
    free(text);
    return 0;
}

static int run_unit(const uint8_t *data, size_t len, char **entries, int nentries, bool trace) {
    char why[128];
    if (!fbc_read(&listing, data, len, why, sizeof(why))) {
        return complain(why);
    }
    if (trace) {
        ctx.host.trace = show_step;
    }
    const filo_unit *unit = NULL;
    if (filo_bc_load(&ctx, data, len, &unit) != FILO_OK) {
        return complain(filo_error(&ctx));
    }
    char first[256] = "main";
    char *fallback[1] = {first};
    if (nentries == 0) {
        if (!filo_bc_has(unit, "main") && listing.nexports > 0) {
            fbc_span s = listing.export_names[0];
            (void)snprintf(first, sizeof(first), "%.*s", (int)s.len, (const char *)data + s.off);
        }
        entries = fallback;
        nentries = 1;
    }
    filo_value v = {0};
    for (int i = 0; i < nentries; i++) {
        if (filo_bc_run(&ctx, unit, entries[i], NULL, &v) != FILO_OK) {
            return complain_at(entries[i]); /* an entry is named by its source */
        }
        if (trace) {
            printf("-- %s: %u steps\n", entries[i], ctx.steps);
        }
    }
    return show_value(&v);
}

/* A bundle's member, by the name given or the default. */
static int run_member(size_t len, char **args, int nargs, bool trace) {
    static fbc_bundle b;
    char why[128];
    if (!fbc_read_bundle(&b, unit_mem, len, why, sizeof(why))) {
        return complain(why);
    }
    char name[256] = "main";
    const uint8_t *unit = NULL;
    size_t ulen = 0;
    if (nargs > 0) {
        (void)snprintf(name, sizeof(name), "%s", args[0]);
        args++;
        nargs--;
    } else if (filo_bundle_find(&ctx, unit_mem, len, name, &unit, &ulen) != FILO_OK) {
        (void)snprintf(name, sizeof(name), "%.*s", (int)b.members[0].name.len,
                       (const char *)unit_mem + b.members[0].name.off);
    }
    if (filo_bundle_find(&ctx, unit_mem, len, name, &unit, &ulen) != FILO_OK) {
        return complain(filo_error(&ctx));
    }
    return run_unit(unit, ulen, args, nargs, trace);
}

/* One run's end as a line: the value, or the error and where. */
static void say_end(const char *who, int rc, const filo_value *v, uint32_t steps,
                    const char *unit) {
    char text[200];
    size_t n = 0;
    if (rc != FILO_OK) {
        uint32_t line = 0;
        uint32_t col = 0;
        char at[40] = "";
        if (filo_error_at(&ctx, &line, &col)) {
            (void)snprintf(at, sizeof(at), " at %u:%u", line, col);
        }
        printf("%s  error%s: %s  (%u %s)\n", who, at, filo_error(&ctx), steps, unit);
        return;
    }
    if (filo_value_repr(&ctx, v, text, sizeof(text), &n) != FILO_OK) {
        n = (size_t)snprintf(text, sizeof(text), "?");
    }
    printf("%s  %.*s%s  (%u %s)\n", who, n < sizeof(text) ? (int)n : (int)sizeof(text) - 1, text,
           n < sizeof(text) ? "" : "...", steps, unit);
}

/* The same source on the IR, as the Go engine runs it, and on the VM: the
   same value or the same error in the same place, in steps of their own. */
static int run_both(const char *path, size_t len) {
    static char src[FILE_MAX];
    memcpy(src, unit_mem, len);
    filo_prog prog;
    filo_value v = {0};
    if (filo_compile(&ctx, (const uint8_t *)src, len, &prog) != FILO_OK) {
        return complain_at(path);
    }
    int rc = filo_run(&ctx, &prog, NULL, &v);
    say_end("ir", rc, &v, ctx.steps, "steps, one a node");
    char *paths[1] = {(char *)path};
    size_t ulen = build(paths, 1);
    if (ulen == 0) {
        return 1;
    }
    const filo_unit *unit = NULL;
    if (filo_bc_load(&ctx, unit_mem, ulen, &unit) != FILO_OK) {
        return complain(filo_error(&ctx));
    }
    char name[256];
    entry_name(path, ".filo", name, sizeof(name));
    rc = filo_bc_run(&ctx, unit, name, NULL, &v);
    say_end("vm", rc, &v, ctx.steps, "steps, one an instruction");
    return 0;
}

static int cmd_run(int argc, char **argv) {
    bool vm = false;
    bool trace = false;
    bool both = false;
    int i = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--vm") == 0) {
            vm = true;
        } else if (strcmp(argv[i], "--both") == 0) {
            both = true;
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else {
            (void)complain2("unknown flag", argv[i]);
            help(stderr, "run");
            return 2;
        }
    }
    if (i >= argc) {
        help(stderr, "run");
        return 2;
    }
    size_t len = read_file(argv[i], unit_mem, sizeof(unit_mem));
    if (len == 0) {
        return 1;
    }
    if (fbc_kind(unit_mem, len) == 1) {
        return run_unit(unit_mem, len, argv + i + 1, argc - i - 1, trace);
    }
    if (fbc_kind(unit_mem, len) == 2) {
        return run_member(len, argv + i + 1, argc - i - 1, trace);
    }
    if (i + 1 < argc) {
        (void)complain2("entries are for units, and this is source", argv[i]);
        return 2;
    }
    if (both) {
        return run_both(argv[i], len);
    }
    if (vm || trace) {
        len = build(argv + i, 1);
        if (len == 0) {
            return 1;
        }
        return run_unit(unit_mem, len, NULL, 0, trace);
    }
    filo_prog prog;
    filo_value v = {0};
    if (filo_compile(&ctx, unit_mem, len, &prog) != FILO_OK ||
        filo_run(&ctx, &prog, NULL, &v) != FILO_OK) {
        return complain_at(argv[i]);
    }
    return show_value(&v);
}

/* The names a VM gives: a profile's lines ("#" a comment), or when there is
   no profile the builtins of this command's context. A line is a function
   the VM gives ("print-at") or, after "global ", a value it sets ("global
   W"): check wants both, build only the functions. */
typedef struct {
    const char *text; /* the profile, or NULL */
    size_t len;
} offer;

/* Calls fn with each name of the profile, and whether it is a value; stops
   when fn returns true, and says whether one did. */
static bool profile_each(const offer *o,
                         bool (*fn)(void *user, const char *name, size_t n, bool global),
                         void *user) {
    static const char global[] = "global ";
    for (size_t at = 0; at < o->len;) {
        size_t end = at;
        while (end < o->len && o->text[end] != '\n') {
            end++;
        }
        size_t a = at;
        size_t z = end;
        at = end + 1;
        while (a < z && (o->text[a] == ' ' || o->text[a] == '\t' || o->text[a] == '\r')) {
            a++;
        }
        while (z > a &&
               (o->text[z - 1] == ' ' || o->text[z - 1] == '\t' || o->text[z - 1] == '\r')) {
            z--;
        }
        if (z == a || o->text[a] == '#') {
            continue;
        }
        bool value = false;
        if (z - a > sizeof(global) - 1 && memcmp(o->text + a, global, sizeof(global) - 1) == 0) {
            value = true;
            a += sizeof(global) - 1;
            while (a < z && (o->text[a] == ' ' || o->text[a] == '\t')) {
                a++;
            }
        }
        if (fn(user, o->text + a, z - a, value)) {
            return true;
        }
    }
    return false;
}

typedef struct {
    const uint8_t *name;
    uint32_t n;
} wanted;

/* cppcheck-suppress constParameterCallback ; profile_each's shape */
static bool same_name(void *user, const char *name, size_t n, bool global) {
    (void)global;
    const wanted *w = user;
    if (n != w->n) {
        return false;
    }
    return memcmp(name, w->name, n) == 0;
}

/* cppcheck-suppress constParameterCallback ; fbc_offers' shape */
static bool offered(void *user, const uint8_t *name, uint32_t n) {
    const offer *o = user;
    if (o->text == NULL) {
        for (uint32_t i = 0; i < ctx.nbuiltins; i++) {
            const char *b = ctx.builtins[i].name;
            if (strlen(b) == n && memcmp(b, name, n) == 0) {
                return true;
            }
        }
        return false;
    }
    wanted w = {name, n};
    return profile_each(o, same_name, &w);
}

/* What a profile's function does here: build only compiles against it,
   and nothing runs it in this command. */
static int b_profiled(filo_ctx *c, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    (void)n;
    (void)out;
    return filo_fail(c, "a builtin of the profile's VM, not this command's");
}

/* The profile's functions, registered so a call to one compiles as a call
   to a builtin (an import), as it would where the VM compiled it. */
static bool register_name(void *user, const char *name, size_t n, bool global) {
    static char names[FILE_MAX];
    static size_t used = 0;
    bool *failed = user;
    if (global) {
        return false;
    }
    if (used + n + 1 > sizeof(names)) {
        (void)complain("the profile has too many names");
        *failed = true;
        return true;
    }
    char *copy = names + used;
    memcpy(copy, name, n);
    copy[n] = '\0';
    for (uint32_t i = 0; i < ctx.nbuiltins; i++) {
        if (strcmp(ctx.builtins[i].name, copy) == 0) {
            return false; /* the core has it already */
        }
    }
    used += n + 1;
    if (filo_register_builtin(&ctx, copy, b_profiled) != FILO_OK) {
        (void)complain(filo_error(&ctx));
        *failed = true;
        return true;
    }
    return false;
}

/* -vm PROFILE in front of args: the profile read into o, and how many
   arguments that took (0 without one, -1 when it cannot be read). */
static int vm_flag(int argc, char **argv, offer *o) {
    static char profile[FILE_MAX];
    if (argc < 2 || strcmp(argv[0], "-vm") != 0) {
        return 0;
    }
    size_t n = read_file(argv[1], profile, sizeof(profile));
    if (n == 0) {
        return -1;
    }
    *o = (offer){profile, n};
    return 2;
}

static int cmd_build(int argc, char **argv) {
    bool strip = false;
    if (argc > 0 && strcmp(argv[0], "--strip") == 0) {
        strip = true;
        argc--;
        argv++;
    }
    offer o = {NULL, 0};
    int skip = vm_flag(argc, argv, &o);
    if (skip < 0) {
        return 1;
    }
    argc -= skip;
    argv += skip;
    if (argc < 3 || strcmp(argv[0], "-o") != 0) {
        help(stderr, "build");
        return 2;
    }
    bool failed = false;
    if (o.text != NULL && profile_each(&o, register_name, &failed) && failed) {
        return 1;
    }
    size_t len = build(argv + 2, argc - 2);
    if (len == 0) {
        return 1;
    }
    if (strip) {
        size_t full = len;
        memcpy(bundle_mem, unit_mem, full);
        if (filo_bc_strip(&ctx, bundle_mem, full, unit_mem, sizeof(unit_mem), &len) != FILO_OK) {
            return complain(filo_error(&ctx));
        }
    }
    FILE *f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(unit_mem, 1, len, f) != len) {
        (void)complain2("cannot write", argv[1]);
        if (f != NULL) {
            fclose(f);
        }
        return 1;
    }
    if (fclose(f) != 0) {
        return complain2("cannot write", argv[1]);
    }
    return 0;
}

static int cmd_bundle(int argc, char **argv) {
    static filo_bundle_member members[FILES_MAX];
    static char names[FILES_MAX][256];
    static uint8_t units[FILES_MAX][FILE_MAX / FILES_MAX];
    if (argc < 3 || argc - 2 > FILES_MAX || strcmp(argv[0], "-o") != 0) {
        help(stderr, "bundle");
        return 2;
    }
    int n = argc - 2;
    for (int i = 0; i < n; i++) {
        const char *path = argv[2 + i];
        size_t len = read_file(path, units[i], sizeof(units[i]));
        if (len == 0) {
            return 1;
        }
        if (fbc_kind(units[i], len) != 1) {
            return complain2("not a unit", path);
        }
        entry_name(path, ".fbc", names[i], sizeof(names[i]));
        members[i].name = names[i];
        members[i].data = units[i];
        members[i].len = len;
    }
    size_t len = 0;
    if (filo_bundle_build(&ctx, members, (uint32_t)n, bundle_mem, sizeof(bundle_mem), &len) !=
        FILO_OK) {
        return complain(filo_error(&ctx));
    }
    FILE *f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(bundle_mem, 1, len, f) != len) {
        (void)complain2("cannot write", argv[1]);
        if (f != NULL) {
            fclose(f);
        }
        return 1;
    }
    if (fclose(f) != 0) {
        return complain2("cannot write", argv[1]);
    }
    return 0;
}

static int cmd_show(int argc, char **argv) {
    if (argc != 2) {
        help(stderr, "show");
        return 2;
    }
    size_t len = read_file(argv[1], unit_mem, sizeof(unit_mem));
    if (len == 0) {
        return 1;
    }
    if (filo_show(&ctx, unit_mem, len, argv[0], print_line, NULL) != FILO_OK) {
        return complain_at(argv[1]);
    }
    return 0;
}

static int cmd_dump(int argc, char **argv) {
    if (argc != 1) {
        help(stderr, "dump");
        return 2;
    }
    size_t len = read_file(argv[0], unit_mem, sizeof(unit_mem));
    if (len == 0) {
        return 1;
    }
    if (fbc_kind(unit_mem, len) == 2) {
        static fbc_bundle b;
        char why[128];
        if (!fbc_read_bundle(&b, unit_mem, len, why, sizeof(why))) {
            return complain(why);
        }
        fbc_dump_bundle(&b, print_line, NULL);
        return 0;
    }
    if (!is_bytecode(unit_mem, len)) {
        len = build(argv, 1);
        if (len == 0) {
            return 1;
        }
    }
    char why[128];
    if (!fbc_read(&listing, unit_mem, len, why, sizeof(why))) {
        return complain(why);
    }
    fbc_dump(&listing, print_line, NULL);
    return 0;
}

/* FILE, or standard input when it is absent or "-", into dst. */
static size_t read_input(const char *path, void *dst, size_t cap) {
    if (path != NULL && strcmp(path, "-") != 0) {
        return read_file(path, dst, cap);
    }
    size_t n = fread(dst, 1, cap, stdin);
    if (n == cap) {
        (void)complain("standard input: too large");
        return 0;
    }
    if (n == 0) {
        (void)complain("standard input: empty");
    }
    return n;
}

static const char *base_name(const char *path) {
    const char *b = strrchr(path, '/');
    return b != NULL ? b + 1 : path;
}

static int cmd_check(int argc, char **argv) {
    offer o = {NULL, 0};
    int i = vm_flag(argc, argv, &o);
    if (i < 0) {
        return 1;
    }
    if (argc - i > 1 || (argc - i == 1 && argv[i][0] == '-' && argv[i][1] != '\0')) {
        help(stderr, "check");
        return 2;
    }
    const char *path = argc - i == 1 ? argv[i] : "-";
    size_t len = read_input(path, unit_mem, sizeof(unit_mem));
    if (len == 0) {
        return 1;
    }
    char why[160];
    int lacking =
        fbc_check(unit_mem, len, base_name(path), offered, &o, print_line, NULL, why, sizeof(why));
    if (lacking < 0) {
        return complain(why);
    }
    return lacking > 0 ? 1 : 0;
}

static int cmd_size(int argc, char **argv) {
    if (argc > 1 || (argc == 1 && argv[0][0] == '-' && argv[0][1] != '\0')) {
        help(stderr, "size");
        return 2;
    }
    const char *path = argc == 1 ? argv[0] : "-";
    size_t len = read_input(path, unit_mem, sizeof(unit_mem));
    if (len == 0) {
        return 1;
    }
    char why[160];
    if (!fbc_size(unit_mem, len, base_name(path), print_line, NULL, why, sizeof(why))) {
        return complain(why);
    }
    return 0;
}

enum { FMT_MEM = 64U << 20U }; /* touched only as it is used */

/* src laid out as filofmt does, in mem; NULL (said) when it does not fit. */
static const char *formatted(const char *src, size_t len, void *mem, size_t *out) {
    const char *text = filo_fmt(src, len, 2, 80, mem, FMT_MEM, out);
    if (text == NULL) {
        (void)complain("formatting needs more memory than it has");
    }
    return text;
}

static int cmd_fmt(int argc, char **argv) {
    bool rewrite = false;
    if (argc > 0) {
        rewrite = strcmp(argv[0], "-w") == 0;
    }
    int first = rewrite ? 1 : 0;
    if (rewrite && argc < 2) {
        help(stderr, "fmt");
        return 2;
    }
    void *mem = malloc(FMT_MEM);
    if (mem == NULL) {
        return complain("out of memory");
    }
    int code = 0;
    for (int i = first; i < argc || (i == first && argc == first); i++) {
        const char *path = i < argc ? argv[i] : "-";
        size_t len = read_input(path, unit_mem, sizeof(unit_mem));
        size_t n = 0;
        const char *text = len > 0 ? formatted((const char *)unit_mem, len, mem, &n) : NULL;
        if (text == NULL) {
            code = 1;
            continue;
        }
        if (!rewrite) {
            (void)fwrite(text, 1, n, stdout);
            continue;
        }
        if (n == len && memcmp(text, unit_mem, n) == 0) {
            continue; /* formatted already: the file is left as it is */
        }
        FILE *f = fopen(path, "wb");
        if (f == NULL || fwrite(text, 1, n, f) != n) {
            code = complain2("cannot write", path);
        }
        if (f != NULL && fclose(f) != 0) {
            code = complain2("cannot write", path);
        }
    }
    free(mem);
    return code;
}

/* The first line filo_show writes: the folded tree's root. */
static void first_line(void *user, const char *line) {
    char *keep = user;
    if (keep[0] == '\0') {
        (void)snprintf(keep, 128, "%s", line);
    }
}

/* Whether this command's folder turns call into a constant: the folded
   tree of it is a number, a bool or a string. */
static bool folds(void *user, const char *call, size_t len) {
    (void)user;
    char root[128] = "";
    if (filo_show(&ctx, (const uint8_t *)call, len, "folded", first_line, root) != FILO_OK) {
        return false;
    }
    const char *what = root;
    while (*what != '\0' && *what != ' ') {
        what++; /* past line:col */
    }
    while (*what == ' ') {
        what++;
    }
    if (strncmp(what, "number ", 7) == 0 || strncmp(what, "bool ", 5) == 0) {
        return true;
    }
    return strncmp(what, "string ", 7) == 0;
}

typedef struct {
    const char *dir;
    uint32_t count;
    uint32_t total;
    int code;
    void *fmt_mem;
} decompiled;

/* An entry point's text: on stdout under its file's name, or as that file
   in the directory, its path on stdout. */
static void put_source(void *user, const char *name, size_t nlen, const char *raw, size_t rawlen) {
    decompiled *w = user;
    size_t len = 0;
    const char *text = formatted(raw, rawlen, w->fmt_mem, &len);
    if (text == NULL) {
        w->code = 1;
        return;
    }
    if (w->dir != NULL) {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/%.*s.filo", w->dir, (int)nlen, name);
        FILE *f = fopen(path, "wb");
        if (f == NULL || fwrite(text, 1, len, f) != len) {
            w->code = complain2("cannot write", path);
            if (f != NULL) {
                fclose(f);
            }
            return;
        }
        if (fclose(f) != 0) {
            w->code = complain2("cannot write", path);
            return;
        }
        puts(path);
        return;
    }
    if (w->total > 1) {
        printf("%s; %.*s.filo\n", w->count > 0 ? "\n" : "", (int)nlen, name);
    }
    (void)fwrite(text, 1, len, stdout);
    w->count++;
}

static int cmd_decompile(int argc, char **argv) {
    decompiled w = {NULL, 0, 0, 0, NULL};
    int i = 0;
    if (argc >= 2 && strcmp(argv[0], "-o") == 0) {
        w.dir = argv[1];
        i = 2;
    }
    if (argc - i < 1 || argc - i > 2) {
        help(stderr, "decompile");
        return 2;
    }
    size_t len = read_input(argv[i], unit_mem, sizeof(unit_mem));
    if (len == 0) {
        return 1;
    }
    const uint8_t *unit = unit_mem;
    size_t ulen = len;
    if (fbc_kind(unit_mem, len) == 2) {
        static fbc_bundle b;
        char why[128];
        if (!fbc_read_bundle(&b, unit_mem, len, why, sizeof(why))) {
            return complain(why);
        }
        char name[256] = "main";
        if (argc - i == 2) {
            (void)snprintf(name, sizeof(name), "%s", argv[i + 1]);
        } else if (filo_bundle_find(&ctx, unit_mem, len, name, &unit, &ulen) != FILO_OK) {
            (void)snprintf(name, sizeof(name), "%.*s", (int)b.members[0].name.len,
                           (const char *)unit_mem + b.members[0].name.off);
        }
        if (filo_bundle_find(&ctx, unit_mem, len, name, &unit, &ulen) != FILO_OK) {
            return complain(filo_error(&ctx));
        }
    } else if (fbc_kind(unit_mem, len) != 1) {
        return complain("not a unit or a bundle: a source is Filo already");
    }
    char why[128];
    if (!fbc_read(&listing, unit, ulen, why, sizeof(why))) {
        return complain(why);
    }
    enum { DECOMPILE_MEM = 256U << 20U }; /* touched only as it is used */
    void *mem = malloc(DECOMPILE_MEM);
    w.fmt_mem = malloc(FMT_MEM);
    if (mem == NULL || w.fmt_mem == NULL) {
        free(mem);
        free(w.fmt_mem);
        return complain("out of memory");
    }
    w.total = listing.nexports;
    bool ok =
        fbc_decompile(&listing, mem, DECOMPILE_MEM, folds, NULL, put_source, &w, why, sizeof(why));
    free(mem);
    free(w.fmt_mem);
    if (!ok) {
        return complain(why);
    }
    return w.code;
}

/* Asking a command for help, anywhere in its arguments, is using it. */
static bool asks_help(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-help") == 0 ||
            strcmp(argv[i], "--help") == 0) {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        help(stderr, NULL);
        return 2;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        help(stdout, NULL);
        return 0;
    }
    for (int i = 0; i < NCOMMANDS; i++) {
        if (strcmp(argv[1], commands[i].name) == 0 && asks_help(argc - 2, argv + 2)) {
            help(stdout, commands[i].name);
            return 0;
        }
    }
    filo_libc_install();
    start();
    if (strcmp(argv[1], "run") == 0) {
        return cmd_run(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "build") == 0) {
        return cmd_build(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "show") == 0) {
        return cmd_show(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "bundle") == 0) {
        return cmd_bundle(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "dump") == 0) {
        return cmd_dump(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "check") == 0) {
        return cmd_check(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "size") == 0) {
        return cmd_size(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "decompile") == 0) {
        return cmd_decompile(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "fmt") == 0) {
        return cmd_fmt(argc - 2, argv + 2);
    }
    (void)complain2("unknown command", argv[1]);
    help(stderr, NULL);
    return 2;
}
