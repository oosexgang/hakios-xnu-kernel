#!/usr/bin/env python3
"""LLVM-IR *definitions* for the exactly-defined AIR operation families.

intrincheck.py pins the semantics of every AIR arithmetic operation the corpus
uses, bit-exactly against the Adreno. That file is the ground truth; this one
is its constructive counterpart: for the families whose result is exactly
defined -- fast_fmin, fast_fmax, fast_fabs, fast_sqrt, fast_clamp, mix, dot,
sign, convert -- it writes the LLVM-IR function body that computes exactly what
intrincheck's Python reference computes, so a module that declares one of them
can be given a definition instead of an external call.

`definitions_for(names)` takes a list of air.* declaration names (as they are
mangled in a module) and returns the IR definitions for the ones in the
families above, in whatever suffixes occur (f32, v2f32, v3f32, v4f32, f16, the
integer widths that convert touches). Nothing is approximated: the transcendental
and fast-math families (fast_pow, fast_exp, fast_sin, fast_cos, fast_atan2,
fast_log*, fast_rsqrt, fast_divide) have no bit-exact reference anywhere -- Metal's
`fast_` prefix permits a lower-precision implementation -- so emitting a body for
them would be fabricating a reference. Those are refused by name instead;
`refusals_for(names)` returns {name: why}, and `analyze(names)` returns the whole
breakdown (defined / refused / skipped).

Two rules the definitions obey, because the whole point is to NOT depend on the
compiler's own flags:

  - no fast-math flags on any instruction (`fmul`, never `fmul fast`): a flag
    would let llc contract fmul+fadd into an fma and the body's meaning would
    then rest on the flag rather than on the plain IEEE operation.
  - signed zeros, the summation order and the PRECISION are matched to
    intrincheck's reference exactly. fmin/fmax use llvm.minimum/llvm.maximum,
    whose signed-zero rule (-0.0 < +0.0) is the one _fmin/_fmax encode; dot
    accumulates p0..p_{n-1} starting from +0.0 in the result's own type, which
    is the left fold the reference's dot branch performs. A half dot product
    adds halves: `fadd half`, never a widened accumulator, because 320.0625 --
    (-0.25, 8, -16) against itself -- is not a number a `half` holds, and this
    file's agreement with the reference was for a while resting on component_ok
    rounding the expectation down to half for it.

The self-check under `if __name__ == "__main__":` proves it: for every family x
suffix that occurs in the corpus it assembles the definition with llc, links a C
main, runs it on the SAME input pool intrincheck uses, and compares the raw bit
pattern of every result component against intrincheck's Python reference. It is
entirely host-side; no GPU and no phone are involved, because intrincheck's
reference is already pinned to the Adreno.
"""

import re

# ---------------------------------------------------------------------------
# which base operation falls into which bucket

# The exactly-defined families this file emits bodies for, keyed by the base
# operation name (the air.* name with any "fast_" prefix stripped).
DEFINED_BASES = {"fmin", "fmax", "fabs", "sqrt", "clamp", "mix", "sign", "dot",
                 "convert"}

# Fast-math / transcendental families with no bit-exact reference. divide is
# here although intrincheck's plain `divide` reference is exact a/b: Metal's
# fast_divide is an approximate reciprocal, not IEEE division, so there is no
# bit-exact reference for the fast_ form either.
REFUSED_BASES = {"pow", "powr", "exp", "exp2", "exp10", "sin", "cos", "tan",
                 "atan2", "log", "log2", "log10", "rsqrt", "divide",
                 # the inverse trigonometric and hyperbolic ones, for the same
                 # reason and with one of their own: the frontend reduces acos
                 # and asin to atan2 and sinh, cosh and tanh to the
                 # exponential, so a body here would have to agree with
                 # Mesa's own atan2 polynomial rather than with IEEE. There is
                 # nothing to agree with bit-exactly.
                 "acos", "asin", "atan", "sinh", "cosh", "tanh"}


