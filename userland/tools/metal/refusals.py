#!/usr/bin/env python3
"""The inventory of this port's refusals, and how often each one CAN fire.

This port refuses things BY NAME on purpose: a shader it cannot translate, a
descriptor it cannot honour, a draw it cannot bound-check. That is what keeps it
from drawing silently wrong pixels, and it is also the map of what is left --
but nobody could say how many refusals there were, or which of them mattered,
because the sentences live in four C files and the counts live in corpus
reports that name them only by their formatted text.

This joins the two. It reads every refusal SITE out of the sources (the format
string, with its file and line) and every refusal EVENT out of a corpus report
(the formatted sentence a module stopped at), matches the second to the first,
and prints each site with the number of modules it stops. The point of the join
is the zero: a site that fires for no module of either corpus is a refusal
nothing has ever measured, and saying so is worth more than an estimate.

What it does NOT count, and why, because the gap bounds every number here:

  - air_metal.c's refusals about an API input -- a texture descriptor, a
    sampler, a vertex layout, a draw -- cannot be counted against a SHADER
    corpus at all. A corpus module carries no MTLTextureDescriptor. Those
    sites are listed with "no corpus" rather than with a zero, because the two
    mean opposite things: one is unmeasured, the other is measured as never.
  - the driver's own refusals (userland/src/redwoodmetal.m) reach an
    application as an NSError and are counted per BOOT, in a log, not per
    module. They are listed and not counted.
  - RW_ONCE notices in the driver are counted apart and are NOT refusals: they
    print once and carry on with the work dropped, which is the one shape of
    failure this project set out to avoid. The count is here so that it is a
    number somebody has to look at.

A corpus report is `tools/air/corpus.py --json`. Two are worth having:
the 456 modules of the device image (docs/air.md section 5) and the 7 879
Metal libraries of Resident Evil 4 (section 8, extracted with
tools/air/repak.py). No byte of either is in this repository, so the reports
are made where the corpus is and this tool is pointed at them.

usage:
  corpus.py --ir3 --list sys.list --json sys.json
  refusals.py --report sys.json=456-module --report re4.json=RE4 [--zero] [--sites]
"""
import argparse
import collections
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Where a refusal is written, and how the sentence is spelled at that call.
#
#   say(why, why_size, "...")  -- air_metal.c, the contract library: two fixed
#                                 arguments before the format
#   air_error(c, "...")        -- the frontend: one
#
# The frontend's own refusals are the only ones a corpus report can count, and
# they are the only ones with a prefix in the sentence itself ("instruction: ",
# "interface: ", "intrinsic: ", "library: ", "stage: ").
#
# air_to_nir's five "stage: ..." refusals are the sixth kind and are written
# straight into the caller's error with ralloc, because they happen before
# there is an air_ctx to hold one -- and one of them is the largest refusal in
# the device corpus, so missing them would miss the number that matters most.
SOURCES = [
    ("tools/mesa/air/air_metal.c", "say", 2, "contract"),
    ("tools/mesa/air/air_to_nir.c", "air_error", 1, "frontend"),
    ("tools/mesa/air/air_intrinsics.c", "air_error", 1, "frontend"),
    ("tools/mesa/air/air_to_nir.c", "*error = ralloc_strdup", 1, "frontend"),
    ("tools/mesa/air/air_to_nir.c", "*error = ralloc_asprintf", 1, "frontend"),
]

# What a module's own stop reason gains on its way out of air_to_nir: the
# dispatcher appends where it happened (air_to_nir.c, translate_block).
WHERE = re.compile(r" \(block \d+, instruction \d+, opcode \d+\)$")


def literals(text, i):
    """The adjacent "..." string literals starting at or after i, joined.

    C concatenates them and so does the compiler; a refusal in this tree is
    three or four lines of them, so reading only the first would truncate every
    sentence long enough to be worth reading.
    """
    out = []
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j, buf = i + 1, []
            while j < n:
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                if text[j] == '"':
                    break
                buf.append(text[j])
                j += 1
            out.append("".join(buf))
            i = j + 1
            continue
        if c in " \t\n":
            i += 1
            continue
        break
    return "".join(out).replace('\\"', '"').replace("\\n", " ")


def sites(root=ROOT):
    """Every refusal site: (path, line, format string, which subsystem)."""
    found = []
    for rel, call, skip, kind in SOURCES:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(re.escape(call) + r"\s*\(", text):
            i, depth, args = m.end(), 1, 0
            while i < len(text) and args < skip:
                ch = text[i]
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                elif ch == "," and depth == 1:
                    args += 1
                i += 1
            fmt = literals(text, i)
            if fmt:
                found.append((rel, text.count("\n", 0, m.start()) + 1, fmt, kind))
    return found


def matcher(fmt):
    """A regex that matches what this format string prints.

    Every conversion becomes a wildcard, and %% becomes a literal percent. The
    match is anchored at both ends: a format of "%s" alone would otherwise
    match every sentence in the table and claim every event in the corpus.
    """
    out, i = [], 0
    spec = re.compile(r"%[-+ #0-9.*]*(?:hh|h|ll|l|z|j|t|L)?([diouxXeEfgGaAcspn%])")
    while i < len(fmt):
        m = spec.search(fmt, i)
        if not m:
            out.append(re.escape(fmt[i:]))
            break
        out.append(re.escape(fmt[i:m.start()]))
        out.append("%" if m.group(1) == "%" else ".*?")
        i = m.end()
    return re.compile("^" + "".join(out) + "$", re.S)


