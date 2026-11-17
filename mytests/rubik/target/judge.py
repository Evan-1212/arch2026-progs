#!/usr/bin/env python3
"""Judge one rv32emu run of mytests/rubik/target/verify.c.

Usage: judge.py <output-dir> <O1|O2|O3|Os> <ref-binary>

Reads states.tsv and invalid.tsv (geninputs.py) and run-<O>.out (the output
of verify-<O>.elf) in the output directory and checks
  states   one @@v line per state, in order; the assembly and the C version
           return the same length; it equals the listed distance when one is
           listed; `ref replay` accepts every assembly text (valid state,
           length = breadth-first distance, the text solves the state)
  budget   no assembly call above 7,500,000 instructions, no C call above
           50,000,000 (RUBIK_INSN_BUDGET, RUBIK_C_INSN_BUDGET of rubik.h)
  invalid  every invalid input returns -1 and leaves the buffer untouched
  cap      cap = length + 1 succeeds with the same text and nothing written
           after the NUL; a smaller cap returns -2 with out[0] = 0 when
           cap >= 1 and nothing written at or after index cap
Writes replay-<O>.tsv, replay-<O>.log and instret-<O>.csv next to the input.
Exit status 0 when every check passes, 1 otherwise, 2 on bad usage.
"""

import re
import subprocess
import sys

ASM_BUDGET = 7500000
C_BUDGET = 50000000
V = re.compile(r"^@@v (\d+) (\S+) na=(-?\d+) nc=(-?\d+) ia=(\d+) ic=(\d+) same=([01]) out=(.*)$")
INV = re.compile(r"^@@inv (own|public) (\d+) na=(-?\d+) ta=(\d+) nc=(-?\d+) tc=(\d+)$")
CAP = re.compile(r"^@@cap (\d+) impl=([ac]) cap=(\d+) ret=(-?\d+) rest=(\d+) buf=([0-9a-f]+)$")
DONE = re.compile(r"^@@done states=(\d+) invalid=(\d+) public_invalid=(\d+) cap_states=(\d+)$")


class Judge:
    def __init__(self):
        self.failures = 0

    def fail(self, msg):
        self.failures += 1
        if self.failures <= 20:
            print("FAIL " + msg)


def valid_state(data):
    """The validity rule of rubik.h on raw bytes, written again here."""
    if len(data) != 14:
        return False
    perm, ori = data[:7], data[7:]
    if sorted(perm) != list(b"1234567"):
        return False
    if any(b not in b"123" for b in ori):
        return False
    return sum(b - ord("1") for b in ori) % 3 == 0


