/* What fbc_dump.c and fbc_decompile.c take from a C library, declared for
   a host without one to supply: text of integers and strings (%u %d %x %c
   %s %.*s %lld %zu, width, '-' and '0'), never of numbers — those come
   from fbc_number, which such a host supplies too (FBC_HOST_NUMBERS). */
#ifndef FILO_FREESTANDING_STDIO_H
#define FILO_FREESTANDING_STDIO_H

#include <stdarg.h>
#include <stddef.h>

int snprintf(char *dst, size_t cap, const char *fmt, ...);
int vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap);

#endif