def _refusal_reason(plain):
    if plain == "divide":
        return ("fast division is an approximate reciprocal, not IEEE a/b; Metal's "
                "fast-math permits reduced precision, so no bit-exact reference exists")
    kind = ("power" if plain in ("pow", "powr")
            else "exponential" if plain in ("exp", "exp2", "exp10")
            else "trigonometric" if plain in ("sin", "cos", "tan", "atan2")
            else "inverse trigonometric" if plain in ("acos", "asin", "atan")
            else "hyperbolic" if plain in ("sinh", "cosh", "tanh")
            else "logarithm" if plain in ("log", "log2", "log10")
            else "reciprocal square root" if plain == "rsqrt"
            else plain)
    return ("approximate %s (fast-math): Metal permits a lower-precision implementation, "
            "so there is no bit-exact reference and emitting one would fabricate it" % kind)


# ---------------------------------------------------------------------------
# type tokens

_BASE = {"f16": "half", "f32": "float", "f64": "double",
         "i64": "i64", "i32": "i32", "i16": "i16", "i8": "i8", "i1": "i1"}
_SHORT = {"half": "f16", "float": "f32", "double": "f64"}
_TOK = re.compile(r"^(?:v(\d+))?(f16|f32|f64|i64|i32|i16|i8|i1)$")


def _tok_type(tok):
    """'v3f32' -> ('float', 3); 'i16' -> ('i16', 1)."""
    m = _TOK.match(tok)
    if not m:
        raise ValueError("unrecognised type token %r" % tok)
    n = int(m.group(1)) if m.group(1) else 1
    return _BASE[m.group(2)], n


def _llty(base, n):
    return base if n == 1 else "<%d x %s>" % (n, base)


def _mangle(base, n):
    """LLVM intrinsic overload suffix for a float type: f32, v3f32, ..."""
    short = _SHORT[base]
    return short if n == 1 else "v%d%s" % (n, short)


def _vconst(base, n, lit):
    """A scalar literal, or a vector splat of it, as the value half of an operand."""
    if n == 1:
        return lit
    return "<" + ", ".join("%s %s" % (base, lit) for _ in range(n)) + ">"


# ---------------------------------------------------------------------------
# emitting one definition. Each emitter returns (ir_text, intrinsics) where
# intrinsics is a set of (llvm_name, base, n, nargs) that must be declared.


def _op_suffix(name):
    """The type token of an air.<op>.<suffix> name (not convert)."""
    return name.split(".")[2]


