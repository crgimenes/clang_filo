/* A reader and disassembler for Filo units, written from docs/bytecode.md
   alone: it shares no code with the loader in filo.c, so it is a second
   reading of the format, and it costs the runtime nothing. The listing is
   for people — every operand resolved to the name, constant or target it
   stands for — and the bytes are untrusted, as a loader's are. */
#ifndef FBC_DUMP_H
#define FBC_DUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    FBC_NAMES_MAX = 4096,
    FBC_CONSTS_MAX = 4096,
    FBC_FNS_MAX = 1024,
    FBC_EXPORTS_MAX = 64,
    FBC_MEMBERS_MAX = 256,
};

typedef struct {
    uint32_t off; /* from the start of the unit */
    uint32_t len;
} fbc_span;

typedef struct {
    uint32_t off; /* in the code section */
    uint32_t len;
    uint32_t params;
    uint32_t slots;
    uint32_t stack;
} fbc_fn;

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t version;
    uint32_t checksum;
    bool checksum_ok;
    uint32_t widest_stack;
    uint32_t widest_frame;
    fbc_span imports[FBC_NAMES_MAX];
    uint32_t nimports;
    fbc_span globals[FBC_NAMES_MAX];
    uint32_t nglobals;
    uint32_t consts[FBC_CONSTS_MAX]; /* where each entry starts */
    uint32_t nconsts;
    fbc_fn fns[FBC_FNS_MAX];
    uint32_t nfns;
    fbc_span code;
    fbc_span export_names[FBC_EXPORTS_MAX];
    uint32_t export_fns[FBC_EXPORTS_MAX];
    uint32_t nexports;
} fbc_unit;

/* A bundle: its header and where each member is. */
typedef struct {
    fbc_span name;
    fbc_span unit;
} fbc_member;

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t version;
    uint32_t checksum;
    bool checksum_ok;
    uint32_t widest_stack;
    uint32_t widest_frame;
    fbc_member members[FBC_MEMBERS_MAX];
    uint32_t n;
} fbc_bundle;

/* 1 for a unit, 2 for a bundle, 0 for anything else. */
uint32_t fbc_kind(const uint8_t *data, size_t len);

/* Reads a bundle's table into b, as fbc_read reads a unit. */
bool fbc_read_bundle(fbc_bundle *b, const uint8_t *data, size_t len, char *why, size_t cap);

/* Reads a unit's header and sections into u, which points into data. False,
   with the reason in why, when the bytes are not a unit this reader can
   list. A wrong checksum is reported in u, not refused: the listing of a
   damaged unit is exactly what someone looking at one wants. */
bool fbc_read(fbc_unit *u, const uint8_t *data, size_t len, char *why, size_t cap);

/* The instruction at pc of the code section, as text ("CALLB 1 print");
   returns its length in bytes, 0 when it cannot be read. */
uint32_t fbc_insn(const fbc_unit *u, uint32_t pc, char *dst, size_t cap);

/* A number as Filo writes it (Go's strconv, 'g', shortest). */
void fbc_number(double x, char *dst, size_t cap);

/* The whole listing, a line at a time. */
typedef void (*fbc_out)(void *user, const char *line);
void fbc_dump(const fbc_unit *u, fbc_out out, void *user);
/* A bundle's listing: its table, then every member's. */
void fbc_dump_bundle(const fbc_bundle *b, fbc_out out, void *user);

#endif
