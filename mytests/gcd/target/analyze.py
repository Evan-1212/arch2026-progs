#!/usr/bin/env python3
"""Check the output of the gcd target ELFs against the instruction model.

Usage:  python3 mytests/gcd/target/analyze.py <run-dir> [dir-of-gcd_model.py]

<run-dir> holds <O>.stdout for O in O1, O2, O3, Os (see run.sh). gcd_model.py
provides run(a, b), the instruction count of the assembly body. It is loaded
from the directory of this script unless another directory is given.

For each ELF:
  c0 calibration   used(gcd(0, x)) - 3 and used(gcd(x, 0)) - 3 must all agree
  model            used - c0 must equal the model for every input
  consistency      assembly and C must agree on every input
For each input set, the timing is judged as scripts/driver.py check_speed does:
the C build with the fewest cycles per call is the best one, and the assembly
measured in that same ELF must not exceed it by more than 2 cycles per call.
The exit status is 0 only if all of the above hold.
"""
import os
import re
import sys

sys.dont_write_bytecode = True

OPTS = ("O1", "O2", "O3", "Os")
ASM_ZERO_BODY = 3
# C body of the zero paths, counted per level from the disassembly.
C_ZERO_BODY = {
    "O1": {"a0": 4, "b0": 4},
    "O2": {"a0": 4, "b0": 4},
    "O3": {"a0": 4, "b0": 4},
    "Os": {"a0": 3, "b0": 6},
}
SLACK = 2.0

CAL_RE = re.compile(r"^@@cal impl=(\w+) kind=(\w+) x=(\d+) used=(\d+)$")
M_RE = re.compile(r"^@@m (\d+) (\d+) (\d+)$")
BENCH_RE = re.compile(
    r"^@@bench prog=gcd fn=(\w+) impl=(\w+) opt=(\w+) n=(\d+) cycles=(\d+) "
    r"instret=(\d+) overhead=(\d+) overhead_instret=(\d+) checksum=(\d+)$"
)
CONS_RE = re.compile(r"^@@cons .*mismatches=(\d+)$")


def parse(path):
    data = {"cal": [], "m": [], "bench": {}, "cons": None, "info": None}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            m = CAL_RE.match(line)
            if m:
                data["cal"].append((m[1], m[2], int(m[3]), int(m[4])))
                continue
            m = M_RE.match(line)
            if m:
                data["m"].append((int(m[1]), int(m[2]), int(m[3])))
                continue
            m = BENCH_RE.match(line)
            if m:
                per_call = (int(m[5]) - int(m[7])) / int(m[4])
                data["bench"][(m[1], m[2])] = dict(
                    per_call=per_call,
                    n=int(m[4]),
                    cycles=int(m[5]),
                    overhead=int(m[7]),
                    checksum=int(m[9]),
                )
                continue
            if CONS_RE.match(line):
                data["cons"] = line
                continue
            if line.startswith("@@info"):
                data["info"] = line
    return data


def calibrate(opt, cal, out):
    """Return (ok, c0_asm); print each distinct c0."""
    ok = True
    groups = {}
    for impl, kind, _x, used in cal:
        body = ASM_ZERO_BODY if impl == "asm" else C_ZERO_BODY[opt][kind]
        groups.setdefault((impl, kind), set()).add(used - body)
    for (impl, kind), vals in sorted(groups.items()):
        print("  %-3s %s: c0 = %s" % (impl, kind, sorted(vals)), file=out)
    asm_vals = groups.get(("asm", "a0"), set()) | groups.get(("asm", "b0"), set())
    if len(asm_vals) != 1:
        print("  FAIL: assembly c0 is not unique: %s" % sorted(asm_vals), file=out)
        ok = False
    c_vals = groups.get(("c", "a0"), set()) | groups.get(("c", "b0"), set())
    if asm_vals and c_vals != asm_vals:
        print("  note: C c0 %s differs from assembly c0 %s" %
              (sorted(c_vals), sorted(asm_vals)), file=out)
    return ok, (min(asm_vals) if asm_vals else None)


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    run_dir = sys.argv[1]
    model_dir = sys.argv[2] if len(sys.argv) == 3 else os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.abspath(model_dir))
    import gcd_model  # pylint: disable=import-outside-toplevel

    out = sys.stdout
    good = True
    print("model file: %s" % os.path.abspath(gcd_model.__file__), file=out)
    datas = {}
    for opt in OPTS:
        d = parse(os.path.join(run_dir, opt + ".stdout"))
        datas[opt] = d
        print("== -%s %s" % (opt, d["info"]), file=out)
        ok, c0 = calibrate(opt, d["cal"], out)
        good = good and ok
        if c0 is None or not d["m"]:
            print("  FAIL: no calibration or no model data", file=out)
            good = False
            continue
        bad = 0
        k_cases = n_zero = 0
        max_model = max_used = 0
        for a, b, used in d["m"]:
            r = gcd_model.run(a, b)
            if used - c0 != r["model"]:
                bad += 1
                if bad <= 10:
                    print("  MISMATCH (%#x, %#x): used=%d c0=%d model=%d" %
                          (a, b, used, c0, r["model"]), file=out)
            k_cases += r["k"] >= 1
            n_zero += a == 0 or b == 0
            max_model = max(max_model, r["model"])
            max_used = max(max_used, used)
        print("  model: %d inputs (%d with k >= 1, %d with a zero operand), "
              "%d mismatches; max model %d, max used %d, c0 = %d" %
              (len(d["m"]), k_cases, n_zero, bad, max_model, max_used, c0),
              file=out)
        if bad:
            good = False
        print("  %s" % d["cons"], file=out)
        cons = CONS_RE.match(d["cons"] or "")
        if not cons or cons[1] != "0":
            print("  FAIL: consistency check", file=out)
            good = False

    print("== speed (cycles per call)", file=out)
    print("%-4s %-8s %8s %8s %8s %8s  %5s %8s %8s %8s %-6s %-8s" %
          ("set", "n", "C O1", "C O2", "C O3", "C Os", "best", "C best",
           "asm same", "diff", "GateA", "internal"), file=out)
    sets = sorted({fn for d in datas.values() for fn, impl in d["bench"]})
    for fn in sets:
        c = {o: datas[o]["bench"][(fn, "c")]["per_call"] for o in OPTS}
        best = min(c, key=c.get)
        asm = datas[best]["bench"][(fn, "asm")]["per_call"]
        diff = asm - c[best]
        gate = asm <= c[best] + SLACK
        internal = asm <= c[best]
        n = datas[best]["bench"][(fn, "c")]["n"]
        for o in OPTS:
            bc = datas[o]["bench"][(fn, "c")]["checksum"]
            ba = datas[o]["bench"][(fn, "asm")]["checksum"]
            if bc != ba:
                print("  FAIL: checksum differs: %s -%s c=%d asm=%d" % (fn, o, bc, ba), file=out)
                good = False
        print("%-4s %-8d %8.2f %8.2f %8.2f %8.2f  %5s %8.2f %8.2f %+8.2f %-6s %-8s" %
              (fn, n, c["O1"], c["O2"], c["O3"], c["Os"], best, c[best], asm, diff,
               "PASS" if gate else "FAIL", "met" if internal else "not met"), file=out)
        if not gate:
            good = False
    print("asm per call in each ELF:", file=out)
    for fn in sets:
        print("  %s: " % fn + "  ".join(
            "%s %.2f" % (o, datas[o]["bench"][(fn, "asm")]["per_call"]) for o in OPTS), file=out)
    print("RESULT: %s" % ("PASS" if good else "FAIL"), file=out)
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
