#!/usr/bin/env python3
"""Diff two callgrind profiles and report what changed, by function name.

Written for the A/B in .ai/memory/graphics-perf-backlog.md item H7, where two
revisions of vsg_backend_selftest print byte-identical logs, so the only way to
tell them apart is to count instructions. callgrind_annotate prints one profile
at a time; this prints the deltas.

    clang -O3 -g ...            # both revisions, same flags
    valgrind --tool=callgrind --callgrind-out-file=A.out ./a/vsg_backend_selftest
    valgrind --tool=callgrind --callgrind-out-file=B.out ./b/vsg_backend_selftest
    python3 scripts/callgrind_diff.py A.out B.out

Keys are FUNCTION NAMES, not (object, function): the two binaries are built in
different directories, so object paths never match and every entry would look
like it went from N to 0.

Format notes (callgrind docs, verified against real dumps): a `fn=` block holds
that function's SELF cost, then `cob=`/`cfn=`/`calls=` open a call block whose
cost lines are the CALLEE's inclusive cost. Names are compressed (`(N) text`
defines N, a bare `(N)` refers back to it) and an `ob=`/`cob=` line ends the
call block it appears in.
"""
import re
import sys
from collections import Counter

NAME_RE = re.compile(r"^\((\d+)\)\s*(.*)$")
SKIP = ("version:", "creator:", "pid:", "cmd:", "part:", "desc:", "positions:",
        "events:", "totals:", "summary:")


class Ids:
    def __init__(self):
        self.table = {}

    def resolve(self, raw):
        m = NAME_RE.match(raw)
        if not m:
            return raw.strip()
        ident, text = m.group(1), m.group(2).strip()
        if text:
            self.table[ident] = text
            return text
        return self.table.get(ident, f"#{ident}")


def parse(path):
    """Return (self_ir, callee_ir, call_counts), all keyed by function name."""
    ids = Ids()
    fn = cfn = None
    in_call = False
    self_ir = Counter()
    callee_ir = Counter()
    calls = Counter()

    with open(path, errors="replace") as fh:
        for line in fh:
            if not line.strip() or line.startswith(SKIP):
                continue
            head = line[0]
            if line.startswith(("ob=", "cob=")):
                cfn, in_call = None, False
            elif line.startswith("fn="):
                fn, cfn, in_call = ids.resolve(line[3:]), None, False
            elif line.startswith("cfn="):
                cfn, in_call = ids.resolve(line[4:]), False
            elif line.startswith(("cfi=", "fl=", "fi=", "fe=")):
                continue
            elif line.startswith("calls="):
                parts = line[6:].split()
                if parts and parts[0].isdigit():
                    calls[(fn, cfn)] += int(parts[0])
                in_call = True
            elif head.isdigit() or head in "+*":
                fields = line.split()
                if len(fields) < 2:
                    continue
                cost = int(fields[-1])
                if in_call and cfn is not None:
                    callee_ir[cfn] += cost
                elif fn is not None:
                    self_ir[fn] += cost
    return self_ir, callee_ir, calls


def report(title, a, b, rows=15, pattern=None):
    print(f"\n== {title} ==")
    keys = set(a) | set(b)
    if pattern:
        keys = {k for k in keys if re.search(pattern, k, re.I)}
    shown = 0
    for k in sorted(keys, key=lambda k: b[k] - a[k], reverse=True):
        delta = b[k] - a[k]
        if delta <= 0 or shown >= rows:
            break
        print(f"  {delta:+14,}  A={a[k]:14,} B={b[k]:14,}  {k}")
        shown += 1


def main():
    a_self, a_callee, a_calls = parse(sys.argv[1])
    b_self, b_callee, b_calls = parse(sys.argv[2])

    total_a, total_b = sum(a_self.values()), sum(b_self.values())
    print(f"self cost: A={total_a:,}  B={total_b:,}  "
          f"delta={total_b - total_a:+,} ({100.0 * (total_b - total_a) / total_a:+.1f}%)")

    report("SELF cost increases (B - A)", a_self, b_self, 20)
    report("SELF cost increases, shader related", a_self, b_self, 20,
           r"glslang|Shader|SPIRV|Compile")
    report("CALLEE inclusive increases (B - A)", a_callee, b_callee, 15)

    print("\n== call-count increases (B - A) ==")
    shown = 0
    for k in sorted(set(a_calls) | set(b_calls), key=lambda k: b_calls[k] - a_calls[k], reverse=True):
        delta = b_calls[k] - a_calls[k]
        if delta <= 0 or shown >= 20:
            break
        print(f"  {delta:+8d}  A={a_calls[k]:8d} B={b_calls[k]:8d}  {k[1]}  <- {k[0]}")
        shown += 1


main()
