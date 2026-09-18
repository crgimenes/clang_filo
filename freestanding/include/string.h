/* Declarations for the freestanding wasm32 build, which has no C library:
   exactly the six functions the core and the packs call. The host supplies
   the definitions (the msh terminal does it in host/wasm/libc.c). */
#ifndef FILO_FREESTANDING_STRING_H
#define FILO_FREESTANDING_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strchr(const char *s, int c);

#endif
