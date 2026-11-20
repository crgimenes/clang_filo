/* Number text for hosts without a C library: wasm32, microcontrollers, and
   anything else with no strtod or printf. Pair it with the math pack's host
   table left empty, since the transcendental functions need libm too.

   Formatting follows the same rule as the libc host: the shortest decimal
   that reads back as the same double, written plainly when the exponent
   lands in [-4, 6) and in scientific form otherwise.

   Both ways are exact for every double, as Go's strconv is: reading rounds
   the decimal correctly however many digits it has, and writing gives the
   shortest digits that read back, the closest of them. Up to 15 significant
   digits with a power of ten within 22 either way — every value a script
   realistically holds — the conversion is a single multiply or divide by an
   exactly held power of ten; anything else goes through big integers on the
   stack (about 1 KB), tens of microseconds for magnitudes near 1e300 or
   1e-300. nolibc_test.c holds all of it to a libc host, which fails on any
   difference.

   %f for str-fmt works on the exact decimal expansion of the double too, so
   it matches printf and Go for every value and precision. */
#ifndef FILO_NOLIBC_H
#define FILO_NOLIBC_H

#include "filo.h"
#include "filo_strings.h"

size_t filo_nolibc_num_to_str(void *user, double x, char *dst, size_t cap);
bool filo_nolibc_str_to_num(void *user, const uint8_t *s, size_t len, double *out);

/* %f for str-fmt; no host math functions, so filo_math_register may be given
   a NULL table (or skipped entirely). */
extern const filo_strings_fns filo_nolibc_strings;

extern const filo_host filo_nolibc_host;

#endif
