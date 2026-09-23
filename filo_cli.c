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

#include "fbc_dump.h"
#include "filo.h"
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

static const char usage[] =
    "usage: filo run [--vm | --trace] FILE [MEMBER] [ENTRY...]\n"
    "       filo build -o OUT FILE...\n"
    "       filo bundle -o OUT UNIT...\n"
    "       filo dump FILE\n"
    "\n"
    "Runs a Filo program and prints its value; builds programs into one unit\n"
    "of bytecode (docs/bytecode.md), each an entry named by its file; puts\n"
    "units into one bundle, each a member named by its file; lists a unit or\n"
    "a bundle. FILE is source, a unit or a bundle, told apart by the magic.\n"
    "\n"
    "  --vm      run source as bytecode, compiled in memory\n"
    "  --trace   run as bytecode and show every instruction with the stack\n"
    "  MEMBER    for a bundle: the unit to run (default: main, or the first)\n"
    "  ENTRY     for a unit: the entries to run, in order, sharing globals\n"
    "            (default: main, or the unit's first entry)\n"
    "\n"
    "Example:\n"
    "  echo '(str-upper \"ola mundo\")' > ola.filo\n"
    "  filo run ola.filo && filo build -o ola.fbc ola.filo && filo dump ola.fbc\n";

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
    return n;
}

static bool is_bytecode(const uint8_t *p, size_t n) {
    return fbc_kind(p, n) != 0;
}

/* "lib/ola.filo" is the entry "ola", and "lib/ola.fbc" the member "ola" */
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
            (void)complain2(paths[i], filo_error(&ctx));
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

static int show_value(const filo_value *v) {
    char text[4096];
    size_t n = 0;
    if (filo_value_text(&ctx, v, text, sizeof(text), &n) != FILO_OK) {
        return complain(filo_error(&ctx));
    }
    printf("%.*s\n", (int)n, text);
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
            return complain2(entries[i], filo_error(&ctx));
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

static int cmd_run(int argc, char **argv) {
    bool vm = false;
    bool trace = false;
    int i = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--vm") == 0) {
            vm = true;
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else {
            (void)complain2("unknown flag", argv[i]);
            (void)fputs(usage, stderr);
            return 2;
        }
    }
    if (i >= argc) {
        fputs(usage, stderr);
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
        return complain(filo_error(&ctx));
    }
    return show_value(&v);
}

static int cmd_build(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[0], "-o") != 0) {
        fputs(usage, stderr);
        return 2;
    }
    size_t len = build(argv + 2, argc - 2);
    if (len == 0) {
        return 1;
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
        (void)fputs(usage, stderr);
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

static int cmd_dump(int argc, char **argv) {
    if (argc != 1) {
        fputs(usage, stderr);
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

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fputs(usage, argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    filo_libc_install();
    start();
    if (strcmp(argv[1], "run") == 0) {
        return cmd_run(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "build") == 0) {
        return cmd_build(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "bundle") == 0) {
        return cmd_bundle(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "dump") == 0) {
        return cmd_dump(argc - 2, argv + 2);
    }
    (void)complain2("unknown command", argv[1]);
    (void)fputs(usage, stderr);
    return 2;
}
