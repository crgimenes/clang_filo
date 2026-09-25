/* The disassembler against the runtime it describes. Its reader knows the
   format only from docs/bytecode.md, so every unit the compiler wrote for
   the corpus and the oracle (make device leaves them in build/units) must
   read, list and agree with the loader; and a constant must be written as
   the runtime writes it, for any double.

   usage: dump_test DIR */
#include <stdio.h>
#include <string.h>

#include "fbc_dump.h"
#include "filo.h"
#include "filo_libc.h"
#include "filo_math.h"
#include "filo_strings.h"

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static uint8_t data[1U << 20U];
static uint8_t persistent_mem[1U << 20U];
static uint8_t run_mem[1U << 20U];
static fbc_unit unit;
static filo_ctx ctx;
static size_t lines = 0;

static size_t broken = 0;

/* A listing of a unit the compiler wrote reads every instruction whole.
   It is also kept beside the unit (NNNNN.dump), for the Go engine's
   listing to be held to it (make govm). */
static void count(void *user, const char *line) {
    FILE *keep = user;
    if (keep != NULL) {
        (void)fputs(line, keep);
        (void)fputc('\n', keep);
    }
    lines++;
    if (strstr(line, "(an instruction cut short)") != NULL || strstr(line, "(unknown") != NULL ||
        strstr(line, "runs past the function") != NULL) {
        broken++;
    }
}

static uint64_t rng = 0x9E3779B97F4A7C15ULL;

static uint64_t next_rand(void) {
    rng += 0x9E3779B97F4A7C15ULL;
    uint64_t z = rng;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

static void test_numbers_read_as_the_runtime_writes_them(void) {
    int diff = 0;
    for (int i = 0; i < 200000; i++) {
        double x = 0;
        if (i % 2 == 0) {
            uint64_t bits = next_rand();
            memcpy(&x, &bits, sizeof(x));
        } else {
            x = (double)(int64_t)(next_rand() % 2000001U) - 1000000.0;
            x /= (double)(1U << (next_rand() % 12U));
        }
        char want[64];
        char got[64];
        size_t n = filo_libc_num_to_str(NULL, x, want, sizeof(want) - 1);
        want[n] = '\0';
        fbc_number(x, got, sizeof(got));
        if (strcmp(want, got) != 0) {
            if (diff < 5) {
                printf("  %a: runtime %s, listing %s\n", x, want, got);
            }
            diff++;
        }
    }
    printf("numbers: 200000 doubles, %d written differently\n", diff);
    CHECK(diff == 0);
}

static void test_units_list_and_agree_with_the_loader(const char *dir) {
    unsigned read = 0;
    for (unsigned no = 0;; no++) {
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%05u.fbc", dir, no);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            break;
        }
        size_t len = fread(data, 1, sizeof(data), f);
        fclose(f);
        char why[128];
        bool ok = fbc_read(&unit, data, len, why, sizeof(why));
        CHECK(ok);
        if (!ok) {
            printf("  %s: %s\n", path, why);
            continue;
        }
        CHECK(unit.checksum_ok);
        lines = 0;
        (void)snprintf(path, sizeof(path), "%s/%05u.dump", dir, no);
        FILE *keep = fopen(path, "w");
        fbc_dump(&unit, count, keep);
        if (keep != NULL) {
            (void)fclose(keep);
        }
        CHECK(lines > unit.nfns);
        CHECK(broken == 0);
        filo_init(&ctx, &filo_libc_host, persistent_mem, sizeof(persistent_mem), run_mem,
                  sizeof(run_mem));
        (void)filo_math_register(&ctx, &filo_libc_math);
        (void)filo_strings_register(&ctx, &filo_libc_strings);
        const filo_unit *u = NULL;
        CHECK(filo_bc_load_lazy(&ctx, data, len, &u) ==
              FILO_OK); /* corpus units: as the interpreter */
        for (uint32_t i = 0; i < unit.nexports; i++) {
            char name[256];
            fbc_span s = unit.export_names[i];
            (void)snprintf(name, sizeof(name), "%.*s", (int)s.len, (const char *)data + s.off);
            CHECK(u != NULL && filo_bc_has(u, name));
        }
        read++;
    }
    printf("units: %u read, listed and loaded\n", read);
    CHECK(read > 1000);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: dump_test DIR\n");
        return 2;
    }
    test_numbers_read_as_the_runtime_writes_them();
    test_units_list_and_agree_with_the_loader(argv[1]);
    if (failures > 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dump tests passed\n");
    return 0;
}
