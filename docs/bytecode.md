# Filo bytecode — a program as a file

The IR in [ir.md](ir.md) is a tree that lives in the memory of the runtime
that lowered it: 72 bytes a node on a 32-bit target, 16 times the source.
A small machine cannot hold that, and does not need to — it can run code it
did not compile. This document is the format that makes that possible: a
compiler turns the IR into a flat stream of instructions for a stack
machine, and a VM runs the stream in place, from wherever it is stored
(flash, a file, a browser's storage). The machine that runs a program needs
the VM and the builtins the program imports; it never needs the parser.

The bytecode is a C runtime feature. The Go engine does not read or write
it (yet), and the language does not change: a program compiled here gives
the same value, the same error or success and the same globals as the IR
evaluated by either runtime. The corpus and the Prolog oracle run through
the VM to hold it to that.

## What differs from the IR

- **Steps.** A step is one executed instruction, not one IR node, so the
  count differs from [ir.md](ir.md)'s. What stays is the purpose: a runaway
  program stops. The corpus keeps wide margins on its step limits, and the
  counts are close (0.93 instructions per IR node, measured statically).
- **Error context.** Messages are the IR's, but the chain of forms around
  them (`in if: in let: …`) is not rebuilt; builtin failures keep
  `in builtin "<name>": `. The corpus asserts that an error occurred, never
  its wording.
- **Line and column.** The IR does not carry them either; a debug section
  is reserved for them.
- **When a call is checked.** The IR checks that the callee is a function,
  its arity and the recursion depth before it evaluates the arguments; on a
  stack machine the arguments are already evaluated when `CALL` runs. The
  outcome is the same — the run fails and leaves no globals behind — and
  only a failing call with an argument that has an effect outside the run
  (a builtin that draws or prints) can tell the two apart.

## Units

A **unit** is what shares one set of globals: for a screen of the board,
its common file, its init and its hooks. It holds any number of entry
points (exports) — each program compiled into it is one, with a name — and
the functions they create.

Everything a unit refers to outside itself is by name, resolved when it is
loaded:

- **Imports** are builtins, the core ones included, so that adding a
  builtin to the core never renumbers anything. A name the loading context
  does not have fails the load: `missing builtin: <name>`. The imports of a
  unit are therefore its capability list.
- **Globals** are names, resolved to the loading context's symbol table
  (created when the table is open; a sealed table without the name fails
  the load: `undefined global: <name>`).

Inside the unit everything is an index: constants, globals, imports,
functions.

## File layout

All integers are little-endian. Counts, indices and lengths inside sections
are ULEB128 (7 bits a byte, low first, high bit set on every byte but the
last).

    offset  size  field
    0       4     magic 7F 46 42 43 ("\x7fFBC": the first byte is not ASCII,
                  so no source text can start this way)
    4       1     kind: 1 = unit (2 = bundle, reserved)
    5       1     format version: 1
    6       2     header size, in bytes; a reader skips what it does not know
    8       4     FNV-1a 32 of the whole file, computed with this field zero
    12      2     widest operand stack of any function, in values
    14      2     widest frame of any function, in slots
    16      2     number of sections
    18      2     reserved, zero
    20      12×n  section table: kind (u16), reserved (u16), offset (u32),
                  length (u32) — offsets from the start of the file

The first 20 bytes are the fixed header; the section table follows it and
the header size covers both.

Sections, each at most once; unknown kinds are skipped:

| kind | name | content |
|---:|---|---|
| 1 | imports | count, then names (length, bytes) |
| 2 | globals | count, then names |
| 3 | constants | count, then entries: tag byte, payload |
| 4 | functions | count, then per function: code offset, code length, params, frame slots, max stack |
| 5 | code | the instruction bytes of every function |
| 6 | exports | count, then per export: name, function index |
| 7 | debug | reserved |

Constant tags: 1 number (8 bytes, IEEE 754 double), 2 string (length,
bytes), 3 true, 4 false, 5 empty list.

A function's code runs from its offset for its length inside the code
section; a jump or a fall-through that leaves that range is an error. Frame
slots are the parameters first, then one slot for every `let`/`letv`
binding in the function, never reused: a closure created in one `let`
keeps seeing that `let`'s slot, as it keeps seeing that `let`'s frame in
the IR.

## Instructions

The first byte of an instruction is five bits of opcode and three of
immediate: `opcode << 3 | imm`, the way the Z80 keeps registers and
conditions in the bits of its opcodes (`LD r,r'` is `01 ddd sss`). An
immediate of 0 to 6 is the operand; 7 means the operand follows as a
ULEB128. Where an instruction needs a second operand, it follows as a
ULEB128. Jumps carry a fixed 2-byte signed offset, counted from the end of
the jump instruction.

| op | name | immediate | then | stack | effect |
|---:|---|---|---|---|---|
| 0 | `PUSH_K` | constant | | → v | push the constant |
| 1 | `PUSH_G` | global | | → v | push the global; `undefined global: <name>` when unset |
| 2 | `STORE_G` | global | | v → v | set the global, keep the value |
| 3 | `PUSH_L` | slot | | → v | push a slot of the current frame |
| 4 | `STORE_L` | slot | | v → v | set a slot of the current frame, keep the value |
| 5 | `PUSH_UP` | depth | slot | → v | push a slot of the frame *depth* functions out |
| 6 | `STORE_UP` | depth | slot | v → v | set it, keep the value |
| 7 | `POP` | count | | v… → | drop values |
| 8 | `JMP` | condition | offset (2 bytes) | see below | jump |
| 9 | `CALL` | argc | | f a… → r | call a function value |
| 10 | `CALLB` | argc | import | a… → r | call a builtin |
| 11 | `RET` | 0 return, 1 exit | | v → | leave the function, or end the run |
| 12 | `CLOSURE` | function | | → f | a function value capturing the current frame |
| 13 | `TUPLE` | count | | a… → t | a tuple of the values |
| 14 | `UNPACK` | count | | t → a… | the elements of a tuple of exactly *count*; `letv expects tuple expression` / `letv arity mismatch` |
| 15 | `TRAP` | constant | | | error with the string constant as message |

`JMP` conditions — every conditional one requires a bool
(`expected bool, got <kind>`):

| imm | used by | effect |
|---:|---|---|
| 0 | `if`, `cond` | always jump |
| 1 | `if`, `cond` | pop; jump when false |
| 2 | `and` | jump when false, keeping the value; else pop |
| 3 | `or` | jump when true, keeping the value; else pop |
| 4 | `and`, `or` | check the last operand is a bool; never jump |

`CALL` follows [ir.md](ir.md): the value must be a function
(`attempt to call non-function (got <kind>)`), the recursion depth is
checked, the argument count must match (`function expects <n> arguments,
got <m>`), and the arguments fill the first slots of a new frame whose
parent is the frame the function captured. `RET 0` in a function returns
its value; at the top of an entry point it ends the run, as `RET 1` does
anywhere. An entry point ends with `RET 0`.

Every structural error the IR raises lazily — a malformed form, `if` with
the wrong number of arguments, a `cond` clause that is not a clause — is a
`TRAP` at the point where the IR would raise it, after everything the IR
would have evaluated before it.

## The device build

A machine that only runs units compiles `filo.c` with `FILO_VM_ONLY` defined
(the whole build, `filo.h` included). The parser, the IR, the tree-walking
evaluator and the compiler are left out, and so are `filo_compile`,
`filo_run` and `filo_bc_build`; what stays is the values, the globals, the
builtins, the loader and the VM. Measured with the ESP32-S3's gcc at `-Os`:
16,945 bytes of code and constants against 30,184 for the full runtime.

`make device` holds it to the full build: the full build writes every corpus
and oracle case as a unit, with what the run gave, and the device build,
with the libc-free number host and no libm, must give the same for each.

## Loading is a trust boundary

A unit may come from anywhere — a serial line, a card, a download — so the
loader and the VM treat it as untrusted: the checksum is verified, every
section is bounds-checked, and every index, slot, jump and stack access is
checked when it is used. A corrupt or hostile file fails with an error; it
never reads or writes outside what it was given.
