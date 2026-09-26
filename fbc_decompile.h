/* A unit back to Filo, from its bytes and docs/bytecode.md: the compiler
   writes each form in one shape, so the forms read back from the shapes (a
   stack machine run on forms instead of values, and the jumps of if, cond,
   and and or). What the bytes do not keep does not come back: comments,
   layout, the names of parameters and let bindings (made up: x y z, a b c,
   a digit for how deeply a function is nested), and forms that compile
   alike come back as one (an if chain as cond, a folded expression as its
   constant). Compiled again, the text is the same unit but for its debug
   section; the Go engine's decompiler (package fbc) writes the same text,
   character for character. Memory is the caller's: nothing is allocated. */
#ifndef FBC_DECOMPILE_H
#define FBC_DECOMPILE_H

#include <stdbool.h>
#include <stddef.h>

#include "fbc_dump.h"

/* Whether the compiler's folder turns the call in text, one form of Filo
   whose arguments are literals, into a constant. The decompiler asks, since
   folding decides where a sequence may go; a host answers with its own
   folder (filo_show's "folded" stage does). */
typedef bool (*fbc_folds)(void *user, const char *call, size_t len);

/* An entry point decompiled: its name and its text, a top-level form a line. */
typedef void (*fbc_source)(void *user, const char *name, size_t nlen, const char *text, size_t len);

/* Decompiles the unit u, each entry point in the unit's order (the order to
   compile them in for the same unit) to out, in mem (cap bytes). False, with
   the reason in why, for a unit that is not one this compiler writes, or when
   mem is too small. */
bool fbc_decompile(const fbc_unit *u, void *mem, size_t cap, fbc_folds folder, void *fold_user,
                   fbc_source out, void *out_user, char *why, size_t whycap);

#endif
