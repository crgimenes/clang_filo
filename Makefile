# Filo C runtime: host build, QA gate and the corpus check.
CC ?= cc
LLVM ?= /opt/homebrew/opt/llvm/bin
WARN = -Wall -Wextra -Werror -Wshadow -Wconversion -Wdouble-promotion -Wundef
CFLAGS ?= -std=c11 -O2 $(WARN)
CORE = filo.c
PACKS = filo_math.c filo_strings.c
HOST = filo_libc.c
NOLIBC = filo_nolibc.c
HDRS = filo.h filo_libc.h filo_math.h filo_strings.h filo_nolibc.h
CORPUS = testdata/corpus/*.txt
ORACLE = testdata/oracle/*.txt
FILO_GO ?= ../filo
TIDY_CHECKS = bugprone-*,cert-*,clang-analyzer-*,readability-*,-readability-magic-numbers,-readability-function-cognitive-complexity,-readability-identifier-length,-readability-braces-around-statements,-bugprone-easily-swappable-parameters,-cert-err33-c,-readability-else-after-return,-readability-avoid-nested-conditional-operator,-readability-math-missing-parentheses,-cert-dcl03-c,-readability-uppercase-literal-suffix

.PHONY: all corpus corpus-nolibc oracle oracle-regen api nolibc device cli cli-regen fmt fmt-check tidy check qa clean freestanding fuzz fuzz-bc bench

all: build/corpus_runner

build/corpus_runner: $(CORE) $(PACKS) $(HOST) $(NOLIBC) corpus_runner.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(CORE) $(PACKS) $(HOST) $(NOLIBC) corpus_runner.c -lm

# The libc-free number host against the libc one, on a machine with both.
nolibc: $(CORE) $(PACKS) $(HOST) $(NOLIBC) nolibc_test.c $(HDRS)
	@mkdir -p build
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(WARN) \
		-o build/nolibc_test $(CORE) $(PACKS) $(HOST) $(NOLIBC) nolibc_test.c -lm
	./build/nolibc_test

# Host API behavior the corpus cannot express, under the same sanitizers.
api: $(CORE) $(PACKS) $(HOST) api_test.c $(HDRS)
	@mkdir -p build
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(WARN) \
		-o build/api_test $(CORE) $(PACKS) $(HOST) api_test.c -lm
	./build/api_test

# The corpus under sanitizers: the C runtime must agree with the Go engine
# on every case, and must do it without a single memory fault.
corpus: $(CORE) $(PACKS) $(HOST) $(NOLIBC) corpus_runner.c $(HDRS)
	@mkdir -p build
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(WARN) \
		-o build/corpus_runner_san $(CORE) $(PACKS) $(HOST) $(NOLIBC) corpus_runner.c -lm
	./build/corpus_runner_san $(CORPUS)
	./build/corpus_runner_san --vm $(CORPUS)

# The same corpus with the libc-free host in place: a runtime that answers
# differently depending on who formats its numbers would be two runtimes.
corpus-nolibc: corpus
	./build/corpus_runner_san --nolibc $(CORPUS)
	./build/corpus_runner_san --nolibc --vm $(CORPUS)

# What the Prolog spec of the Go repository answers, exported there in the
# corpus format: the C runtime answers to the oracle the Go engine answers to.
# Generated, so it lives apart from the corpus, which is written by hand and
# kept identical in both repositories.
oracle: corpus
	./build/corpus_runner_san $(ORACLE)
	./build/corpus_runner_san --nolibc $(ORACLE)
	./build/corpus_runner_san --vm $(ORACLE)
	./build/corpus_runner_san --nolibc --vm $(ORACLE)

# The device build: the core without its front end (FILO_VM_ONLY: no parser,
# no IR, no compiler), the libc-free number host, no libm, and the symbol
# table a small board sizes down to. It runs the corpus and the oracle as
# units the full build compiled, and must give what the full build gave on
# every one.
device: all device_test.c
	@rm -rf build/units && mkdir -p build/units
	@./build/corpus_runner --nolibc --vm --write-units build/units $(CORPUS) $(ORACLE) > /dev/null
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(WARN) -DFILO_VM_ONLY -DFILO_SYMBOLS_MAX=128 \
		-o build/device_test $(CORE) $(PACKS) $(NOLIBC) device_test.c
	./build/device_test build/units

# The filo command, the listing and the trace. The listing reads units with
# a reader of its own (fbc_dump.c, from docs/bytecode.md alone), so it is
# held to the loader on every unit the corpus and the oracle compile to, and
# to the runtime on how numbers are written. What the examples show is kept
# in testdata/cli: a change to the compiler is a change to what a lesson
# shows, and it shows up here (make cli-regen rewrites them).
CLI_CASES = ola.run ola.dump fib.run fib.dump dobro.dump dobro.trace demo.run demo.dump
CLI_SRC = $(CORE) $(PACKS) $(HOST) fbc_dump.c

build/filo: $(CLI_SRC) filo_cli.c fbc_dump.h $(HDRS)
	@mkdir -p build
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(WARN) \
		-o build/filo $(CLI_SRC) filo_cli.c -lm

# the demo bundle: two examples, built and bundled as a lesson would
build/cli/demo.fbb: build/filo examples/ola.filo examples/dobro.filo
	@mkdir -p build/cli
	./build/filo build -o build/cli/ola.fbc examples/ola.filo
	./build/filo build -o build/cli/dobro.fbc examples/dobro.filo
	./build/filo bundle -o $@ build/cli/ola.fbc build/cli/dobro.fbc

cli: device build/filo build/cli/demo.fbb dump_test.c
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(WARN) \
		-o build/dump_test $(CLI_SRC) dump_test.c -lm
	./build/dump_test build/units
	@for c in $(CLI_CASES); do \
		./build/filo $$(sh testdata/cli/args.sh $$c) > build/cli.out 2>&1; \
		diff -u testdata/cli/$$c build/cli.out || exit 1; \
	done; echo "cli: $(words $(CLI_CASES)) outputs as kept"

cli-regen: build/filo build/cli/demo.fbb
	@for c in $(CLI_CASES); do ./build/filo $$(sh testdata/cli/args.sh $$c) > testdata/cli/$$c 2>&1; done

# Rewrites the oracle files from the spec; needs the Go checkout beside this one.
oracle-regen:
	cd $(FILO_GO)/conformance && \
		FILO_ORACLE_OUT=$(CURDIR)/testdata/oracle go test -run TestExportOracle -count=1 .

fmt:
	$(LLVM)/clang-format -i *.c *.h

fmt-check:
	$(LLVM)/clang-format --dry-run --Werror *.c *.h

tidy:
	$(LLVM)/clang-tidy --quiet --warnings-as-errors='*' \
		--checks='$(TIDY_CHECKS)' \
		$(CORE) $(PACKS) $(HOST) $(NOLIBC) corpus_runner.c bench.c fuzz.c fuzz_bc.c api_test.c \
		nolibc_test.c fbc_dump.c filo_cli.c dump_test.c -- -std=c11
	$(LLVM)/clang-tidy --quiet --warnings-as-errors='*' \
		--checks='$(TIDY_CHECKS)' \
		$(CORE) device_test.c -- -std=c11 -DFILO_VM_ONLY

check:
	cppcheck --enable=warning,style,performance,portability --inline-suppr \
		--suppress=missingIncludeSystem --error-exitcode=1 $(CORE) $(PACKS) $(HOST) $(NOLIBC) corpus_runner.c bench.c fuzz.c fuzz_bc.c \
		api_test.c nolibc_test.c device_test.c fbc_dump.c filo_cli.c dump_test.c

# Proves the core and the packs need nothing from libc but memcpy/memcmp/
# strlen/strchr: the same freestanding wasm32 target the msh terminal uses.
freestanding: $(CORE) $(PACKS) $(NOLIBC) $(HDRS)
	@mkdir -p build
	@for f in $(CORE) $(PACKS) $(NOLIBC); do \
		$(LLVM)/clang --target=wasm32 -ffreestanding -nostdlib -fno-builtin -c \
			-std=c11 -O2 $(WARN) -Wno-unused-function -isystem freestanding/include \
			-o build/$${f%.c}_wasm.o $$f || exit 1; \
		echo "freestanding wasm32 object: build/$${f%.c}_wasm.o"; \
	done
	$(LLVM)/clang --target=wasm32 -ffreestanding -nostdlib -fno-builtin -c -DFILO_VM_ONLY \
		-std=c11 -O2 $(WARN) -Wno-unused-function -isystem freestanding/include \
		-o build/filo_vm_wasm.o $(CORE)

FUZZ_SECONDS ?= 15
# Seconds one input may take. Every input runs in well under a millisecond
# here, so one that takes seconds is a finding, not a slow machine: it once
# ate the whole budget unseen (str-fmt walking a four-billion-column width).
FUZZ_TIMEOUT ?= 2

# Any byte string is a script and the runtime must not fault on any of them.
# Seeds are the corpus scripts plus testdata/fuzz, the inputs that once went
# wrong; a finding is written as build/fuzz_crash_* (or fuzz_crash_timeout-*).
fuzz: $(CORE) $(PACKS) $(HOST) fuzz.c fuzz.dict $(HDRS)
	@mkdir -p build/fuzz_seeds build/fuzz_corpus
	@awk '/^=== /{n++; f=sprintf("build/fuzz_seeds/%04d", n); next} \
		/^--- /{if (f != "") close(f); f=""; next} \
		/^(given |limits )/{next} f!=""{print > f}' $(CORPUS)
	$(LLVM)/clang -std=c11 -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all $(WARN) \
		-o build/fuzz $(CORE) $(PACKS) $(HOST) fuzz.c -lm
	@./build/fuzz -max_total_time=$(FUZZ_SECONDS) -timeout=$(FUZZ_TIMEOUT) -max_len=2048 \
		-dict=fuzz.dict -artifact_prefix=build/fuzz_crash_ \
		build/fuzz_corpus build/fuzz_seeds testdata/fuzz > build/fuzz.log 2>&1 \
		|| { tail -30 build/fuzz.log; exit 1; }
	@tail -2 build/fuzz.log

# A unit is input from anywhere, so the loader and the machine are fuzzed
# with units: the corpus compiled to bytecode as seeds, mutated from there.
# The target is the device build, which is where units from anywhere run.
fuzz-bc: all build/filo fuzz_bc.c fbc_dump.c fbc_dump.h
	@mkdir -p build/fuzz_bc_seeds build/fuzz_bc_corpus
	@./build/corpus_runner --vm --write-units build/fuzz_bc_seeds $(CORPUS) > /dev/null
	@rm -f build/fuzz_bc_seeds/*.expect
	@./build/filo bundle -o build/fuzz_bc_seeds/b1.fbb build/fuzz_bc_seeds/00001.fbc \
		build/fuzz_bc_seeds/00002.fbc build/fuzz_bc_seeds/00003.fbc
	@./build/filo bundle -o build/fuzz_bc_seeds/b2.fbb build/fuzz_bc_seeds/00100.fbc
	$(LLVM)/clang -std=c11 -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all $(WARN) \
		-DFILO_VM_ONLY -o build/fuzz_bc $(CORE) $(PACKS) $(HOST) fbc_dump.c fuzz_bc.c -lm
	@./build/fuzz_bc -max_total_time=$(FUZZ_SECONDS) -timeout=$(FUZZ_TIMEOUT) -max_len=65536 \
		-artifact_prefix=build/fuzz_bc_crash_ build/fuzz_bc_corpus build/fuzz_bc_seeds \
		> build/fuzz_bc.log 2>&1 || { tail -30 build/fuzz_bc.log; exit 1; }
	@tail -2 build/fuzz_bc.log

bench: $(CORE) $(PACKS) $(HOST) bench.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/bench $(CORE) $(PACKS) $(HOST) bench.c -lm
	./build/bench 2000

qa: all fmt-check corpus corpus-nolibc oracle api nolibc device cli tidy check freestanding fuzz fuzz-bc

clean:
	rm -rf build
