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
    FBC_SECTIONS_MAX = 64,
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

/* An entry of the section table: its kind and where it lies. */
typedef struct {
    uint32_t kind;
    fbc_span span;
} fbc_section;

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t version;
    uint32_t checksum;
    bool checksum_ok;
    uint32_t widest_stack;
    uint32_t widest_frame;
    uint32_t header_size;                   /* the fixed header and the section table */
    fbc_section sections[FBC_SECTIONS_MAX]; /* as the table lists them, the first 64 */
    uint32_t nsections;
    fbc_span imports[FBC_NAMES_MAX];
    uint32_t nimports;
    fbc_span globals[FBC_NAMES_MAX];
    uint32_t nglobals;
    bool externs[FBC_NAMES_MAX]; /* globals read and never written: the VM's to provide */
    uint32_t nexterns;
    uint32_t consts[FBC_CONSTS_MAX]; /* where each entry starts */
    uint32_t nconsts;
    fbc_fn fns[FBC_FNS_MAX];
    uint32_t nfns;
    fbc_span code;
    fbc_span export_names[FBC_EXPORTS_MAX];
    uint32_t export_fns[FBC_EXPORTS_MAX];
    uint32_t nexports;
    fbc_span debug; /* empty when the unit was stripped */
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

/* The source position of the instruction at pc, by the debug section;
   false without one. */
bool fbc_position(const fbc_unit *u, uint32_t pc, uint32_t *line, uint32_t *col);

/* A number as Filo writes it (Go's strconv, 'g', shortest). It takes the C
   library's printf and strtod; a host without them defines FBC_HOST_NUMBERS
   and supplies this function with its own number text (msh's wasm build,
   filo_nolibc_num_to_str). The rest of this file and fbc_decompile.c take
   only snprintf and vsnprintf for integers and strings. */
void fbc_number(double x, char *dst, size_t cap);

/* The whole listing, a line at a time. */
typedef void (*fbc_out)(void *user, const char *line);
void fbc_dump(const fbc_unit *u, fbc_out out, void *user);
/* A bundle's listing: its table, then every member's. */
void fbc_dump_bundle(const fbc_bundle *b, fbc_out out, void *user);

/* Whether a VM gives name: a function a unit imports, or an extern global. */
typedef bool (*fbc_offers)(void *user, const uint8_t *name, uint32_t len);

/* A line for each unit of the file in data (itself, or a bundle's members),
   label naming a unit file: "NAME  runs: I imports, E externs", or "NAME  lacks
   N: a, b" (the functions first) for what offered says the VM lacks. How many
   lack something, or -1 with the reason in why when data does not read. */
int fbc_check(const uint8_t *data, size_t len, const char *label, fbc_offers offered,
              void *offers_user, fbc_out out, void *user, char *why, size_t cap);

/* Where the bytes of the file in data go: a bundle's header and table, and
   each unit's header and sections in the order the file has them. False,
   with the reason in why, when data does not read. */
bool fbc_size(const uint8_t *data, size_t len, const char *label, fbc_out out, void *user,
              char *why, size_t cap);

#endif
