#!/usr/bin/env python3
"""Check the output of the fixed target ELFs against the instruction model.

Usage:  python3 mytests/fixed/target/analyze.py <run-dir>

<run-dir> holds <O>.stdout for O in O1, O2, O3, Os (see run.sh).

For each ELF:
  c0 calibration   each call site must give one c0: op2 and op3 through the u
                   site (body 2 and 5), op1 with b = 0 (body 4), q_mul(0, 0)
                   (body 11)
  model            used - c0 must equal the model of fixed_model.py for every
                   input; a deliberately wrong model must be rejected
  consistency      assembly, C and the reference must agree on every input
For each input set, the timing is judged as scripts/driver.py check_speed does:
the C build with the fewest cycles per call is the best one, and the assembly
measured in that same ELF must not exceed it by more than 2 cycles per call.
The exit status is 0 only if all of the above hold.
"""
import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fixed_model as model  # noqa: E402

OPTS = ("O1", "O2", "O3", "Os")
SLACK = 2.0
CAL_BODY = {"op2": model.OP2_BODY, "op3": model.OP3_BODY,
            "op1": model.OP1_ZERO_BODY, "q_mul": model.QMUL_ZERO_BODY}

CAL_RE = re.compile(r"^@@cal site=(\w+) fn=(\w+) used=(\d+)$")
M_RE = re.compile(r"^@@m (\w+) (\d+) (\d+) (\d+) (-?\d+) (\d+) (\d+) (\d+)$")
BENCH_RE = re.compile(
    r"^@@bench prog=fixed fn=(\w+) impl=(\w+) opt=(\w+) n=(\d+) cycles=(\d+) "
    r"instret=(\d+) overhead=(\d+) overhead_instret=(\d+) checksum=(\d+)$"
)
CONS_RE = re.compile(r"^@@cons .*mismatches=(\d+)$")


def parse(path):
    data = {"cal": [], "asm": [], "c": [], "bench": {}, "cons": None, "info": None, "diff": []}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            m = CAL_RE.match(line)
            if m:
                data["cal"].append((m[1], m[2], int(m[3])))
                continue
            m = M_RE.match(line)
            if m:
                row = (int(m[2]), int(m[3]), int(m[4]), int(m[5]), int(m[6]), int(m[7]), int(m[8]))
                data[m[1]].append(row)
                continue
            m = BENCH_RE.match(line)
            if m:
                data["bench"][(m[1], m[2])] = dict(
                    per_call=(int(m[5]) - int(m[7])) / int(m[4]),
                    n=int(m[4]), checksum=int(m[9]))
                continue
            if CONS_RE.match(line):
                data["cons"] = line
            elif line.startswith("@@info"):
                data["info"] = line
            elif line.startswith("@@diff"):
                data["diff"].append(line)
    return data


def calibrate(cal, out):
    """Return (ok, {site: c0})."""
    groups = {}
    for site, fn, used in cal:
        groups.setdefault(site, set()).add(used - CAL_BODY[fn])
    ok = True
    for site, vals in sorted(groups.items()):
        print("  c0 site %-3s = %s" % (site, sorted(vals)), file=out)
        if len(vals) != 1:
            print("  FAIL: c0 of site %s is not unique" % site, file=out)
            ok = False
    return ok, {s: min(v) for s, v in groups.items()}


def check_model(rows, c0, out):
    """Compare every row of the assembly with the true and the wrong model."""
    bad = {"op1": 0, "op2": 0, "op3": 0, "q_mul": 0, "n": 0}
    wrong = {"op1": 0, "q_mul": 0}
    worst = {"op1": (0, 0, 0), "q_mul": (0, 0, 0)}
    for a, b, u1, n, u2, u3, uq in rows:
        if n != model.op1_passes(a, b):
            bad["n"] += 1
        checks = (
            ("op1", u1 - c0["op1"], model.op1_body(a, b)),
            ("op2", u2 - c0["u"], model.OP2_BODY),
            ("op3", u3 - c0["u"], model.OP3_BODY),
            ("q_mul", uq - c0["q"], model.qmul_body(a, b)),
        )
        for fn, got, want in checks:
            if got != want:
                bad[fn] += 1
                if bad[fn] <= 5:
                    print("  MISMATCH %s(%#x, %#x): body %d, model %d" % (fn, a, b, got, want), file=out)
        wrong["op1"] += u1 - c0["op1"] != model.op1_body(a, b, per_pass=6)
        wrong["q_mul"] += uq - c0["q"] != model.qmul_body(a, b, carry_cost=0)
        if u1 > worst["op1"][0]:
            worst["op1"] = (u1, a, b)
        if uq > worst["q_mul"][0]:
            worst["q_mul"] = (uq, a, b)
    return bad, wrong, worst


