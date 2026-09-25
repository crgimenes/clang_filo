// govm: the units corpus_runner --write-units DIR wrote, run on the Go
// engine's machine. Each must give what it gave on the C one — the result,
// or the error and where it happened, and the globals the case checks — as
// device_test holds the C build without a compiler to it. A unit is the
// same program on either.
//
// usage: go run . DIR   (make govm)
package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/crgimenes/filo"
	"github.com/crgimenes/filo/filomath"
	"github.com/crgimenes/filo/filostrings"
)

func main() {
	if len(os.Args) != 2 || os.Args[1] == "-h" || os.Args[1] == "--help" {
		fmt.Println("usage: govm DIR\nRuns the units corpus_runner --write-units DIR wrote on the Go engine's\nmachine, and checks each gives what it gave on the C one.")
		if len(os.Args) == 2 {
			return
		}
		os.Exit(2)
	}
	dir := os.Args[1]
	passed, failed := 0, 0
	for no := 0; ; no++ {
		expect, err := os.ReadFile(filepath.Join(dir, fmt.Sprintf("%05d.expect", no))) // #nosec G304 G703 -- a unit in the directory the command was given
		if err != nil {
			break
		}
		why := runCase(dir, no, string(expect))
		if why == "" {
			passed++
			continue
		}
		failed++
		fmt.Printf("FAIL %s/%05d: %s\n", dir, no, why)
	}
	fmt.Printf("go vm: %d passed, %d failed\n", passed, failed)
	if passed == 0 || failed > 0 {
		os.Exit(1)
	}
}

func load(e *filo.Engine, dir string, no int, suffix string) (*filo.Unit, error) {
	data, err := os.ReadFile(filepath.Join(dir, fmt.Sprintf("%05d%s", no, suffix))) // #nosec G304 G703 -- a unit in the directory the command was given
	if err != nil {
		return nil, err
	}
	return e.LoadUnit(data)
}

// newEngine has what the C runner registers: the core and the math and
// strings packs, which are packages of their own on the Go side.
func newEngine() *filo.Engine {
	e := filo.NewEngine()
	filomath.RegisterBuiltins(e)
	filostrings.RegisterBuiltins(e)
	return e
}

func runCase(dir string, no int, expect string) string {
	e := newEngine()
	var cfg filo.EvalConfig
	globals := map[string]filo.Value{}
	lines := strings.Split(strings.TrimRight(expect, "\n"), "\n")
	given := 0
	i := 0
	for ; i < len(lines); i++ {
		l := lines[i]
		if rest, ok := strings.CutPrefix(l, "limits "); ok {
			f := strings.Fields(rest)
			if len(f) != 2 {
				return "bad limits: " + l
			}
			cfg.StepLimit, _ = strconv.Atoi(f[0])
			cfg.RecursionLimit, _ = strconv.Atoi(f[1])
			continue
		}
		name, ok := strings.CutPrefix(l, "given ")
		if !ok {
			break
		}
		u, err := load(e, dir, no, fmt.Sprintf(".g%d.fbc", given))
		if err != nil {
			return fmt.Sprintf("given %s: %v", name, err)
		}
		v, _, err := u.Run(context.Background(), "main", nil, filo.EvalConfig{})
		if err != nil {
			return fmt.Sprintf("given %s: %v", name, err)
		}
		globals[name] = v
		given++
	}
	u, err := load(e, dir, no, ".fbc")
	var got filo.Value
	var after map[string]filo.Value
	if err == nil {
		got, after, err = u.Run(context.Background(), "main", globals, cfg)
	}
	if i == len(lines) {
		return "no expectation"
	}
	for ; i < len(lines); i++ {
		l := lines[i]
		if strings.HasPrefix(l, "error") {
			if err == nil {
				return "expected an error, got " + repr(got)
			}
			at := "error"
			if pe, ok := errors.AsType[*filo.PositionError](err); ok {
				at = fmt.Sprintf("error at %d:%d", pe.Line, pe.Col)
			}
			if at != l {
				return fmt.Sprintf("%s, want %s (%v)", at, l, err)
			}
			continue
		}
		if err != nil {
			return "unexpected error: " + err.Error()
		}
		if want, ok := strings.CutPrefix(l, "result "); ok {
			if repr(got) != want {
				return fmt.Sprintf("got %s, want %s", repr(got), want)
			}
			continue
		}
		rest, ok := strings.CutPrefix(l, "global ")
		name, want, found := strings.Cut(rest, " ")
		if !ok || !found {
			return "bad expectation: " + l
		}
		v, held := after[name]
		if !held || repr(v) != want {
			return fmt.Sprintf("global %s is %s, want %s", name, repr(v), want)
		}
	}
	return ""
}

// repr is how the C runtime writes a value: as Value.String does, but a
// control byte in a string is \xNN unless it is \n, \t or \r.
func repr(v filo.Value) string {
	switch v.Kind {
	case filo.KString:
		var b strings.Builder
		b.WriteByte('"')
		for i := 0; i < len(v.Str); i++ {
			c := v.Str[i]
			switch {
			case c == '"' || c == '\\':
				b.WriteByte('\\')
				b.WriteByte(c)
			case c == '\n':
				b.WriteString(`\n`)
			case c == '\t':
				b.WriteString(`\t`)
			case c == '\r':
				b.WriteString(`\r`)
			case c < 0x20 || c == 0x7f:
				fmt.Fprintf(&b, `\x%02x`, c)
			default:
				b.WriteByte(c)
			}
		}
		b.WriteByte('"')
		return b.String()
	case filo.KList, filo.KTuple:
		head, items := "(list", v.List
		if v.Kind == filo.KTuple {
			head, items = "(tuple", v.Tup
		}
		var b strings.Builder
		b.WriteString(head)
		for _, e := range items {
			b.WriteByte(' ')
			b.WriteString(repr(e))
		}
		b.WriteByte(')')
		return b.String()
	}
	return v.String()
}