def judge_states(j, out, opt, ref, states, lines):
    rows = [V.match(x) for x in lines if x.startswith("@@v ")]
    if len(rows) != len(states) or None in rows:
        j.fail(f"states: {len(rows)} @@v lines for {len(states)} states, or a malformed line")
        return
    same = 0
    table = []
    with open(f"{out}/replay-{opt}.tsv", "x", encoding="ascii") as f:
        for (idx, kind, state, dist), m in zip(states, rows):
            i, s, na, nc, ia, ic = int(m[1]), m[2], int(m[3]), int(m[4]), int(m[5]), int(m[6])
            text = m[8]
            if i != idx or s != state:
                j.fail(f"states: line {idx} is for {i} {s}, expected {state}")
                continue
            if na != nc:
                j.fail(f"states: {state} asm returned {na}, C returned {nc}")
            if dist >= 0 and na != dist:
                j.fail(f"states: {state} asm returned {na}, the listed distance is {dist}")
            if ia > ASM_BUDGET:
                j.fail(f"budget: {state} asm retired {ia}")
            if ic > C_BUDGET:
                j.fail(f"budget: {state} C retired {ic}")
            same += m[7] == "1"
            f.write(f"{state}\t{na}\t{text}\n")
            table.append((ia, ic, kind, state, na))
    with open(f"{out}/instret-{opt}.csv", "x", encoding="ascii") as f:
        f.write("kind,state,length,asm_instret,c_instret\n")
        for ia, ic, kind, state, na in table:
            f.write(f"{kind},{state},{na},{ia},{ic}\n")
    p = subprocess.run([ref, "replay", f"{out}/replay-{opt}.tsv"], capture_output=True, text=True, check=False)
    with open(f"{out}/replay-{opt}.log", "x", encoding="ascii") as f:
        f.write(p.stdout + p.stderr)
    m = re.search(r"^replay: records=(\d+) failures=(\d+) checksum=([0-9a-f]+)$", p.stdout, re.M)
    if p.returncode != 0 or not m or int(m[1]) != len(states) or int(m[2]) != 0:
        j.fail(f"replay: rc={p.returncode} summary={m[0] if m else None}")
    print(f"states: lines={len(rows)} replay rc={p.returncode} {m[0] if m else 'no summary'}")
    print(f"states: asm text identical to the C text in {same} of {len(rows)}")
    top = sorted(table, reverse=True)[:5]
    for ia, ic, kind, state, na in top:
        print(f"asm top: {state} ({kind}, {na} moves) asm={ia} C={ic} asm/budget={ia / ASM_BUDGET:.6f}")
    cmax = max(table, key=lambda r: r[1])
    print(f"C max: {cmax[3]} ({cmax[2]}, {cmax[4]} moves) C={cmax[1]} asm={cmax[0]} C/budget={cmax[1] / C_BUDGET:.6f}")
    slower = [r for r in table if r[0] > r[1]]
    print(f"asm slower than this ELF's C on {len(slower)} of {len(table)} inputs")
    for ia, ic, kind, state, na in sorted(slower, key=lambda r: r[1] - r[0])[:5]:
        print(f"  slower: {state} ({kind}, {na} moves) asm={ia} C={ic}")
    print(f"sum asm={sum(r[0] for r in table)} sum C={sum(r[1] for r in table)}")


def judge_invalid(j, own, public_count, lines):
    rows = [INV.match(x) for x in lines if x.startswith("@@inv ")]
    if None in rows or len(rows) != len(own) + public_count:
        j.fail(f"invalid: {len(rows)} @@inv lines for {len(own) + public_count} inputs, or a malformed line")
        return
    seen = {"own": 0, "public": 0}
    classes = {}
    for m in rows:
        tag, i, na, ta, nc, tc = m[1], int(m[2]), int(m[3]), int(m[4]), int(m[5]), int(m[6])
        if i != seen[tag]:
            j.fail(f"invalid: {tag} line {i} out of order")
        seen[tag] += 1
        cls = own[i][0] if tag == "own" and i < len(own) else "public"
        classes[cls] = classes.get(cls, 0) + 1
        if na != -1 or ta != 0:
            j.fail(f"invalid: {tag} {i} ({cls}) asm returned {na}, changed {ta} bytes")
        if nc != -1 or tc != 0:
            j.fail(f"invalid: {tag} {i} ({cls}) C returned {nc}, changed {tc} bytes")
    if seen != {"own": len(own), "public": public_count}:
        j.fail(f"invalid: counts {seen}")
    print(f"invalid: lines={len(rows)} by class {classes}")


