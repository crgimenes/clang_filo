/* libFuzzer entry for the bytecode loader and machine: any byte string is a
   unit someone handed the board over a wire or a card. The checksum is
   recomputed before loading, or every mutation would stop at the first
   check instead of reaching the sections and the machine. Nothing may
   fault; a bad unit may only fail with an error. The listing reads the same
   bytes with its own reader, and the trace hook decodes every instruction
   the machine reaches, so both are held to the same rule. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fbc_dump.h"
#include "filo.h"
#include "filo_libc.h"
#include "filo_math.h"
#include "filo_strings.h"

static uint8_t persistent_mem[1U << 20U];
static uint8_t run_mem[1U << 20U];
static uint8_t unit[1U << 16U];
static filo_ctx ctx;
static fbc_unit listing;
static fbc_bundle bundle;
static bool listed = false;

static void discard(void *user, const char *line) {
    (void)user;
    (void)line;
}

static void follow(void *user, const filo_trace *t) {
    (void)user;
    if (listed) {
        char text[160];
        (void)fbc_insn(&listing, t->pc, text, sizeof(text));
    }
    for (uint32_t i = 0; i < t->depth; i++) {
        (void)t->stack[i].kind; /* the stack the hook sees is all readable */
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 12 || size > sizeof(unit)) {
        return 0;
    }
    memcpy(unit, data, size);
    memset(unit + 8, 0, 4);
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < size; i++) {
        h ^= unit[i];
        h *= 16777619U;
    }
    for (unsigned i = 0; i < 4U; i++) {
        unit[8 + i] = (uint8_t)(h >> (8U * i));
    }
    char why[128];
    const uint8_t *code = unit;
    size_t code_len = size;
    if (fbc_read_bundle(&bundle, unit, size, why, sizeof(why))) {
        fbc_dump_bundle(&bundle, discard, NULL);
    }
    filo_libc_install();
    filo_init(&ctx, &filo_libc_host, persistent_mem, sizeof(persistent_mem), run_mem,
              sizeof(run_mem));
    (void)filo_math_register(&ctx, &filo_libc_math);
    (void)filo_strings_register(&ctx, &filo_libc_strings);
    ctx.host.trace = follow;
    /* a bundle: its first member runs, found the way a host finds one */
    if (unit[4] == 2 && bundle.n > 0) {
        char name[260];
        const fbc_member *m = &bundle.members[0];
        (void)snprintf(name, sizeof(name), "%.*s", (int)m->name.len,
                       (const char *)unit + m->name.off);
        if (filo_bundle_find(&ctx, unit, size, name, &code, &code_len) != FILO_OK) {
            return 0;
        }
    }
    listed = fbc_read(&listing, code, code_len, why, sizeof(why));
    if (listed) {
        fbc_dump(&listing, discard, NULL);
    }
    const filo_unit *u = NULL;
    (void)filo_bc_load(&ctx, code, code_len, &u); /* the refusal, and its list of names */
    if (filo_bc_load_lazy(&ctx, code, code_len, &u) != FILO_OK) {
        return 0;
    }
    filo_limits limits = {20000U, 64U};
    filo_value v;
    int whole = filo_bc_run(&ctx, u, "main", &limits, &v);
    char text[2][256];
    char err[2][FILO_ERROR_MAX];
    size_t n = 0;
    size_t full[2] = {0, 0}; /* repr says the whole length, and writes what fits */
    bool shown[2] = {false, false};
    if (whole == FILO_OK && filo_value_repr(&ctx, &v, text[0], sizeof(text[0]), &n) == FILO_OK) {
        shown[0] = true;
    }
    full[0] = shown[0] ? n : 0;
    text[0][full[0] < sizeof(text[0]) ? full[0] : sizeof(text[0]) - 1] = '\0';
    (void)snprintf(err[0], sizeof(err[0]), "%s", filo_error(&ctx));
    (void)filo_bc_run(&ctx, u, "main", &limits, &v); /* a second run on the same unit */

    /* the same unit on a fresh context, a few instructions at a time, must
       end as the run in one go ended */
    filo_init(&ctx, &filo_libc_host, persistent_mem, sizeof(persistent_mem), run_mem,
              sizeof(run_mem));
    (void)filo_math_register(&ctx, &filo_libc_math);
    (void)filo_strings_register(&ctx, &filo_libc_strings);
    if (filo_bc_load_lazy(&ctx, code, code_len, &u) != FILO_OK) {
        __builtin_trap(); /* it loaded a moment ago */
    }
    uint32_t budget = 1U + (uint32_t)(size % 5U);
    int sliced = filo_bc_start(&ctx, u, "main", &limits, budget, &v);
    while (sliced == FILO_PAUSED) {
        sliced = filo_bc_resume(&ctx, budget, &v);
    }
    if (sliced == FILO_OK && filo_value_repr(&ctx, &v, text[1], sizeof(text[1]), &n) == FILO_OK) {
        shown[1] = true;
    }
    full[1] = shown[1] ? n : 0;
    text[1][full[1] < sizeof(text[1]) ? full[1] : sizeof(text[1]) - 1] = '\0';
    (void)snprintf(err[1], sizeof(err[1]), "%s", filo_error(&ctx));
    if (whole != sliced || shown[0] != shown[1] || full[0] != full[1] ||
        strcmp(text[0], text[1]) != 0 || strcmp(err[0], err[1]) != 0) {
        __builtin_trap();
    }
    return 0;
}
