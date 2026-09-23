/* libFuzzer entry for the bytecode loader and machine: any byte string is a
   unit someone handed the board over a wire or a card. The checksum is
   recomputed before loading, or every mutation would stop at the first
   check instead of reaching the sections and the machine. Nothing may
   fault; a bad unit may only fail with an error. The listing reads the same
   bytes with its own reader, and the trace hook decodes every instruction
   the machine reaches, so both are held to the same rule. */
#include <stddef.h>
#include <stdint.h>
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
    listed = fbc_read(&listing, unit, size, why, sizeof(why));
    if (listed) {
        fbc_dump(&listing, discard, NULL);
    }
    filo_libc_install();
    filo_init(&ctx, &filo_libc_host, persistent_mem, sizeof(persistent_mem), run_mem,
              sizeof(run_mem));
    (void)filo_math_register(&ctx, &filo_libc_math);
    (void)filo_strings_register(&ctx, &filo_libc_strings);
    ctx.host.trace = follow;
    const filo_unit *u = NULL;
    if (filo_bc_load(&ctx, unit, size, &u) != FILO_OK) {
        return 0;
    }
    filo_limits limits = {20000U, 64U};
    filo_value v;
    (void)filo_bc_run(&ctx, u, "main", &limits, &v);
    (void)filo_bc_run(&ctx, u, "main", &limits, &v); /* a second run on the same unit */
    return 0;
}
