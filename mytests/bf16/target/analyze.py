#!/usr/bin/env python3
"""Check the output of the bf16 target ELFs against the instruction model.

Usage:  python3 mytests/bf16/target/analyze.py <run-dir>

<run-dir> holds <O>.stdout for O in O1, O2, O3, Os (see run.sh).

For each ELF:
  c0 calibration   per call site, the first calibration path (from_f32 of
                   +Inf, body 5; mul(+Inf, 1.0), body 17; sqrt(+0), body 7)
                   gives c0, and every other calibration path through the same
                   site must give the same value
  model            used - c0 must equal the model of bf16_model.py for every
                   input; each deliberately wrong model must be rejected
  coverage         every branch of the mul dispatch, every loop length 0 to 7,
                   every normalization length 1 to 7 for a and for b, every
                   result exit, both from_f32 paths and every sqrt path
  bounds           the worst body equals the static bound of the design (the
                   model inputs include each longest path) and no call
                   exceeds the budget of bf16.h (C: the loose budget)
  consistency      assembly, C and the reference agree on every input
For each input set, the timing is judged as scripts/driver.py check_speed does:
the C build with the fewest cycles per call is the best one, and the assembly
measured in that same ELF must not exceed it by more than 2 cycles per call.
The internal target (assembly not above the best C at all) is reported too.
The exit status is 0 only if all of the above hold, the internal target aside.
"""
import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bf16_model as model  # noqa: E402

OPTS = ("O1", "O2", "O3", "Os")
SLACK = 2.0
BUDGET = {"f32": 60, "mul": 400, "sqrt": 1500}
C_BUDGET = {"f32": 150, "mul": 1000, "sqrt": 4000}
BOUND = {"f32": model.F32_BOUND, "mul": model.MUL_BOUND, "sqrt": model.SQRT_BOUND}

CAL_RE = re.compile(r"^@@cal site=(\w+) (?:x=([0-9a-f]+)|a=([0-9a-f]+)(?: b=([0-9a-f]+))?) used=(\d+)$")
M_RE = re.compile(r"^@@m (f32|mul|sqrt) ([0-9a-f]+)(?: ([0-9a-f]+))? (\d+) (\d+)$")
BENCH_RE = re.compile(
    r"^@@bench prog=bf16 fn=(\w+) impl=(\w+) opt=(\w+) n=(\d+) cycles=(\d+) "
    r"instret=(\d+) overhead=(\d+) overhead_instret=(\d+) checksum=(\d+)$"
)
CONS_RE = re.compile(r"^@@cons .*mismatches=(\d+)$")


def body(fn, args, **kw):
    if fn == "f32":
        return model.f32_body(args[0], **kw)
    if fn == "mul":
        return model.mul_body(args[0], args[1], **kw)
    return model.sqrt_body(args[0], **kw)


def parse(path):
    data = {"cal": [], "m": [], "bench": {}, "cons": None, "info": None, "diff": []}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            m = CAL_RE.match(line)
            if m:
                site = m[1]
                args = (int(m[2], 16),) if m[2] else tuple(int(x, 16) for x in (m[3], m[4]) if x)
                data["cal"].append((site, args, int(m[5])))
                continue
            m = M_RE.match(line)
            if m:
                args = (int(m[2], 16),) + ((int(m[3], 16),) if m[3] else ())
                data["m"].append((m[1], args, int(m[4]), int(m[5])))
                continue
            m = BENCH_RE.match(line)
            if m:
                data["bench"][(m[1], m[2])] = dict(
                    per_call=(int(m[5]) - int(m[7])) / int(m[4]),
                    cycles=int(m[5]), overhead=int(m[7]), n=int(m[4]), checksum=int(m[9]))
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
    c0, ok = {}, True
    for site, args, used in cal:
        if site not in c0:
            c0[site] = used - body(site, args)
    for site in sorted(c0):
        rows = [(args, used - body(site, args)) for s, args, used in cal if s == site]
        vals = sorted({v for _, v in rows})
        paths = sorted({args for args, _ in rows})
        print("  c0 site %-4s = %s from %d paths (%d calls): %s" % (
            site, vals, len(paths), len(rows),
            ", ".join("(" + ", ".join("%#x" % x for x in p) + ")" for p in paths)), file=out)
        if len(vals) != 1:
            print("  FAIL: c0 of site %s is not unique" % site, file=out)
            ok = False
    return ok, c0


WRONG = (("f32", {"normal": 10}), ("mul", {"zero_step": 4}), ("mul", {"qnan_li": 1}),
         ("sqrt", {"one_digit": 1}))


def check_model(rows, c0, out):
    bad = {"f32": 0, "mul": 0, "sqrt": 0}
    count = {"f32": 0, "mul": 0, "sqrt": 0}
    wrong = [0] * len(WRONG)
    worst = {fn: (0, None) for fn in bad}
    cworst = {fn: (0, None) for fn in bad}
    cover = {"path": set(), "bits": set(), "k": set(), "k2": set(), "exit": set(),
             "f32": set(), "sqrt": set()}
    for fn, args, s, c in rows:
        count[fn] += 1
        got = s - c0[fn]
        want = body(fn, args)
        if got != want:
            bad[fn] += 1
            if bad[fn] <= 5:
                print("  MISMATCH %s(%s): body %d, model %d" % (
                    fn, ", ".join("%#x" % x for x in args), got, want), file=out)
        for i, (wfn, kw) in enumerate(WRONG):
            if wfn == fn and got != body(fn, args, **kw):
                wrong[i] += 1
        if got > worst[fn][0]:
            worst[fn] = (got, args)
        if c > cworst[fn][0]:
            cworst[fn] = (c, args)
        if fn == "mul":
            _, info = model.mul_info(*args)
            cover["path"].add(info["path"])
            if info["path"] == "nn":
                cover["bits"].add(info["bits"])
            if info["k"]:
                cover["k"].add(info["k"])
            if info["k2"]:
                cover["k2"].add(info["k2"])
            if info["exit"]:
                cover["exit"].add(info["exit"])
        elif fn == "f32":
            cover["f32"].add(want)
        else:
            cover["sqrt"].add(want if want < 50 else "normal")
    return bad, count, wrong, worst, cworst, cover