def _emit_elementwise(name, plain):
    base, n = _tok_type(_op_suffix(name))
    ty = _llty(base, n)
    sfx = _mangle(base, n)
    intr = set()
    body = []
    params = None

    if plain == "fabs":
        intr.add(("llvm.fabs", base, n, 1))
        body.append("  %%r = call %s @llvm.fabs.%s(%s %%a0)" % (ty, sfx, ty))
        params = [ty]
        rv = "%r"
    elif plain == "sqrt":
        intr.add(("llvm.sqrt", base, n, 1))
        body.append("  %%r = call %s @llvm.sqrt.%s(%s %%a0)" % (ty, sfx, ty))
        params = [ty]
        rv = "%r"
    elif plain == "fmin":
        # _fmin: the lesser, and for (+-0, -+0) the negative zero. That is
        # exactly llvm.minimum, whose rule is -0.0 < +0.0 and which propagates
        # NaN (the pool has no NaN, so only the signed-zero rule is exercised).
        intr.add(("llvm.minimum", base, n, 2))
        body.append("  %%r = call %s @llvm.minimum.%s(%s %%a0, %s %%a1)"
                    % (ty, sfx, ty, ty))
        params = [ty, ty]
        rv = "%r"
    elif plain == "fmax":
        intr.add(("llvm.maximum", base, n, 2))
        body.append("  %%r = call %s @llvm.maximum.%s(%s %%a0, %s %%a1)"
                    % (ty, sfx, ty, ty))
        params = [ty, ty]
        rv = "%r"
    elif plain == "clamp":
        # _clamp_f(x, lo, hi) == _fmin(_fmax(x, lo), hi): fmax first, then fmin.
        intr.add(("llvm.maximum", base, n, 2))
        intr.add(("llvm.minimum", base, n, 2))
        body.append("  %%t = call %s @llvm.maximum.%s(%s %%a0, %s %%a1)"
                    % (ty, sfx, ty, ty))
        body.append("  %%r = call %s @llvm.minimum.%s(%s %%t, %s %%a2)"
                    % (ty, sfx, ty, ty))
        params = [ty, ty, ty]
        rv = "%r"
    elif plain == "mix":
        # _mix(a, b, t) = ROUND(a + ROUND(ROUND(b - a) * t)): three plain ops,
        # each rounding once to the type's precision. No fast-math flag, so llc
        # cannot fuse the fmul and fadd into an fma.
        body.append("  %%d = fsub %s %%a1, %%a0" % ty)
        body.append("  %%m = fmul %s %%d, %%a2" % ty)
        body.append("  %%r = fadd %s %%a0, %%m" % ty)
        params = [ty, ty, ty]
        rv = "%r"
    elif plain == "sign":
        # _sign: x>0 -> 1, x<0 -> -1, otherwise x (so +-0 keep their sign; the
        # pool has no NaN). Returning %a0 for the zero case preserves the sign.
        zero = "0.0" if n == 1 else "zeroinitializer"
        one = _vconst(base, n, "1.0")
        neg = _vconst(base, n, "-1.0")
        cty = "i1" if n == 1 else "<%d x i1>" % n
        body.append("  %%gt = fcmp ogt %s %%a0, %s" % (ty, zero))
        body.append("  %%lt = fcmp olt %s %%a0, %s" % (ty, zero))
        body.append("  %%s1 = select %s %%gt, %s %s, %s %%a0" % (cty, ty, one, ty))
        body.append("  %%r = select %s %%lt, %s %s, %s %%s1" % (cty, ty, neg, ty))
        params = [ty]
        rv = "%r"
    else:
        raise ValueError("not an elementwise family: %s" % plain)

    sig = _define(name, ty, params, body, ty, rv)
    return sig, intr


def _emit_dot(name):
    base, n = _tok_type(_op_suffix(name))
    vt = _llty(base, n)
    body = []
    for k in range(n):
        body.append("  %%x%d = extractelement %s %%a0, i32 %d" % (k, vt, k))
        body.append("  %%y%d = extractelement %s %%a1, i32 %d" % (k, vt, k))
        body.append("  %%p%d = fmul %s %%x%d, %%y%d" % (k, base, k, k))
    # The reference's dot branch folds left from +0.0, rounding each step to
    # the result's own precision: acc = +0.0 + p0, then + p1, ... in index
    # order. The `fadd %base` below is in that same precision, which is the
    # point -- for a half dot product an accumulator one precision wider gives
    # 320.0625 where the type holds only 320. For the dyadic DOT_FLOATS pool
    # every product and partial sum is exact, so the order cannot change the
    # value -- but it is matched anyway, because that is the one thing a dot
    # lowering is most likely to get wrong.
    body.append("  %%s0 = fadd %s 0.0, %%p0" % base)
    last = "%s0"
    for k in range(1, n):
        body.append("  %%s%d = fadd %s %%s%d, %%p%d" % (k, base, k - 1, k))
        last = "%%s%d" % k
    sig = _define(name, base, [vt, vt], body, base, last)
    return sig, set()