def judge_cap(j, cap_states, lines):
    rows = [CAP.match(x) for x in lines if x.startswith("@@cap ")]
    if None in rows:
        j.fail("cap: malformed line")
        return
    groups = {}
    for m in rows:
        groups.setdefault((int(m[1]), m[2]), []).append((int(m[3]), int(m[4]), int(m[5]), bytes.fromhex(m[6])))
    if sorted(groups) != [(i, t) for i in range(cap_states) for t in "ac"]:
        j.fail(f"cap: {len(groups)} groups for {cap_states} states and two versions")
        return
    cases = {"fit": 0, "exact": 0, "short": 0, "one": 0, "zero": 0}
    texts = {}
    for (i, tag), group in sorted(groups.items()):
        who = f"cap: state {i} impl {tag}"
        cap, ret, rest, buf = group[0]
        nul = buf.find(0)
        if cap != 96 or ret < 0 or rest or nul < 0 or buf[nul + 1 :] != b"#" * (len(buf) - nul - 1):
            j.fail(f"{who}: full call cap={cap} ret={ret} rest={rest} buf={buf!r}")
            continue
        text, length = buf[:nul], nul
        texts[(i, tag)] = (ret, text)
        cases["fit"] += 1
        want = [length + 1] + ([length] if length >= 1 else []) + ([1] if length >= 2 else []) + [0]
        if [g[0] for g in group[1:]] != want:
            j.fail(f"{who}: caps {[g[0] for g in group[1:]]}, expected {want}")
            continue
        for cap, r, rest, buf in group[1:]:
            tail = buf[cap:] if cap < len(buf) else b""
            if rest or tail != b"#" * len(tail):
                j.fail(f"{who}: cap={cap} wrote at or after index cap: {buf!r} rest={rest}")
            if cap == length + 1:
                cases["exact"] += 1
                if r != ret or buf[: length + 1] != text + b"\0":
                    j.fail(f"{who}: cap={cap} (length + 1) returned {r}, buffer {buf!r}")
            else:
                cases["zero" if cap == 0 else "one" if cap == 1 and length >= 2 else "short"] += 1
                if r != -2:
                    j.fail(f"{who}: cap={cap} returned {r}, expected -2")
                if cap >= 1 and buf[0] != 0:
                    j.fail(f"{who}: cap={cap} left out[0] = {buf[0]}")
                if cap >= 1 and buf[1:cap] != b"#" * (cap - 1):
                    # Not required by rubik.h; the C version leaves them alone.
                    j.fail(f"{who}: cap={cap} changed bytes 1..cap-1: {buf!r}")
    for i in range(cap_states):
        a, c = texts.get((i, "a")), texts.get((i, "c"))
        if a is None or c is None or a[0] != c[0]:
            j.fail(f"cap: state {i} asm {a} C {c}")
    print(f"cap: states={cap_states} versions=2 cases={cases}")


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    out, opt, ref = sys.argv[1:]
    states = []
    with open(f"{out}/states.tsv", encoding="ascii") as f:
        for line in f:
            i, kind, s, d = line.rstrip("\n").split("\t")
            states.append((int(i), kind, s, int(d)))
    own = []
    with open(f"{out}/invalid.tsv", encoding="ascii") as f:
        for line in f:
            _, cls, lit = line.rstrip("\n").split("\t")
            data = eval(lit)  # a bytes literal written by geninputs.py
            own.append((cls, data))
    j = Judge()
    for cls, data in own:
        if valid_state(data.split(b"\0")[0]):
            j.fail(f"invalid: the {cls} input {data!r} is a valid state")
    with open(f"{out}/run-{opt}.out", encoding="latin-1") as f:
        lines = f.read().splitlines()
    done = [DONE.match(x) for x in lines if x.startswith("@@done")]
    if len(done) != 1 or done[0] is None:
        j.fail("no @@done line: the run did not finish")
        print(f"JUDGE {opt}: failures={j.failures}")
        return 1
    n_states, n_inv, n_pub, n_cap = (int(done[0][k]) for k in range(1, 5))
    if n_states != len(states) or n_inv != len(own):
        j.fail(f"@@done reports {n_states} states and {n_inv} invalid inputs")
    judge_states(j, out, opt, ref, states, lines)
    judge_invalid(j, own, n_pub, lines)
    judge_cap(j, n_cap, lines)
    print(f"JUDGE {opt}: failures={j.failures}")
    return 1 if j.failures else 0


if __name__ == "__main__":
    sys.exit(main())
