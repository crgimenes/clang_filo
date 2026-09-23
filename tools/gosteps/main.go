// gosteps: the steps each corpus program takes on the Go engine, which is
// the reference: the smallest step limit it runs under, found by bisection
// (the engine does not report the count). Only cases that run, with no
// given globals and no packs. The C runtime's IR must take the same steps
// (corpus_runner --steps): a step limit is behavior a script can see.
//
// usage: go run . CORPUS_FILE...   (make steps-regen)
package main

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"strings"

	"github.com/crgimenes/filo"
)

func main() {
	if len(os.Args) < 2 || os.Args[1] == "-h" || os.Args[1] == "--help" {
		fmt.Println("usage: gosteps CORPUS_FILE...\nPrints file, case and steps on the Go engine, a tab between them.")
		return
	}
	for _, path := range os.Args[1:] {
		f, err := os.Open(path)
		if err != nil {
			continue
		}
		var cases [][2]string
		name, script, skip, in, packs := "", []string{}, false, false, false
		flush := func() {
			if name != "" && !skip {
				cases = append(cases, [2]string{name, strings.TrimRight(strings.Join(script, "\n"), " \t\n")})
			}
		}
		sc := bufio.NewScanner(f)
		sc.Buffer(make([]byte, 1<<20), 1<<20)
		for sc.Scan() {
			l := strings.TrimRight(sc.Text(), " \t\r")
			if strings.HasPrefix(l, "packs:") && name == "" {
				packs = true
			}
			if strings.HasPrefix(l, "=== ") {
				flush()
				name, script, skip, in = l[4:], nil, false, true
				continue
			}
			if strings.HasPrefix(l, "--- ") {
				in = false
				continue
			}
			if in {
				if len(script) == 0 && (strings.HasPrefix(l, "given ") || strings.HasPrefix(l, "needs ")) {
					skip = true
					continue
				}
				if len(script) == 0 && strings.HasPrefix(l, "limits ") {
					continue
				}
				script = append(script, l)
			}
		}
		flush()
		f.Close()
		if packs {
			continue
		}
		base := path[strings.LastIndex(path, "/")+1:]
		for _, c := range cases {
			e := filo.NewEngine()
			p, err := e.Compile(c[1])
			if err != nil {
				continue
			}
			run := func(k int) bool {
				_, _, err := p.Execute(context.Background(), nil, filo.EvalConfig{StepLimit: k, RecursionLimit: 128})
				return err == nil
			}
			if !run(1 << 30) {
				continue
			}
			lo, hi := 1, 1<<30
			for lo < hi {
				mid := (lo + hi) / 2
				if run(mid) {
					hi = mid
				} else {
					lo = mid + 1
				}
			}
			fmt.Printf("%s\t%s\t%d\n", base, c[0], lo)
		}
	}
}