def _emit_convert(name):
    # air.convert.<dk>.<dtype>.<sk>.<stype>(v)
    parts = name.split(".")
    dk, dtype, sk, stype = parts[2], parts[3], parts[4], parts[5]
    dbase, dn = _tok_type(dtype)
    sbase, sn = _tok_type(stype)
    dt = _llty(dbase, dn)
    st = _llty(sbase, sn)
    body = []
    rv = "%r"
    if dbase == "i1":
        # A bool destination is a comparison with zero, not a conversion: C's
        # rule, which MSL inherits. This used to emit `fptoui <ty> to i1`,
        # which truncates -- so it answered 0 for 0.5 and, worse, is poison
        # under LLVM's own rule for every |x| >= 2, because the truncated value
        # does not fit the destination. Apple's compiler puts the result of
        # air.convert.u.i1.f.f32 straight into a `br i1` (four calls in the
        # device image), and a branch on poison is not something a compiler
        # emits. See the long note in intrincheck.ref_convert.
        #
        # The vector form IS attested, and this file used to refuse it: the
        # device image declares air.convert.u.v4i1.s.v4i64 and
        # air.convert.u.v4i1.u.v4i64, so "no vector bool destination is
        # attested" was true when it was written and false by the time two
        # declarations of a defined family were being reported as "declared in
        # a defined family but not emitted". There is nothing to guess about
        # the shape: `x != 0` is elementwise, which is what the reference
        # already computes component by component, and a vector comparison in
        # LLVM is the same instruction over a vector.
        zero = "zeroinitializer" if dn > 1 else ("0.0" if sbase in ("half", "float") else "0")
        if sbase in ("half", "float"):
            body.append("  %%r = fcmp une %s %%a0, %s" % (st, zero))
        else:
            body.append("  %%r = icmp ne %s %%a0, %s" % (st, zero))
    elif sk == "f":
        # float -> int: Metal truncates toward zero, which is what fptosi/fptoui
        # do. dk picks the signedness; out-of-range is undefined in the MSL spec
        # and the reference raises on it, so it is never a compared case.
        instr = "fptosi" if dk == "s" else "fptoui"
        body.append("  %%r = %s %s %%a0 to %s" % (instr, st, dt))
    elif dk == "f":
        # int -> float, rounded once to the destination precision. sk decides
        # whether the source integer is read signed or unsigned.
        instr = "sitofp" if sk == "s" else "uitofp"
        body.append("  %%r = %s %s %%a0 to %s" % (instr, st, dt))
    else:
        # int -> int. The source kind sk decides sign- vs zero-extension when
        # widening; narrowing keeps the low bits; the destination kind dk only
        # decides how those bits are later read, not what they are.
        ws, wd = int(sbase[1:]), int(dbase[1:])
        if wd > ws:
            instr = "sext" if sk == "s" else "zext"
            body.append("  %%r = %s %s %%a0 to %s" % (instr, st, dt))
        elif wd < ws:
            body.append("  %%r = trunc %s %%a0 to %s" % (st, dt))
        else:
            rv = "%a0"  # same width: the bits are unchanged
    sig = _define(name, dt, [st], body, dt, rv)
    return sig, set()


def _define(name, retty, params, body, _retty2, rv):
    args = ", ".join("%s %%a%d" % (t, i) for i, t in enumerate(params))
    lines = ["define %s @%s(%s) {" % (retty, name, args)]
    lines += body
    lines.append("  ret %s %s" % (retty, rv))
    lines.append("}")
    return "\n".join(lines)


_INTRIN_DECL = {
    1: "declare %s @%s.%s(%s)",
    2: "declare %s @%s.%s(%s, %s)",
}


def _intrinsic_decl(llvm_name, base, n, nargs):
    ty = _llty(base, n)
    sfx = _mangle(base, n)
    if nargs == 1:
        return "declare %s @%s.%s(%s)" % (ty, llvm_name, sfx, ty)
    return "declare %s @%s.%s(%s, %s)" % (ty, llvm_name, sfx, ty, ty)


# ---------------------------------------------------------------------------
# the public surface