def events(path):
    """A corpus report's stop reasons: sentence -> how many modules stopped there."""
    d = json.load(open(path, encoding="utf-8", errors="replace"))
    c = collections.Counter()
    for r in d["rows"]:
        if r.get("translate") != "ok":
            c[WHERE.sub("", r.get("error", ""))] += 1
    return d, c


def rw_once(root=ROOT):
    """The driver's RW_ONCE notices: printed once, work dropped, nothing refused."""
    path = os.path.join(root, "userland/src/redwoodmetal.m")
    if not os.path.exists(path):
        return []
    text = open(path, encoding="utf-8", errors="replace").read()
    out = []
    for m in re.finditer(r"RW_ONCE\s*\(", text):
        if text.count("\n", 0, m.start()) + 1 < 400:
            continue              # the macro's own definition
        out.append((text.count("\n", 0, m.start()) + 1, literals(text, m.end())))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="append", default=[],
                    help="corpus.py --json FILE, optionally FILE=name")
    ap.add_argument("--zero", action="store_true",
                    help="only the frontend sites no module of any report reaches")
    ap.add_argument("--sites", action="store_true", help="every site, counted or not")
    ap.add_argument("--notices", action="store_true", help="the driver's RW_ONCE notices")
    ap.add_argument("--root", default=ROOT,
                    help="read the sources from here instead of the repository, so an "
                         "inventory keeps its line numbers while another session edits")
    a = ap.parse_args()

    table = sites(a.root)
    pats = [(matcher(f), (p, l, f, k)) for p, l, f, k in table]

    reports = []
    for spec in a.report:
        path, _, name = spec.partition("=")
        d, c = events(path)
        reports.append((name or os.path.basename(path), d, c))

    # Each event is credited to the site whose format matches it. More than one
    # can match -- "interface: %s" matches what "interface: input %s" prints --
    # so the LONGEST format wins, which is the more specific one. When two sites
    # carry the SAME format the tie is real and is not broken: both are named
    # and the count is marked shared, because "intrinsic: %s" is written twice
    # in air_intrinsics.c (the llvm.* fall-through and the air.* one) and
    # crediting one of them would invent a fact the sentence does not carry.
    hits = collections.defaultdict(lambda: collections.Counter())
    shared = collections.defaultdict(set)
    unmatched = collections.defaultdict(collections.Counter)
    for name, d, c in reports:
        for sentence, n in c.items():
            best, tied = None, []
            for rx, site in pats:
                if not rx.match(sentence):
                    continue
                if best is None or len(site[2]) > len(best[2]):
                    best, tied = site, [site]
                elif len(site[2]) == len(best[2]) and site[2] == best[2]:
                    tied.append(site)
            if best is None:
                unmatched[name][sentence] += n
            else:
                hits[(best[0], best[1])][name] += n
                if len(tied) > 1:
                    for s in tied:
                        shared[(best[0], best[1])].add("%s:%d" % (s[0], s[1]))

    print("REFUSAL SITES: %d" % len(table))
    for kind in ("frontend", "contract"):
        n = sum(1 for _, _, _, k in table if k == kind)
        print("  %-9s %3d" % (kind, n))
    for name, d, c in reports:
        print("  report %-14s %s, %d modules stopped" % (name, d["stages"], sum(c.values())))
    print()

    counted = [(p, l, f, k) for p, l, f, k in table if (p, l) in hits]
    counted.sort(key=lambda s: -sum(hits[(s[0], s[1])].values()))
    if not a.zero:
        print("--- sites a corpus reaches, most modules first ---")
        for p, l, f, k in counted:
            h = hits[(p, l)]
            where = "%s:%d" % (p, l)
            if (p, l) in shared:
                where = "shared between " + ", ".join(sorted(shared[(p, l)]))
            print("%-34s %s" % (", ".join("%s %d" % (n, h[n]) for n in h if h[n]), where))
            print("        %s" % f[:160])
        print()

    if a.zero or a.sites:
        print("--- frontend sites no report reaches (unmeasured, not measured as never) ---")
        for p, l, f, k in table:
            if k == "frontend" and (p, l) not in hits:
                print("%-40s %s" % ("%s:%d" % (p, l), f[:130]))
        print()
        print("--- contract sites: no corpus can count these (an API input, not a module) ---")
        print("    %d sites in %s" % (sum(1 for _, _, _, k in table if k == "contract"),
                                      SOURCES[0][0]))

    for name in unmatched:
        if unmatched[name]:
            print("--- %s: stop reasons no site in the sources explains ---" % name)
            for s, n in unmatched[name].most_common():
                print("%6d  %s" % (n, s[:140]))

    if a.notices:
        notes = rw_once(a.root)
        print()
        print("--- the driver's RW_ONCE notices: %d. NOT refusals: printed once, work "
              "dropped ---" % len(notes))
        for l, s in notes:
            print("  redwoodmetal.m:%-6d %s" % (l, s[:150]))


main()
