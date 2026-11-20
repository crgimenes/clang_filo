/* Filo source laid out the way filofmt (the Go repository's) lays it out,
   byte for byte: a form that fits on the rest of the line stays on one
   line; one that does not keeps its leading children on the head line
   while they fit, and from the first that does not, every child takes a
   line of its own; special forms keep a fixed head (fn its params, let its
   bindings, def its name, if its condition, letv its names and value, a
   cond clause its test); cond, do and list read as a column; comments and
   blank lines stay where they are. make govm holds it to the Go one on
   every source of the corpus. Memory is the caller's: nothing is
   allocated, and nothing but memcpy and friends is called. */
#ifndef FILO_FMT_H
#define FILO_FMT_H

#include <stddef.h>
#include <stdint.h>

/* The source formatted, indent spaces a level and width columns a line
   (filofmt's are 2 and 80), laid out in mem (cap bytes): the text and its
   length, or NULL when mem is too small for it. The text ends with one
   newline, or is empty. */
const char *filo_fmt(const char *src, size_t len, uint32_t indent, uint32_t width, void *mem,
                     size_t cap, size_t *out_len);

#endif
