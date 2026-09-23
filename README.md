# Filo, the C runtime

The [Filo](https://github.com/crgimenes/filo) language as a C library: the same
six types, fourteen special forms and thirty-one core builtins as the Go engine,
lowered to the instruction set in [docs/ir.md](docs/ir.md) and evaluated by the
same rules, so a script's outcome is identical on both.

Two files are the whole core — `filo.h` and `filo.c` — with no libc beyond
`memcpy`/`memset`/`memcmp`/`strlen`/`strcmp`/`strchr` and no allocation after
init. It builds for a freestanding wasm32 target and for microcontrollers.

A program can also be compiled to bytecode ([docs/bytecode.md](docs/bytecode.md)):
a unit of a few kilobytes that a stack machine runs in place, from flash or
from wherever its bytes live, with no parser and no IR in memory. The ten
screens of the author's board take 766 KB of IR and 25 KB as units. Its
imports are its capability list — a context without one of them refuses the
unit — and a unit is treated as untrusted input: checksummed, bounds-checked
and fuzzed. Every corpus case and the Prolog oracle run through both the IR
and the bytecode. A bundle puts several units, whole and named, in one file
that travels as one.

Built with `-DFILO_VM_ONLY`, the runtime keeps only what runs a unit: no
parser, no IR, no compiler. On an ESP32-S3 that is 17 KB of code instead of
30 KB. `make device` runs the corpus and the oracle, compiled by the full
build, on that build. `FILO_SYMBOLS_MAX` (512 by default, about 40 bytes a
name in every context) is a build setting too; the device check runs with
128.

`filo show`, `filo run` (`--both`, `--trace`), `filo build`, `filo bundle` and `filo dump` show the road
from source to the machine: `examples/` starts at "olá mundo".

## Memory

The host hands over two blocks and the runtime never asks for more:

- **persistent arena** — compiled programs, the symbol table, the globals. It
  only grows.
- **run arena** — everything a single run creates (frames, lists, strings,
  closures). Reset at the start of the next run.

Globals a run writes are copied into the persistent arena when the run ends, so
they survive; the result value stays valid until the next run or compile. A
script that exhausts either arena fails with an error and never corrupts
anything. Steps, recursion and parse depth are bounded (`filo_limits`), and the
`should_stop` hook lets the host end a run at any step — a runaway script cannot
take the host with it.

```c
#include <stdio.h>
#include <string.h>

#include "filo.h"
#include "filo_libc.h"

static uint8_t persistent[64 * 1024], run[64 * 1024];

int main(void) {
    filo_libc_install(); /* libm-backed pow; once per process */

    filo_ctx ctx;
    filo_init(&ctx, &filo_libc_host, persistent, sizeof persistent, run, sizeof run);
    filo_set_global(&ctx, "score", filo_num(42));

    const char *src = "(+ score 1)";
    filo_prog prog;
    if (filo_compile(&ctx, (const uint8_t *)src, strlen(src), &prog) != FILO_OK) {
        fprintf(stderr, "%s\n", filo_error(&ctx));
        return 1;
    }

    filo_value out;
    if (filo_run(&ctx, &prog, NULL, &out) != FILO_OK) {
        fprintf(stderr, "%s\n", filo_error(&ctx));
        return 1;
    }
    printf("%g\n", out.u.num); /* 43 */
    return 0;
}
```

`filo_seal_globals` closes the set of globals: from there on a script naming a
global the host never created fails to compile instead of creating one silently.
It is off unless asked for, and has no counterpart in the Go engine, which no
host has needed it for.

## Hosts

Number formatting and parsing are host hooks, because doing them exactly
(shortest round-trip text, Go's `ParseFloat` rules) needs either a libc or a
deliberate algorithm.

| file | what it gives | for |
| --- | --- | --- |
| `filo_libc.c` | `snprintf`/`strtod` formatting, libm `pow` | any host with a C library |
| `filo_nolibc.c` | the same number text, written out | freestanding targets |

The whole corpus runs against both. The libc-free host skips exactly the cases
that need libm — a power with a fractional exponent, and the transcendental
functions of the `math` pack — and answers everything else identically.

## Packs

The `math` and `strings` packs are one opt-in file each (`filo_math.c`,
`filo_strings.c`), freestanding too. Transcendental functions and `%f`
formatting come from the host through a small table that `filo_libc.c` fills
from libm; a host that passes a partial table registers only what it can
sustain. Case mapping covers ASCII and the Latin-1 letters.

## Building

`make` builds `build/corpus_runner`. Everything else is a QA target:

| target | what it does |
| --- | --- |
| `make corpus` | the corpus under ASan/UBSan, through the IR and the bytecode |
| `make corpus-nolibc` | the same corpus with the libc-free host |
| `make oracle` | the Prolog spec's answers, exported from the Go repository |
| `make oracle-regen` | rewrites them; needs the Go checkout beside this one |
| `make steps-regen` | the steps each corpus case takes on the Go engine, which `make corpus` holds the IR to |
| `make api` | host API behavior the corpus cannot express |
| `make nolibc` | the libc-free number text against the libc one |
| `make device` | the corpus and the oracle as units, on the `FILO_VM_ONLY` build |
| `make cli` | the `filo` command, its listing and trace against the runtime and `testdata/cli` |
| `make fmt-check` / `make fmt` | clang-format |
| `make tidy` / `make check` | clang-tidy and cppcheck, warnings as errors |
| `make freestanding` | wasm32 objects, `-ffreestanding -nostdlib` |
| `make fuzz` | libFuzzer on source, `FUZZ_SECONDS=15` by default |
| `make fuzz-bc` | libFuzzer on units, seeded with the corpus compiled |
| `make bench` | `fib 15`, the Go `BenchmarkRecursiveCall` script |
| `make qa` | all of the above; the gate before anything is called done |

clang-format, clang-tidy and the freestanding build come from LLVM; the
Makefile looks for them under `/opt/homebrew/opt/llvm/bin` and takes `LLVM=` to
point elsewhere. `make freestanding` uses the declarations in
`freestanding/include/string.h` — the six functions the core and the packs call,
which the embedding host defines.

## The corpus

`testdata/corpus/*.txt` pins what a script evaluates to: plain-text cases
asserting outcomes — the resulting value, the fact that an error occurred, the
globals left behind — never the wording of a message. Error messages are a
Go-side promise; both runtimes only have to agree on what happens.

**The corpus is shared with the Go repository and duplicated here.** It is the
contract between the two implementations, so a case added or changed on either
side belongs on both:

```bash
diff -ru testdata/corpus ../filo/testdata/corpus
```

Format and conventions: [testdata/corpus/README.md](testdata/corpus/README.md).

## More of my projects

- [filo](https://github.com/crgimenes/filo): the language and its Go engine.
- [kutta](https://github.com/crgimenes/kutta): a 2D wind tunnel; watch air misbehave around an airfoil.
- [glaze](https://github.com/crgimenes/glaze): WebView desktop apps in Go, cgo-free.
- [compterm](https://github.com/crgimenes/compterm): share your terminal over the network.
- [native](https://github.com/crgimenes/native): cgo-free Go bindings for OS APIs: clipboard, mmap, keep-awake, and friends.

More at [github.com/crgimenes](https://github.com/crgimenes) and [crg.eti.br](https://crg.eti.br).