def c_worst(rows):
    w = {"op1": (0, 0, 0), "op2": (0, 0, 0), "op3": (0, 0, 0), "q_mul": (0, 0, 0)}
    for a, b, u1, _n, u2, u3, uq in rows:
        for fn, u in (("op1", u1), ("op2", u2), ("op3", u3), ("q_mul", uq)):
            if u > w[fn][0]:
                w[fn] = (u, a, b)
    return w


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    run_dir = sys.argv[1]
    out = sys.stdout
    good = True
    datas = {}
    for opt in OPTS:
        d = parse(os.path.join(run_dir, opt + ".stdout"))
        datas[opt] = d
        print("== -%s %s" % (opt, d["info"]), file=out)
        ok, c0 = calibrate(d["cal"], out)
        good = good and ok
        if not d["asm"] or len(c0) != 3:
            print("  FAIL: no model data or calibration", file=out)
            good = False
            continue
        bad, wrong, worst = check_model(d["asm"], c0, out)
        print("  model: %d inputs; mismatches op1=%d n=%d op2=%d op3=%d q_mul=%d" %
              (len(d["asm"]), bad["op1"], bad["n"], bad["op2"], bad["op3"], bad["q_mul"]), file=out)
        print("  negative control (wrong model must differ): op1 per_pass=6 rejected on %d, "
              "q_mul carry_cost=0 rejected on %d" % (wrong["op1"], wrong["q_mul"]), file=out)
        print("  asm worst used: op1 %d at (%#x, %#x); q_mul %d at (%#x, %#x)" %
              (worst["op1"] + worst["q_mul"]), file=out)
        if any(bad.values()) or not wrong["op1"] or not wrong["q_mul"]:
            print("  FAIL: model check", file=out)
            good = False
        cw = c_worst(d["c"])
        print("  C worst used: " + "  ".join("%s %d at (%#x, %#x)" % ((fn,) + cw[fn]) for fn in cw), file=out)
        print("  %s" % d["cons"], file=out)
        for line in d["diff"]:
            print("  " + line, file=out)
        cons = CONS_RE.match(d["cons"] or "")
        if not cons or cons[1] != "0":
            print("  FAIL: consistency check", file=out)
            good = False

    print("== speed (cycles per call)", file=out)
    print("%-9s %8s %8s %8s %8s  %5s %8s %8s %8s %-6s %-8s" %
          ("set", "C O1", "C O2", "C O3", "C Os", "best", "C best", "asm same", "diff", "GateA", "internal"), file=out)
    sets = sorted({fn for d in datas.values() for fn, _impl in d["bench"]})
    gate_all = internal_all = 0
    for fn in sets:
        c = {o: datas[o]["bench"][(fn, "c")]["per_call"] for o in OPTS}
        best = min(c, key=c.get)
        asm = datas[best]["bench"][(fn, "asm")]["per_call"]
        diff = asm - c[best]
        gate = asm <= c[best] + SLACK
        internal = asm <= c[best]
        gate_all += gate
        internal_all += internal
        for o in OPTS:
            bc = datas[o]["bench"][(fn, "c")]["checksum"]
            ba = datas[o]["bench"][(fn, "asm")]["checksum"]
            if bc != ba:
                print("  FAIL: checksum differs: %s -%s c=%d asm=%d" % (fn, o, bc, ba), file=out)
                good = False
        print("%-9s %8.2f %8.2f %8.2f %8.2f  %5s %8.2f %8.2f %+8.2f %-6s %-8s" %
              (fn, c["O1"], c["O2"], c["O3"], c["Os"], best, c[best], asm, diff,
               "PASS" if gate else "FAIL", "met" if internal else "not met"), file=out)
        if not gate:
            good = False
    print("Gate A: %d of %d sets; internal target: %d of %d sets" %
          (gate_all, len(sets), internal_all, len(sets)), file=out)
    print("asm per call in each ELF:", file=out)
    for fn in sets:
        print("  %-9s " % fn + "  ".join(
            "%s %.2f" % (o, datas[o]["bench"][(fn, "asm")]["per_call"]) for o in OPTS), file=out)
    print("RESULT: %s" % ("PASS" if good else "FAIL"), file=out)
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