NEED = {
    "path": {"a_nan", "a_inf_zero", "a_inf", "b_nan", "b_inf_zero", "b_inf", "zero",
             "nn", "den_a", "den_b", "den_ab"},
    "bits": set(range(8)), "k": set(range(1, 8)), "k2": set(range(1, 8)),
    "exit": {"normal", "inf", "denormal", "zero"},
    "f32": {5, 11}, "sqrt": {7, 10, 11, 13, "normal"},
}


def fmt_args(args):
    return "(" + ", ".join("%#x" % x for x in args) + ")" if args else "-"


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
        if not d["m"] or len(c0) != 3:
            print("  FAIL: no model data or calibration", file=out)
            good = False
            continue
        bad, count, wrong, worst, cworst, cover = check_model(d["m"], c0, out)
        print("  model: f32 %d, mul %d, sqrt %d inputs; mismatches f32=%d mul=%d sqrt=%d" % (
            count["f32"], count["mul"], count["sqrt"], bad["f32"], bad["mul"], bad["sqrt"]), file=out)
        print("  negative controls (wrong model must differ): " + ", ".join(
            "%s %s rejected on %d" % (fn, kw, n) for (fn, kw), n in zip(WRONG, wrong)), file=out)
        if any(bad.values()) or not all(wrong):
            print("  FAIL: model check", file=out)
            good = False
        missing = {k: sorted(NEED[k] - cover[k], key=str) for k in NEED if NEED[k] - cover[k]}
        print("  coverage: mul paths %d, loop bits %s, k %s, k2 %s, exits %s; f32 bodies %s; sqrt %s" % (
            len(cover["path"]), sorted(cover["bits"]), sorted(cover["k"]), sorted(cover["k2"]),
            sorted(cover["exit"]), sorted(cover["f32"]), sorted(cover["sqrt"], key=str)), file=out)
        if missing:
            print("  FAIL: coverage missing %s" % missing, file=out)
            good = False
        for fn in ("f32", "mul", "sqrt"):
            b, args = worst[fn]
            used = b + c0[fn]
            cu, cargs = cworst[fn]
            print("  %-4s asm worst body %d at %s (bound %d, %s), used %d = body + c0 %d (budget %d); "
                  "C worst used %d at %s (budget %d)" % (
                      fn, b, fmt_args(args), BOUND[fn],
                      "reached" if b == BOUND[fn] else "NOT REACHED",
                      used, c0[fn], BUDGET[fn], cu, fmt_args(cargs), C_BUDGET[fn]), file=out)
            # The model inputs include the longest path of each routine, so the
            # measured worst body must equal the static bound, not only stay
            # below it.
            if b != BOUND[fn] or used > BUDGET[fn] or cu > C_BUDGET[fn]:
                print("  FAIL: bound or budget", file=out)
                good = False
        print("  %s" % d["cons"], file=out)
        for line in d["diff"]:
            print("  " + line, file=out)
        cons = CONS_RE.match(d["cons"] or "")
        if not cons or cons[1] != "0":
            print("  FAIL: consistency check", file=out)
            good = False

    print("== speed (cycles per call)", file=out)
    print("%-9s %8s %8s %8s %8s  %5s %8s %8s %8s %-6s %-8s" %
          ("set", "C O1", "C O2", "C O3", "C Os", "best", "C best", "asm same", "diff",
           "GateA", "internal"), file=out)
    sets = sorted({fn for d in datas.values() for fn, _impl in d["bench"]},
                  key=lambda s: int(re.search(r"S(\d+)$", s)[1]))
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
        print("%-9s %8.4f %8.4f %8.4f %8.4f  %5s %8.4f %8.4f %+8.4f %-6s %-8s" %
              (fn, c["O1"], c["O2"], c["O3"], c["Os"], best, c[best], asm, diff,
               "PASS" if gate else "FAIL", "met" if internal else "not met"), file=out)
        if not gate:
            good = False
    print("Gate A: %d of %d sets; internal target: %d of %d sets" %
          (gate_all, len(sets), internal_all, len(sets)), file=out)
    print("asm per call in each ELF (raw cycles, overhead, n):", file=out)
    for fn in sets:
        print("  %-9s " % fn + "  ".join(
            "%s %.4f (%d-%d)/%d" % (o, datas[o]["bench"][(fn, "asm")]["per_call"],
                                    datas[o]["bench"][(fn, "asm")]["cycles"],
                                    datas[o]["bench"][(fn, "asm")]["overhead"],
                                    datas[o]["bench"][(fn, "asm")]["n"]) for o in OPTS), file=out)
    print("C per call in each ELF (raw cycles, overhead, n):", file=out)
    for fn in sets:
        print("  %-9s " % fn + "  ".join(
            "%s %.4f (%d-%d)/%d" % (o, datas[o]["bench"][(fn, "c")]["per_call"],
                                    datas[o]["bench"][(fn, "c")]["cycles"],
                                    datas[o]["bench"][(fn, "c")]["overhead"],
                                    datas[o]["bench"][(fn, "c")]["n"]) for o in OPTS), file=out)
    print("RESULT: %s" % ("PASS" if good else "FAIL"), file=out)
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