def _base_of(name):
    """The base operation of an air.* name: 'air.fast_fmin.v3f32' -> 'fmin'."""
    op = name.split(".")[1]
    return op[5:] if op.startswith("fast_") else op


def analyze(names):
    """Classify names and emit what can be emitted.

    Returns a dict with:
      ir        -- the LLVM-IR text (intrinsic declarations + definitions)
      defined   -- [name] for which a definition was emitted
      refused   -- {name: why} for the approximate families
      skipped   -- [name] that belong to no family this file handles
    """
    defs = []
    intrinsics = set()
    defined = []
    refused = {}
    skipped = []
    for name in names:
        if not name.startswith("air."):
            skipped.append(name)
            continue
        base = _base_of(name)
        if base in REFUSED_BASES:
            refused[name] = _refusal_reason(base)
            continue
        if base not in DEFINED_BASES:
            skipped.append(name)
            continue
        try:
            if base == "convert":
                ir, intr = _emit_convert(name)
            elif base == "dot":
                ir, intr = _emit_dot(name)
            else:
                ir, intr = _emit_elementwise(name, base)
        except (ValueError, KeyError, IndexError):
            # a name in a defined family whose suffix this file cannot parse is
            # skipped rather than guessed at
            skipped.append(name)
            continue
        defs.append(ir)
        intrinsics |= intr
        defined.append(name)

    decls = sorted(_intrinsic_decl(*spec) for spec in intrinsics)
    chunks = []
    if decls:
        chunks.append("\n".join(decls))
    chunks.extend(defs)
    return dict(ir="\n\n".join(chunks) + ("\n" if chunks else ""),
                defined=defined, refused=refused, skipped=skipped)


def definitions_for(names):
    """LLVM-IR definitions for the exactly-defined families among `names`.

    The approximate families are not defined; see refusals_for(names).
    """
    return analyze(names)["ir"]


def refusals_for(names):
    """{name: why} for the approximate families among `names`, which have no
    bit-exact reference and so are refused rather than defined."""
    return analyze(names)["refused"]


# ===========================================================================
# self-check: assemble every emitted definition and compare it, bit for bit,
# against intrincheck's Python reference on the pool intrincheck uses.

if __name__ == "__main__":
    import math
    import os
    import struct
    import subprocess
    import sys
    import tempfile

    HERE = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, HERE)
    sys.path.insert(0, os.path.join(HERE, "..", ".."))
    sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
    import intrincheck  # noqa: E402

    def host_triple():
        out = subprocess.run(["llc", "--version"], capture_output=True, text=True).stdout
        m = re.search(r"Default target: (\S+)", out)
        return m.group(1) if m else "x86_64-unknown-linux-gnu"

    def collect_decls(argv):
        """The declare lines to test: a --decls file, or the corpus metallibs.

        air2nir may not be built, so the corpus is read the way cpuref.py does:
        metallib.parse + llvm-dis, not air2nir --ll.
        """
        for i, a in enumerate(argv):
            if a == "--decls":
                return [l.strip() for l in open(argv[i + 1]) if l.strip()]
        ROOT = os.path.dirname(os.path.dirname(HERE))
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import metallib
        libs = []
        for dp, _, ns in os.walk(os.path.join(ROOT, "ios165/rootfs")):
            for n in ns:
                if n.endswith(".metallib"):
                    libs.append(os.path.join(dp, n))
        out = set()
        for lib in sorted(libs):
            try:
                b, header, funcs = metallib.parse(lib)
            except Exception:
                continue
            for f in funcs:
                try:
                    bc, _ = metallib.bitcode_of(b, header, f)
                except Exception:
                    continue
                with tempfile.NamedTemporaryFile(suffix=".bc", delete=False) as t:
                    t.write(bc)
                    p = t.name
                try:
                    ll = subprocess.run(["llvm-dis", "-o", "-", p],
                                        capture_output=True).stdout.decode("utf-8", "replace")
                finally:
                    os.unlink(p)
                for line in ll.splitlines():
                    if line.startswith("declare") and "@air." in line:
                        line = re.sub(r"\s+local_unnamed_addr.*$", "", line)
                        line = re.sub(r"\s+#\d+$", "", line)
                        out.add(line.strip())
        return sorted(out)

    # -- packing one component into the 8-byte input slot the harness reads ----

    def pack_component(base, v):
        if base == "float":
            b = struct.pack("<f", intrincheck.f32(v))
        elif base == "half":
            h = intrincheck.f16(v)
            if math.isinf(h):
                b = struct.pack("<H", 0x7C00 if h > 0 else 0xFC00)
            else:
                b = struct.pack("<e", h)
        elif base == "double":
            b = struct.pack("<d", v)
        elif base == "i1":
            b = bytes([int(v) & 1])
        else:
            w = int(base[1:])
            b = (int(v) & ((1 << w) - 1)).to_bytes(w // 8, "little")
        return b + b"\x00" * (8 - len(b))

    def pack_case(case, argtys):
        out = b""
        for ty, comps in zip(argtys, case):
            base, n = intrincheck.parse_type(ty)
            for c in range(n):
                out += pack_component(base, comps[c])
        return out

    # -- comparing one result component against the reference, bit for bit -----

    def half_bits(h):
        if math.isinf(h):
            return 0x7C00 if h > 0 else 0xFC00
        return int.from_bytes(struct.pack("<e", h), "little")

    def component_ok(base, slot, exp):
        if base == "float":
            gbits = int.from_bytes(slot[:4], "little")
            gf = struct.unpack("<f", slot[:4])[0]
            if math.isnan(gf) and isinstance(exp, float) and math.isnan(exp):
                return True, gbits, "nan"
            ebits = intrincheck.to_f32_bits(intrincheck.f32(exp))
            return gbits == ebits, gbits, ebits
        if base == "half":
            gbits = int.from_bytes(slot[:2], "little")
            gh = struct.unpack("<e", slot[:2])[0]
            if math.isnan(gh) and isinstance(exp, float) and math.isnan(exp):
                return True, gbits, "nan"
            ebits = half_bits(intrincheck.f16(exp))
            return gbits == ebits, gbits, ebits
        if base == "double":
            gbits = int.from_bytes(slot[:8], "little")
            gd = struct.unpack("<d", slot[:8])[0]
            if math.isnan(gd) and isinstance(exp, float) and math.isnan(exp):
                return True, gbits, "nan"
            ebits = struct.unpack("<Q", struct.pack("<d", exp))[0]
            return gbits == ebits, gbits, ebits
        w = int(base[1:])
        nb = max(1, w // 8)
        g = int.from_bytes(slot[:nb], "little") & ((1 << w) - 1)
        e = int(exp) & ((1 << w) - 1)
        return g == e, g, e

    # -- building, compiling and running one declaration's harness -------------

    def build_run_ll(name, argtys, retty):
        lines = []
        slot = 0
        callargs = []
        for j, ty in enumerate(argtys):
            base, n = intrincheck.parse_type(ty)
            comps = []
            for c in range(n):
                off = slot * 8
                slot += 1
                lines.append("  %%p_%d_%d = getelementptr i8, ptr %%in, i64 %d"
                             % (j, c, off))
                lines.append("  %%l_%d_%d = load %s, ptr %%p_%d_%d, align 1"
                             % (j, c, base, j, c))
                comps.append("%%l_%d_%d" % (j, c))
            if n == 1:
                callargs.append("%s %s" % (base, comps[0]))
            else:
                prev = "undef"
                for c, nm in enumerate(comps):
                    v = "%%g_%d_%d" % (j, c)
                    lines.append("  %s = insertelement %s %s, %s %s, i32 %d"
                                 % (v, ty, prev, base, nm, c))
                    prev = v
                callargs.append("%s %s" % (ty, prev))
        rbase, rn = intrincheck.parse_type(retty)
        lines.append("  %%res = call %s @%s(%s)" % (retty, name, ", ".join(callargs)))
        for k in range(rn):
            if rn > 1:
                lines.append("  %%e_%d = extractelement %s %%res, i32 %d"
                             % (k, retty, k))
                rv = "%%e_%d" % k
            else:
                rv = "%res"
            lines.append("  %%o_%d = getelementptr i8, ptr %%out, i64 %d" % (k, k * 8))
            lines.append("  store %s %s, ptr %%o_%d, align 1" % (rbase, rv, k))
        fn = ("define void @__run(ptr %%in, ptr %%out) {\n%s\n  ret void\n}"
              % "\n".join(lines))
        return fn, slot, rn, rbase

    def run_decl(name, argtys, retty, family, plain, sign, work, triple):
        """Returns (n_cases, [divergences])."""
        a = analyze([name])
        if name not in a["defined"]:
            return 0, [("not emitted", None, None, None)]
        cases = intrincheck.inputs_for(argtys, family)
        signed = True if sign is None else sign
        keep = []
        for case in cases:
            try:
                exp = intrincheck.reference(family, plain, name, case, argtys,
                                            retty, signed)
            except (ValueError, OverflowError, ZeroDivisionError):
                continue
            keep.append((case, exp))
        if not keep:
            return 0, []

        run_fn, in_slots, rn, rbase = build_run_ll(name, argtys, retty)
        module = ('target triple = "%s"\n\n%s\n\n%s\n'
                  % (triple, a["ir"].strip(), run_fn))
        llpath = os.path.join(work, "m.ll")
        with open(llpath, "w") as f:
            f.write(module)
        obj = os.path.join(work, "m.o")
        q = subprocess.run(["llc", "-filetype=obj", "-o", obj, llpath],
                           capture_output=True, text=True)
        if q.returncode:
            tail = (q.stderr or "").strip().splitlines()
            return len(keep), [("llc refused: " + (tail[-1] if tail else "?"),
                                None, None, None)]
        in_size = in_slots * 8
        out_size = rn * 8
        csrc = os.path.join(work, "h.c")
        with open(csrc, "w") as f:
            f.write("#include <stdio.h>\n"
                    "extern void __run(const void *, void *);\n"
                    "int main(void){\n"
                    "  unsigned char in[%d]; unsigned char out[%d];\n"
                    "  while (fread(in,1,%d,stdin)==%d){ __run(in,out);"
                    " fwrite(out,1,%d,stdout);} return 0;\n}\n"
                    % (in_size, out_size, in_size, in_size, out_size))
        exe = os.path.join(work, "h")
        q = subprocess.run(["cc", "-O0", "-o", exe, csrc, obj, "-lm"],
                           capture_output=True, text=True)
        if q.returncode:
            tail = q.stderr.strip().splitlines()
            return len(keep), [("cc refused: " + (tail[-1] if tail else "?"),
                                None, None, None)]
        indata = b"".join(pack_case(case, argtys) for case, _ in keep)
        q = subprocess.run([exe], input=indata, capture_output=True, timeout=120)
        out = q.stdout
        if len(out) != len(keep) * out_size:
            return len(keep), [("harness produced %d bytes, expected %d"
                                % (len(out), len(keep) * out_size),
                                None, None, None)]

        bad = []
        n = 0
        for i, (case, exp) in enumerate(keep):
            caseout = out[i * out_size:(i + 1) * out_size]
            for c in range(rn):
                slot = caseout[c * 8:c * 8 + 8]
                n += 1
                ok, got, want = component_ok(rbase, slot, exp[c])
                if not ok:
                    bad.append((intrincheck.showargs(case), c, got, want))
        return n, bad

    def main(argv):
        decls = collect_decls(argv)
        if not decls:
            print("airdefs: no declarations found (build or pass --decls FILE)",
                  file=sys.stderr)
            return 1
        triple = host_triple()

        # Parse each declare the way intrincheck.main does.
        parsed = []
        for d in decls:
            m = intrincheck.DECL.match(d)
            if not m:
                continue
            retty, name, args = m.group(1), m.group(2), m.group(3)
            op, sign = intrincheck.base_and_sign(name)
            plain = op[5:] if op.startswith("fast_") else op
            argtys = [x.strip() for x in args.split(",")] if args.strip() else []
            family = ("convert" if plain == "convert"
                      else plain if plain in ("dot", "all", "any")
                      else "op" if plain in intrincheck.OPS
                      or (plain, sign) in intrincheck.IOPS
                      else None)
            parsed.append((d, name, retty, argtys, op, plain, sign, family))

        names = [p[1] for p in parsed]
        a = analyze(names)

        # (1) refusals must be observable and must carry no definition.
        print("== refused (approximate, no bit-exact reference) ==")
        for name in sorted(a["refused"]):
            assert ("@%s(" % name) not in a["ir"], \
                "a refused op leaked a definition: %s" % name
            print("  refuse %-28s %s" % (name, _base_of(name)))
        refused_bases = sorted(set(_base_of(n) for n in a["refused"]))
        print("  families refused: %s" % ", ".join(refused_bases))

        # (2) run every emitted definition against the reference.
        print("\n== self-check (host-side, bit-exact vs intrincheck reference) ==")
        total = 0
        diverged = []
        covered = {}
        # In scope: every convert and dot, and the floating-point fmin/fmax/
        # fabs/sqrt/clamp/mix/sign. The integer min/max/clamp share a base name
        # with the float ops (air.clamp.s.i32 vs air.fast_clamp.f32) but are a
        # different family -- intrincheck handles them through IOPS and this file
        # does not define them, so they are not expected to be emitted.
        _floatfam = {"fmin", "fmax", "fabs", "sqrt", "clamp", "mix", "sign"}
        _floattok = re.compile(r"^(?:v\d+)?f(?:16|32|64)$")

        def _in_scope(p):
            plain = p[5]
            if plain in ("convert", "dot"):
                return True
            if plain in _floatfam:
                return bool(_floattok.match(p[1].split(".")[2]))
            return False

        want_defined = [p for p in parsed if _in_scope(p)]
        with tempfile.TemporaryDirectory(prefix="airdefs.") as work:
            for d, name, retty, argtys, op, plain, sign, family in sorted(want_defined,
                                                                          key=lambda p: p[1]):
                if name not in a["defined"]:
                    diverged.append((name, "declared in a defined family but not emitted"))
                    print("  MISS   %-34s not emitted" % name)
                    continue
                n, bad = run_decl(name, argtys, retty, family, plain, sign, work, triple)
                total += n
                covered.setdefault(plain, []).append(_op_suffix(name)
                                                     if plain != "convert"
                                                     else name.split(".", 2)[2])
                if bad:
                    for item in bad[:4]:
                        diverged.append((name, item))
                    print("  WRONG  %-34s %d cases, %d diverge" % (name, n, len(bad)))
                    for args, c, got, want in bad[:4]:
                        if c is None:
                            print("           %s" % args)
                        else:
                            print("           %-28s [%d] got %s want %s"
                                  % (args, c, got, want))
                else:
                    print("  ok     %-34s %d cases" % (name, n))

        print("\n== families x suffixes defined ==")
        for plain in sorted(covered):
            print("  %-9s %s" % (plain, " ".join(sorted(set(covered[plain])))))

        print("\nairdefs: %d defined declarations, %d refused, %d skipped"
              % (len(a["defined"]), len(a["refused"]), len(a["skipped"])))
        print("airdefs: %d cases run, %d divergences" % (total, len(diverged)))
        return 1 if diverged else 0

    sys.exit(main(sys.argv[1:]))
